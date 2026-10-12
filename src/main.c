/**
 * @file main.c
 * @brief kasin-httpd, a small, multi-threaded HTTP/1.1 server for Linux.
 *
 * Design:
 *  - Only libc and Linux syscalls (epoll, accept4, SO_REUSEPORT, mmap). No other deps.
 *  - One worker per thread, each with its own epoll instance and its own
 *    SO_REUSEPORT listening socket. The kernel spreads incoming connections
 *    across workers; workers share nothing, so there are no locks.
 *  - ZERO malloc/free. At startup a single mmap() reserves all memory the server
 *    will ever use. It is carved up once by a linear (bump) arena into, per worker:
 *        [Worker][Conn pool][request buffers][response buffers]
 *    Every connection owns one fixed request buffer and one fixed response
 *    buffer. Connection slots are recycled through an intrusive free list (O(1)
 *    alloc/free, no fragmentation). When the pool is exhausted new connections
 *    are refused rather than growing memory.
 *  - Level-triggered epoll, non-blocking sockets, HTTP keep-alive and
 *    pipelining, idle timeouts. Serves GET/HEAD for static files plus /health.
 *
 * Limits (all compile-time, see CMakeLists.txt): request headers must fit in
 * REQ_BUF_SIZE. File bodies of any size are streamed with sendfile(), so only
 * headers and small built-in bodies use the response buffer.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "db.h"
#include "jwt.h"
#include "sha256.h"
#include "str.h"

// Compile-time configuration.

#ifndef MAX_CONNS_PER_WORKER
#define MAX_CONNS_PER_WORKER 512
#endif
#ifndef REQ_BUF_SIZE
#define REQ_BUF_SIZE 8192
#endif
#ifndef RESP_BUF_SIZE
#define RESP_BUF_SIZE 32768
#endif
#ifndef DB_HEAP_SIZE
#define DB_HEAP_SIZE (32 * 1024 * 1024)  // SQLite heap; BUDDY_MIN_BLOCK times a power of two.
#endif

#define MAX_WORKERS       64
#define EVENT_BATCH       128
#define ACCEPT_BURST      64
#define LISTEN_BACKLOG    1024
#define IDLE_TIMEOUT_SEC  15
#define MAX_PATH_LEN      1024
#define HEAD_RESERVE      512   // Room in front of an in-place body for its head.
#define TOKEN_LIFETIME_SEC (8 * 60 * 60)

static_assert (RESP_BUF_SIZE >= 4 * HEAD_RESERVE, "RESP_BUF_SIZE too small");
static_assert (REQ_BUF_SIZE >= 512, "REQ_BUF_SIZE too small");

#define CACHE_LINE 64

// Connection / worker state.

enum {
    ST_READING,
    ST_WRITING,
};

typedef struct Conn {
    int           fd;  /**< -1 when the slot is free. */
    uint32_t      events;  /**< Epoll events currently registered. */
    int           state;
    int           keepalive;
    uint32_t      req_len;  /**< Bytes buffered in req. */
    uint32_t      resp_pos;  /**< Next byte of resp to send. */
    uint32_t      resp_end;  /**< One past last byte of resp. */
    int           file_fd;  /**< File streamed after resp, or -1. */
    off_t         file_pos;  /**< Next byte of file_fd to send. */
    off_t         file_end;  /**< One past last byte of file_fd to send. */
    int64_t       last_active;  /**< Monotonic seconds. */
    struct Conn  *next_free;
    uint8_t      *req;  /**< REQ_BUF_SIZE bytes, lives in the arena. */
    uint8_t      *resp;  /**< RESP_BUF_SIZE bytes, lives in the arena. */
} Conn;

typedef struct Worker {
    pthread_t           thread;
    int                 id;
    int                 epfd;
    int                 listen_fd;
    Conn               *conns;
    Conn               *free_list;
    struct epoll_event *events;
    Db                  db;  /**< This worker's own database connection. */
} Worker;

static volatile sig_atomic_t STOP_REQUESTED = 0;
static int ROOT_FD = -1;  // Directory static files are served from.
static uint8_t JWT_KEY[SHA256_DIGEST_SIZE];  // Signs session tokens.

// Fixed admin credentials for now.
static Str const ADMIN_USERNAME = STR_INIT("hanna");
static Str const ADMIN_PASSWORD = STR_INIT("koti");

// Small helpers.

static int64_t mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec;
}

static Str mime_for(Str path) {
    static struct {
        Str ext;
        Str mime;
    } const MIME_TABLE[] = {
        {
            .ext = STR_INIT(".html"),
            .mime = STR_INIT("text/html; charset=utf-8"),
        },
        {
            .ext = STR_INIT(".htm"),
            .mime = STR_INIT("text/html; charset=utf-8"),
        },
        {
            .ext = STR_INIT(".css"),
            .mime = STR_INIT("text/css; charset=utf-8"),
        },
        {
            .ext = STR_INIT(".js"),
            .mime = STR_INIT("text/javascript; charset=utf-8"),
        },
        {
            .ext = STR_INIT(".json"),
            .mime = STR_INIT("application/json"),
        },
        {
            .ext = STR_INIT(".txt"),
            .mime = STR_INIT("text/plain; charset=utf-8"),
        },
        {
            .ext = STR_INIT(".svg"),
            .mime = STR_INIT("image/svg+xml"),
        },
        {
            .ext = STR_INIT(".png"),
            .mime = STR_INIT("image/png"),
        },
        {
            .ext = STR_INIT(".jpg"),
            .mime = STR_INIT("image/jpeg"),
        },
        {
            .ext = STR_INIT(".jpeg"),
            .mime = STR_INIT("image/jpeg"),
        },
        {
            .ext = STR_INIT(".gif"),
            .mime = STR_INIT("image/gif"),
        },
        {
            .ext = STR_INIT(".ico"),
            .mime = STR_INIT("image/x-icon"),
        },
        {
            .ext = STR_INIT(".wasm"),
            .mime = STR_INIT("application/wasm"),
        },
    };
    size_t dot = str_rfind_char(path, '.');
    if (dot != STR_NPOS) {
        Str ext = str_slice(path, dot, path.len);
        for (size_t i = 0; i < sizeof MIME_TABLE / sizeof MIME_TABLE[0]; i++) {
            if (str_eq_nocase(ext, MIME_TABLE[i].ext)) {
                return MIME_TABLE[i].mime;
            }
        }
    }
    return STR("application/octet-stream");
}

// Response building.

static Str const JSON_TYPE = STR_INIT("application/json; charset=utf-8");
static Str const NO_STORE = STR_INIT("Cache-Control: no-store\r\n");

static Str status_reason(int status) {
    switch (status) {
    case 200:
        return STR("OK");
    case 201:
        return STR("Created");
    case 400:
        return STR("Bad Request");
    case 401:
        return STR("Unauthorized");
    case 404:
        return STR("Not Found");
    case 405:
        return STR("Method Not Allowed");
    case 409:
        return STR("Conflict");
    case 411:
        return STR("Length Required");
    case 413:
        return STR("Content Too Large");
    case 431:
        return STR("Request Header Fields Too Large");
    case 503:
        return STR("Service Unavailable");
    case 505:
        return STR("HTTP Version Not Supported");
    default:
        return STR("Internal Server Error");
    }
}

/**
 * Appends a status line and the common headers, up to and including the blank line.
 */
static void write_head(Str_Builder *b, Conn const *c, int status, Str ctype,
                       uint64_t content_length, Str extra_headers) {
    str_builder_append(b, STR("HTTP/1.1 "));
    str_builder_append_u64(b, (uint64_t)status);
    str_builder_append(b, STR(" "));
    str_builder_append(b, status_reason(status));
    str_builder_append(b, STR("\r\nServer: kasin\r\nContent-Type: "));
    str_builder_append(b, ctype);
    str_builder_append(b, STR("\r\nContent-Length: "));
    str_builder_append_u64(b, content_length);
    str_builder_append(b, STR("\r\nConnection: "));
    str_builder_append(b, c->keepalive ? STR("keep-alive") : STR("close"));
    str_builder_append(b, STR("\r\n"));
    str_builder_append(b, extra_headers);
    str_builder_append(b, STR("\r\n"));
}

static void respond_simple(Conn *c, int status, Str ctype, Str body, bool head_only,
                           Str extra_headers) {
    Str_Builder b;
    str_builder_init(&b, (char *)c->resp, RESP_BUF_SIZE);
    write_head(&b, c, status, ctype, body.len, extra_headers);
    if (!head_only) {
        str_builder_append(&b, body);
    }
    c->resp_pos = 0;
    c->resp_end = (uint32_t)b.len;  // Bodies passed here are small; overflow only truncates.
}

static void respond_error(Conn *c, int status, bool head_only) {
    char body_buf[96];
    Str_Builder body;
    str_builder_init(&body, body_buf, sizeof body_buf);
    str_builder_append_u64(&body, (uint64_t)status);
    str_builder_append(&body, STR(" "));
    str_builder_append(&body, status_reason(status));
    str_builder_append(&body, STR("\n"));
    Str extra = status == 405 ? STR("Allow: GET, HEAD\r\n") : STR("");
    respond_simple(c, status, STR("text/plain; charset=utf-8"), str_builder_str(&body),
                   head_only, extra);
}

/**
 * Starts a response body that is built in place in the response buffer,
 * behind HEAD_RESERVE bytes kept free for the head. Finish with respond_built().
 */
static void body_begin(Conn *c, Str_Builder *body) {
    str_builder_init(body, (char *)c->resp + HEAD_RESERVE, RESP_BUF_SIZE - HEAD_RESERVE);
}

/**
 * Writes the head right in front of a body started with body_begin(), so the
 * response is one contiguous span and Content-Length is exact.
 */
static void respond_built(Conn *c, int status, Str ctype, Str_Builder const *body,
                          bool head_only, Str extra_headers) {
    if (body->overflow) {
        respond_error(c, 500, head_only);
        return;
    }
    char head_buf[HEAD_RESERVE];
    Str_Builder head;
    str_builder_init(&head, head_buf, sizeof head_buf);
    write_head(&head, c, status, ctype, body->len, extra_headers);
    if (head.overflow) {
        respond_error(c, 500, head_only);
        return;
    }
    c->resp_pos = HEAD_RESERVE - (uint32_t)head.len;
    memcpy(c->resp + c->resp_pos, head.data, head.len);
    c->resp_end = HEAD_RESERVE + (head_only ? 0 : (uint32_t)body->len);
}

/**
 * Responds with {"error": message}. The message must not need JSON escaping.
 */
static void respond_json_error(Conn *c, int status, Str message, Str extra_headers) {
    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"error\":\""));
    str_builder_append(&body, message);
    str_builder_append(&body, STR("\"}"));
    respond_built(c, status, JSON_TYPE, &body, false, extra_headers);
}

static void respond_db_error(Conn *c, Db_Status status) {
    switch (status) {
    case DB_NOT_FOUND:
        respond_json_error(c, 404, STR("not found"), NO_STORE);
        break;
    case DB_INVALID:
        respond_json_error(c, 400, STR("invalid request body"), NO_STORE);
        break;
    case DB_CONFLICT:
        respond_json_error(c, 409, STR("conflict with existing data"), NO_STORE);
        break;
    case DB_BUSY:
        respond_json_error(c, 503, STR("database busy"),
                           STR("Cache-Control: no-store\r\nRetry-After: 1\r\n"));
        break;
    case DB_TOO_LARGE:
        respond_json_error(c, 500, STR("response too large"), NO_STORE);
        break;
    default:
        respond_json_error(c, 500, STR("database error"), NO_STORE);
        break;
    }
}

static Str const WELCOME = STR_INIT(
    "<!doctype html><meta charset=utf-8><title>kasin-httpd</title>"
    "<h1>kasin-httpd is running</h1><p>Put files in the served directory.</p>\n");

/**
 * Opens a path under the docroot and stats it.
 *
 * @return The descriptor, or -1 with errno set.
 */
static int open_in_root(Str rel_path, struct stat *st) {
    char rel_path_z[MAX_PATH_LEN];  // openat() needs a NUL-terminated path.
    if (ROOT_FD < 0) {
        errno = ENOENT;
        return -1;
    }
    if (!str_to_cstr(rel_path, rel_path_z, sizeof rel_path_z)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = openat(ROOT_FD, rel_path_z, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, st) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/**
 * Writes the response header into resp and keeps the file open so its body can
 * be streamed with sendfile(), whatever its size.
 *
 * Clean URLs are supported: a directory serves its index.html, and a missing
 * path without an extension falls back to the same path plus ".html".
 */
static void serve_file(Conn *c, Str rel_path, bool head_only, bool is_root) {
    char path_buf[MAX_PATH_LEN];
    Str_Builder path;
    str_builder_init(&path, path_buf, sizeof path_buf);
    str_builder_append(&path, rel_path);

    struct stat st;
    int fd = open_in_root(str_builder_str(&path), &st);
    size_t slash = str_rfind_char(rel_path, '/');
    Str last_segment = slash == STR_NPOS ? rel_path : str_slice(rel_path, slash + 1, rel_path.len);
    if (fd >= 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        str_builder_append(&path, STR("/index.html"));
        fd = open_in_root(str_builder_str(&path), &st);
    } else if (fd < 0 && errno == ENOENT && str_find_char(last_segment, '.') == STR_NPOS) {
        str_builder_append(&path, STR(".html"));
        fd = open_in_root(str_builder_str(&path), &st);
    }

    if (fd < 0) {
        if (is_root) {  // No index.html yet: show a built-in page.
            respond_simple(c, 200, STR("text/html; charset=utf-8"), WELCOME, head_only, STR(""));
        } else {
            bool not_found = path.overflow || errno == ENOENT || errno == ENOTDIR;
            respond_error(c, not_found ? 404 : 500, head_only);
        }
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        respond_error(c, 404, head_only);
        return;
    }

    Str_Builder hdr;
    str_builder_init(&hdr, (char *)c->resp, RESP_BUF_SIZE);
    write_head(&hdr, c, 200, mime_for(str_builder_str(&path)), (uint64_t)st.st_size, STR(""));
    if (hdr.overflow) {
        close(fd);
        respond_error(c, 500, head_only);
        return;
    }
    c->resp_pos = 0;
    c->resp_end = (uint32_t)hdr.len;

    if (head_only || st.st_size == 0) {
        close(fd);
        return;
    }
    c->file_fd = fd;  // Streamed by conn_advance() once the header is out.
    c->file_pos = 0;
    c->file_end = st.st_size;
}

// Request parsing.

typedef struct {
    bool     close;
    bool     keepalive;
    bool     chunked;
    bool     bad_content_length;
    uint64_t content_length;
    Str      authorization;
} Request_Headers;

typedef struct {
    Str                    method;
    Str                    path;  /**< Target without query string or fragment. */
    Str                    body;
    bool                   head_only;
    Request_Headers const *headers;
} Request;

/**
 * Scans header lines (CRLF separated, without the final blank line) for the
 * headers the server acts on.
 */
static void scan_headers(Str headers, Request_Headers *h) {
    Str rest = headers;
    while (rest.len > 0) {
        Str line;
        if (!str_split_once(rest, STR("\r\n"), &line, &rest)) {
            line = rest;
            rest = str_make(rest.data + rest.len, 0);
        }
        Str name;
        Str value;
        if (!str_split_once(line, STR(":"), &name, &value)) {
            continue;
        }
        value = str_trim(value);
        if (str_eq_nocase(name, STR("connection"))) {
            if (str_contains_nocase(value, STR("close"))) {
                h->close = true;
            } else if (str_contains_nocase(value, STR("keep-alive"))) {
                h->keepalive = true;
            }
        } else if (str_eq_nocase(name, STR("content-length"))) {
            if (!str_to_u64(value, &h->content_length)) {
                h->bad_content_length = true;
            }
        } else if (str_eq_nocase(name, STR("transfer-encoding"))) {
            h->chunked = true;  // Any transfer coding: the body length is unknown.
        } else if (str_eq_nocase(name, STR("authorization"))) {
            h->authorization = value;
        }
    }
}

// API.

static int64_t unix_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

static bool authenticate(Request const *r, Jwt_Claims *claims) {
    Str auth = r->headers->authorization;
    Str scheme = STR("Bearer ");
    if (auth.len <= scheme.len || !str_eq_nocase(str_slice(auth, 0, scheme.len), scheme)) {
        return false;
    }
    Str token = str_trim(str_slice(auth, scheme.len, auth.len));
    return jwt_verify(token, JWT_KEY, sizeof JWT_KEY, unix_now(), claims);
}

static void respond_unauthorized(Conn *c) {
    respond_json_error(c, 401, STR("login required"),
                       STR("Cache-Control: no-store\r\nWWW-Authenticate: Bearer\r\n"));
}

static void api_login(Worker *w, Conn *c, Request const *r) {
    char username_buf[128];
    char password_buf[128];
    Str_Builder username;
    Str_Builder password;
    str_builder_init(&username, username_buf, sizeof username_buf);
    str_builder_init(&password, password_buf, sizeof password_buf);

    Db_Status status = db_parse_login(&w->db, r->body, &username, &password);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    // Compare both fields every time so timing does not reveal which was wrong.
    bool user_ok = str_eq_constant_time(str_builder_str(&username), ADMIN_USERNAME);
    bool password_ok = str_eq_constant_time(str_builder_str(&password), ADMIN_PASSWORD);
    if (!(user_ok & password_ok)) {
        respond_json_error(c, 401, STR("invalid username or password"),
                           STR("Cache-Control: no-store\r\nWWW-Authenticate: Bearer\r\n"));
        return;
    }

    int64_t now = unix_now();
    int64_t expires_at = now + TOKEN_LIFETIME_SEC;
    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"token\":\""));
    if (!jwt_issue(&body, ADMIN_USERNAME, now, expires_at, JWT_KEY, sizeof JWT_KEY)) {
        respond_json_error(c, 500, STR("cannot issue token"), NO_STORE);
        return;
    }
    str_builder_append(&body, STR("\",\"expires_at\":"));
    str_builder_append_u64(&body, (uint64_t)expires_at);
    str_builder_append(&body, STR("}"));
    respond_built(c, 200, JSON_TYPE, &body, false, NO_STORE);
}

static void api_session(Conn *c, Request const *r) {
    Jwt_Claims claims;
    if (!authenticate(r, &claims)) {
        respond_unauthorized(c);
        return;
    }
    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"username\":\""));
    str_builder_append(&body, str_make(claims.subject, claims.subject_len));
    str_builder_append(&body, STR("\",\"expires_at\":"));
    str_builder_append_u64(&body, (uint64_t)claims.expires_at);
    str_builder_append(&body, STR("}"));
    respond_built(c, 200, JSON_TYPE, &body, r->head_only, NO_STORE);
}

/**
 * A collection exposed as /api/{name} and /api/{name}/{id}. Reads are public;
 * writes need a valid session.
 */
typedef struct {
    Str       path;  /**< Collection path, e.g. "/api/recipes". */
    Db_Status (*list)(Db *db, Str_Builder *out);
    Db_Status (*get)(Db *db, int64_t id, Str_Builder *out);
    Db_Status (*create)(Db *db, Str json, int64_t *id);
    Db_Status (*update)(Db *db, int64_t id, Str json);
    Db_Status (*remove)(Db *db, int64_t id);
} Resource;

static Resource const RESOURCES[] = {
    {
        .path = STR_INIT("/api/recipes"),
        .list = db_list_recipes_json,
        .get = db_get_recipe_json,
        .create = db_create_recipe,
        .update = db_update_recipe,
        .remove = db_delete_recipe,
    },
    {
        .path = STR_INIT("/api/categories"),
        .list = db_list_categories_json,
        .get = db_get_category_json,
        .create = db_create_category,
        .update = db_update_category,
        .remove = db_delete_category,
    },
};

static void respond_id(Conn *c, int status, int64_t id) {
    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"id\":"));
    str_builder_append_u64(&body, (uint64_t)id);
    str_builder_append(&body, STR("}"));
    respond_built(c, status, JSON_TYPE, &body, false, NO_STORE);
}

static void api_list(Worker *w, Conn *c, Request const *r, Resource const *res) {
    Str_Builder body;
    body_begin(c, &body);
    Db_Status status = res->list(&w->db, &body);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_built(c, 200, JSON_TYPE, &body, r->head_only, NO_STORE);
}

static void api_get(Worker *w, Conn *c, Request const *r, Resource const *res, int64_t id) {
    Str_Builder body;
    body_begin(c, &body);
    Db_Status status = res->get(&w->db, id, &body);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_built(c, 200, JSON_TYPE, &body, r->head_only, NO_STORE);
}

static void api_create(Worker *w, Conn *c, Request const *r, Resource const *res) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    int64_t id;
    Db_Status status = res->create(&w->db, r->body, &id);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_id(c, 201, id);
}

static void api_update(Worker *w, Conn *c, Request const *r, Resource const *res, int64_t id) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    Db_Status status = res->update(&w->db, id, r->body);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_id(c, 200, id);
}

static void api_delete(Worker *w, Conn *c, Request const *r, Resource const *res, int64_t id) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    Db_Status status = res->remove(&w->db, id);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_id(c, 200, id);
}

static void respond_method_not_allowed(Conn *c, Str allow_header) {
    respond_json_error(c, 405, STR("method not allowed"), allow_header);
}

/**
 * Handles a request whose path is a resource's collection or one of its items.
 *
 * @return False when the path belongs to neither.
 */
static bool handle_resource(Worker *w, Conn *c, Request const *r, Resource const *res) {
    bool is_get = str_eq(r->method, STR("GET")) || r->head_only;
    bool is_post = str_eq(r->method, STR("POST"));
    bool is_put = str_eq(r->method, STR("PUT"));
    bool is_delete = str_eq(r->method, STR("DELETE"));

    if (str_eq(r->path, res->path)) {
        if (is_get) {
            api_list(w, c, r, res);
        } else if (is_post) {
            api_create(w, c, r, res);
        } else {
            respond_method_not_allowed(c, STR("Cache-Control: no-store\r\nAllow: GET, HEAD, POST\r\n"));
        }
        return true;
    }

    Str item = str_slice(r->path, res->path.len, r->path.len);
    if (!str_starts_with(r->path, res->path) || !str_starts_with(item, STR("/"))) {
        return false;
    }
    uint64_t id;
    if (!str_to_u64(str_slice(item, 1, item.len), &id) || id == 0 || id > INT64_MAX) {
        respond_db_error(c, DB_NOT_FOUND);
    } else if (is_get) {
        api_get(w, c, r, res, (int64_t)id);
    } else if (is_put) {
        api_update(w, c, r, res, (int64_t)id);
    } else if (is_delete) {
        api_delete(w, c, r, res, (int64_t)id);
    } else {
        respond_method_not_allowed(c, STR("Cache-Control: no-store\r\nAllow: GET, HEAD, PUT, DELETE\r\n"));
    }
    return true;
}

/**
 * Routes requests under /api/.
 *
 *   POST   /api/login             {username, password} -> {token, expires_at}
 *   GET    /api/session           (auth) -> {username, expires_at}
 *
 *   GET    /api/recipes           -> [{id, name, image, description, category_id, category}, ...]
 *   POST   /api/recipes           (auth) {name, image, description, category_id} -> 201 {id}
 *   GET    /api/recipes/{id}      -> {id, name, image, description, category_id, category}
 *   PUT    /api/recipes/{id}      (auth) {name, image, description, category_id} -> {id}
 *   DELETE /api/recipes/{id}      (auth) -> {id}
 *
 *   GET    /api/categories        -> [{id, name, recipe_count}, ...] alphabetically
 *   POST   /api/categories        (auth) {name} -> 201 {id}, or 409 if the name is taken
 *   GET    /api/categories/{id}   -> {id, name, recipe_count}
 *   PUT    /api/categories/{id}   (auth) {name} -> {id}, or 409 if the name is taken
 *   DELETE /api/categories/{id}   (auth) -> {id}, or 409 while recipes use it
 */
static void handle_api(Worker *w, Conn *c, Request const *r) {
    if (str_eq(r->path, STR("/api/login"))) {
        if (str_eq(r->method, STR("POST"))) {
            api_login(w, c, r);
        } else {
            respond_method_not_allowed(c, STR("Cache-Control: no-store\r\nAllow: POST\r\n"));
        }
        return;
    }
    if (str_eq(r->path, STR("/api/session"))) {
        if (str_eq(r->method, STR("GET")) || r->head_only) {
            api_session(c, r);
        } else {
            respond_method_not_allowed(c, STR("Cache-Control: no-store\r\nAllow: GET, HEAD\r\n"));
        }
        return;
    }
    for (size_t i = 0; i < sizeof RESOURCES / sizeof RESOURCES[0]; i++) {
        if (handle_resource(w, c, r, &RESOURCES[i])) {
            return;
        }
    }
    respond_json_error(c, 404, STR("no such endpoint"), NO_STORE);
}

// Routing.

/**
 * Returns 1 and fills c->resp if a full request (head and body) is buffered, else 0.
 */
static int build_response(Worker *w, Conn *c) {
    Str buffered = str_make((char const *)c->req, c->req_len);
    Str head;
    Str after_head;
    if (!str_split_once(buffered, STR("\r\n\r\n"), &head, &after_head)) {
        return 0;
    }
    size_t head_len = buffered.len - after_head.len;
    size_t consumed = head_len;

    c->keepalive = 0;  // Errors below close.

    Str request_line = head;
    Str headers = STR("");
    str_split_once(head, STR("\r\n"), &request_line, &headers);

    Str method;
    Str after_method;
    Str target;
    Str version;
    if (!str_split_once(request_line, STR(" "), &method, &after_method) ||
        !str_split_once(after_method, STR(" "), &target, &version)) {
        respond_error(c, 400, false);
        goto done;
    }

    bool http11 = str_eq(version, STR("HTTP/1.1"));
    bool http10 = str_eq(version, STR("HTTP/1.0"));
    if (!http11 && !http10) {
        respond_error(c, 505, false);
        goto done;
    }

    Request_Headers h = {
        .close = false,
        .keepalive = false,
        .chunked = false,
        .bad_content_length = false,
        .content_length = 0,
        .authorization = STR(""),
    };
    scan_headers(headers, &h);
    if (h.bad_content_length) {
        respond_error(c, 400, false);
        goto done;
    }
    if (h.chunked) {
        respond_error(c, 411, false);
        goto done;
    }
    if (h.content_length > REQ_BUF_SIZE - head_len) {  // The body can never fit.
        respond_error(c, 413, false);
        goto done;
    }
    size_t request_len = head_len + (size_t)h.content_length;
    if (buffered.len < request_len) {
        return 0;  // Wait for the rest of the body.
    }
    consumed = request_len;
    c->keepalive = http11 ? !h.close : h.keepalive;

    Request r = {
        .method = method,
        .path = target,
        .body = str_slice(buffered, head_len, request_len),
        .head_only = str_eq(method, STR("HEAD")),
        .headers = &h,
    };

    // Strip query string / fragment.
    size_t query = str_find_any(r.path, STR("?#"));
    if (query != STR_NPOS) {
        r.path = str_slice(r.path, 0, query);
    }

    /*
     * No percent-decoding: reject anything that could escape the root.
     * The set below is length-based, so it includes the NUL byte too.
     */
    if (r.path.len == 0 || r.path.data[0] != '/' || r.path.len >= MAX_PATH_LEN - 16 ||
        str_find_any(r.path, STR("%\\\0")) != STR_NPOS || str_contains(r.path, STR(".."))) {
        respond_error(c, 400, r.head_only);
        goto done;
    }

    if (str_starts_with(r.path, STR("/api/"))) {
        handle_api(w, c, &r);
        goto done;
    }

    if (!str_eq(method, STR("GET")) && !r.head_only) {
        respond_error(c, 405, false);
        goto done;
    }

    if (str_eq(r.path, STR("/health"))) {
        respond_simple(c, 200, STR("text/plain; charset=utf-8"), STR("ok\n"), r.head_only, STR(""));
        goto done;
    }

    Str rel = str_slice(r.path, 1, r.path.len);  // Drop leading '/'.
    char path_buf[MAX_PATH_LEN];
    Str_Builder path;
    str_builder_init(&path, path_buf, sizeof path_buf);
    str_builder_append(&path, rel);
    if (rel.len == 0 || str_ends_with(rel, STR("/"))) {
        str_builder_append(&path, STR("index.html"));
    }

    serve_file(c, str_builder_str(&path), r.head_only, rel.len == 0);

done:
    // The request is fully handled; keep any pipelined bytes behind it.
    c->req_len -= (uint32_t)consumed;
    memmove(c->req, c->req + consumed, c->req_len);
    return 1;
}

// Connection lifecycle.

static void conn_want(Worker *w, Conn *c, uint32_t events) {
    if (c->events == events) {
        return;
    }
    struct epoll_event ev = {
        .events = events,
        .data.ptr = c,
    };
    epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
    c->events = events;
}

static void conn_close(Worker *w, Conn *c) {
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    close(c->fd);  // Also removes it from the epoll set.
    c->fd = -1;
    c->next_free = w->free_list;
    w->free_list = c;
}

/**
 * Drives a connection forward until it needs more input, more socket space, or dies.
 */
static void conn_advance(Worker *w, Conn *c) {
    for (;;) {
        if (c->state == ST_READING) {
            if (!build_response(w, c)) {
                if (c->req_len >= REQ_BUF_SIZE) {  // Headers too large.
                    c->keepalive = 0;
                    c->req_len = 0;
                    respond_error(c, 431, 0);
                } else {
                    conn_want(w, c, EPOLLIN);
                    return;
                }
            }
            c->state = ST_WRITING;
        }

        while (c->resp_pos < c->resp_end) {
            ssize_t n = send(c->fd, c->resp + c->resp_pos, c->resp_end - c->resp_pos,
                             MSG_NOSIGNAL);
            if (n > 0) {
                c->resp_pos += (uint32_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                conn_want(w, c, EPOLLOUT);
                return;
            }
            conn_close(w, c);
            return;
        }

        // Zero-copy body: the kernel moves file pages straight to the socket.
        while (c->file_fd >= 0 && c->file_pos < c->file_end) {
            ssize_t n = sendfile(c->fd, c->file_fd, &c->file_pos,
                                 (size_t)(c->file_end - c->file_pos));
            if (n > 0) {
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                conn_want(w, c, EPOLLOUT);
                return;
            }
            // Error, or the file shrank: Content-Length can no longer be honored.
            conn_close(w, c);
            return;
        }
        if (c->file_fd >= 0) {
            close(c->file_fd);
            c->file_fd = -1;
        }

        if (!c->keepalive) {
            conn_close(w, c);
            return;
        }

        c->state = ST_READING;
        if (c->req_len == 0) {
            conn_want(w, c, EPOLLIN);
            return;
        }
        // Else: a pipelined request is already buffered, loop and handle it.
    }
}

static void conn_on_readable(Worker *w, Conn *c, int64_t now) {
    if (c->req_len < REQ_BUF_SIZE) {
        ssize_t n = recv(c->fd, c->req + c->req_len, REQ_BUF_SIZE - c->req_len, 0);
        if (n > 0) {
            c->req_len += (uint32_t)n;
        } else if (n == 0) {
            conn_close(w, c);
            return;
        } else if (errno == EAGAIN || errno == EINTR) {
            return;
        } else {
            conn_close(w, c);
            return;
        }
    }
    c->last_active = now;
    conn_advance(w, c);
}

static void accept_all(Worker *w, int64_t now) {
    for (int i = 0; i < ACCEPT_BURST; i++) {
        int fd = accept4(w->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;  // EAGAIN or transient error.
        }

        Conn *c = w->free_list;
        if (!c) {
            close(fd);  // Pool exhausted: shed load.
            continue;
        }
        w->free_list = c->next_free;

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        c->fd = fd;
        c->state = ST_READING;
        c->keepalive = 0;
        c->req_len = c->resp_pos = c->resp_end = 0;
        c->file_fd = -1;
        c->file_pos = c->file_end = 0;
        c->last_active = now;
        c->events = EPOLLIN;

        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.ptr = c,
        };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            conn_close(w, c);
        }
    }
}

static void sweep_idle(Worker *w, int64_t now) {
    for (int i = 0; i < MAX_CONNS_PER_WORKER; i++) {
        Conn *c = &w->conns[i];
        if (c->fd >= 0 && now - c->last_active > IDLE_TIMEOUT_SEC) {
            conn_close(w, c);
        }
    }
}

static void *worker_main(void *arg) {
    Worker *w = arg;
    int64_t last_sweep = mono_sec();

    while (!STOP_REQUESTED) {
        int n = epoll_wait(w->epfd, w->events, EVENT_BATCH, 1000);
        int64_t now = mono_sec();

        for (int i = 0; i < n; i++) {
            uint32_t ev = w->events[i].events;
            Conn *c = w->events[i].data.ptr;
            if (!c) {
                accept_all(w, now);
                continue;
            }

            if (ev & (EPOLLERR | EPOLLHUP)) {
                conn_close(w, c);
                continue;
            }
            c->last_active = now;
            if (c->state == ST_WRITING && (ev & EPOLLOUT)) {
                conn_advance(w, c);
            } else if (c->state == ST_READING && (ev & EPOLLIN)) {
                conn_on_readable(w, c, now);
            }
        }

        if (now != last_sweep) {
            sweep_idle(w, now);
            last_sweep = now;
        }
    }
    return NULL;
}

// Startup.

static int make_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(fd, LISTEN_BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void on_signal(int sig) {
    (void)sig;
    STOP_REQUESTED = 1;
}

static void usage(char const *argv0) {
    fprintf(stderr, "usage: %s [-p port] [-t threads] [-r docroot]\n", argv0);
}

int main(int argc, char **argv) {
    int port = 8080;
    // CHECK What is _SC_NPROCESSORS_ONLN?
    long nworkers = sysconf(_SC_NPROCESSORS_ONLN);
    
    char const *root = NULL;
    char const *db_path = "kotoa.db";
    int opt;
    // CHECK What does getopt do?
    while ((opt = getopt(argc, argv, "p:t:r:d:h")) != -1) {
        uint64_t value;
        switch (opt) {
        case 'p':
            if (!str_to_u64(str_from_cstr(optarg), &value) || value > 65535) {
                usage(argv[0]);
                return EX_USAGE;
            }
            port = (int)value;
            break;
        case 't':
            if (!str_to_u64(str_from_cstr(optarg), &value) || value > MAX_WORKERS) {
                usage(argv[0]);
                return EX_USAGE;
            }
            nworkers = (long)value;
            break;
        case 'r':
            root = optarg;
            break;
        case 'd':
            db_path = optarg;
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? EXIT_SUCCESS : EX_USAGE;
        }
    }
    if (port <= 0 || port > 65535) {
        usage(argv[0]);
        return EX_USAGE;
    }
    if (nworkers < 1) {
        nworkers = 1;
    }
    if (nworkers > MAX_WORKERS) {
        nworkers = MAX_WORKERS;
    }

    if (root) {
        ROOT_FD = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (ROOT_FD < 0) {
            fprintf(stderr, "warning: cannot open docroot '%s': %s\n", root, strerror(errno));
        }
    }

    // The one and only memory reservation.
    size_t per_worker = arena_align_up(sizeof (Worker), CACHE_LINE)
                      + arena_align_up(sizeof (struct epoll_event) * EVENT_BATCH, CACHE_LINE)
                      + sizeof (Conn) * MAX_CONNS_PER_WORKER
                      + (size_t)MAX_CONNS_PER_WORKER * REQ_BUF_SIZE
                      + (size_t)MAX_CONNS_PER_WORKER * RESP_BUF_SIZE;
    size_t total = arena_align_up(per_worker * (size_t)nworkers + DB_HEAP_SIZE + CACHE_LINE * 8,
                                  4096);

    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return EX_OSERR;
    }
    madvise(mem, total, MADV_HUGEPAGE);  // Best effort.

    Arena arena;
    arena_init(&arena, mem, total);

    // Session tokens survive restarts only when the secret is configured.
    char const *jwt_secret = getenv("KASIN_JWT_SECRET");
    if (jwt_secret && jwt_secret[0] != '\0') {
        Str secret = str_from_cstr(jwt_secret);
        Sha256 key_hash;
        sha256_init(&key_hash);
        sha256_update(&key_hash, secret.data, secret.len);
        sha256_final(&key_hash, JWT_KEY);
    } else if (getrandom(JWT_KEY, sizeof JWT_KEY, 0) != (ssize_t)sizeof JWT_KEY) {
        perror("getrandom");
        return EX_OSERR;
    }

    // SQLite gets a fixed heap out of the same reservation.
    void *db_heap = arena_alloc(&arena, DB_HEAP_SIZE, CACHE_LINE);
    if (!db_heap) {
        fputs("arena exhausted\n", stderr);
        return EX_SOFTWARE;
    }
    if (!db_global_init(db_heap, DB_HEAP_SIZE) || !db_prepare_file(db_path)) {
        return EX_SOFTWARE;
    }

    Worker *workers[MAX_WORKERS];
    for (long i = 0; i < nworkers; i++) {
        Worker *w = arena_alloc(&arena, sizeof *w, CACHE_LINE);
        if (!w) {
            fputs("arena exhausted\n", stderr);
            return EX_SOFTWARE;
        }
        w->events = arena_alloc(&arena, sizeof (struct epoll_event) * EVENT_BATCH, CACHE_LINE);
        w->conns = arena_alloc(&arena, sizeof (Conn) * MAX_CONNS_PER_WORKER, CACHE_LINE);
        uint8_t *reqs = arena_alloc(&arena, (size_t)MAX_CONNS_PER_WORKER * REQ_BUF_SIZE, CACHE_LINE);
        uint8_t *resps = arena_alloc(&arena, (size_t)MAX_CONNS_PER_WORKER * RESP_BUF_SIZE, CACHE_LINE);
        if (!w->events || !w->conns || !reqs || !resps) {
            fputs("arena exhausted\n", stderr);
            return EX_SOFTWARE;
        }

        w->id = (int)i;
        w->free_list = NULL;
        for (int j = MAX_CONNS_PER_WORKER - 1; j >= 0; j--) {  // Build free list.
            Conn *c = &w->conns[j];
            c->fd = -1;
            c->req = reqs + (size_t)j * REQ_BUF_SIZE;
            c->resp = resps + (size_t)j * RESP_BUF_SIZE;
            c->next_free = w->free_list;
            w->free_list = c;
        }

        w->listen_fd = make_listener((uint16_t)port);
        if (w->listen_fd < 0) {
            perror("listen");
            return EX_OSERR;
        }
        w->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (w->epfd < 0) {
            perror("epoll_create1");
            return EX_OSERR;
        }
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.ptr = NULL,
        };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listen_fd, &ev) < 0) {
            perror("epoll_ctl");
            return EX_OSERR;
        }
        if (!db_open(&w->db, db_path)) {
            return EX_SOFTWARE;
        }
        workers[i] = w;
    }

    // No SA_RESTART so signals wake epoll_wait.
    struct sigaction sa = {
        .sa_handler = on_signal,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    printf("kasin-httpd: port %d, %ld workers, %d conns/worker, req %d B, resp %d B, "
           "reserved %.1f MiB (SQLite heap %d MiB), docroot %s, database %s\n",
           port, nworkers, MAX_CONNS_PER_WORKER, REQ_BUF_SIZE, RESP_BUF_SIZE,
           (double)total / (1024.0 * 1024.0), DB_HEAP_SIZE / (1024 * 1024),
           root ? root : "(none)", db_path);
    fflush(stdout);

    long const worker_count = nworkers;
    for (long i = 1; i < nworkers; i++) {
        if (pthread_create(&workers[i]->thread, NULL, worker_main, workers[i]) != 0) {
            perror("pthread_create");
            STOP_REQUESTED = 1;
            nworkers = i;
            break;
        }
    }

    worker_main(workers[0]);  // Main thread is worker 0.

    for (long i = 1; i < nworkers; i++) {
        pthread_join(workers[i]->thread, NULL);
    }
    for (long i = 0; i < worker_count; i++) {
        db_close(&workers[i]->db);
    }
    puts("kasin-httpd: shut down");
    return EXIT_SUCCESS;
}
