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
#include <arpa/inet.h>
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
#include <sys/syscall.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "db.h"
#include "geoip.h"
#include "ip.h"
#include "jwt.h"
#include "login_guard.h"
#include "password.h"
#include "sha256.h"
#include "str.h"

// Compile-time configuration. The first six can be set with -D (see CMakeLists.txt), so they
// are macros that the build may override.

#ifndef MAX_CONNS_PER_WORKER
/** Connection slots per worker thread. */
#define MAX_CONNS_PER_WORKER 512
#endif
#ifndef REQ_BUF_SIZE
/** Request buffer of each connection, in bytes: the head and a small body. */
#define REQ_BUF_SIZE 8192
#endif
#ifndef RESP_BUF_SIZE
/** Response buffer of each connection, in bytes. */
#define RESP_BUF_SIZE 32768
#endif
#ifndef BIG_BUF_SIZE
/** A large request body or response, in bytes: uploads, long recipes. */
#define BIG_BUF_SIZE (2 * 1024 * 1024)
#endif
#ifndef BIG_BUFS_PER_WORKER
/** Large buffers per worker thread. */
#define BIG_BUFS_PER_WORKER 2
#endif
#ifndef DB_HEAP_SIZE
/** The SQLite heap, in bytes: BUDDY_MIN_BLOCK times a power of two. */
#define DB_HEAP_SIZE (32 * 1024 * 1024)
#endif

enum {
    /** Most worker threads. */
    MAX_WORKERS = 64,
    /** Epoll events taken from the kernel at once. */
    EVENT_BATCH = 128,
    /** Connections accepted in one go. */
    ACCEPT_BURST = 64,
    /** Length of the queue of connections that are not accepted yet. */
    LISTEN_BACKLOG = 1024,
    /** A connection idle this long is closed, in seconds. */
    IDLE_TIMEOUT_SEC = 15,
    /** Longest request path or file path, in bytes. */
    MAX_PATH_LEN = 1024,
    /** Room in front of an in-place body for its head, in bytes (about 700 are used at most). */
    HEAD_RESERVE = 1024,
    /** How long a session token is valid for, in seconds (8 hours). */
    TOKEN_LIFETIME_SEC = 8 * 60 * 60,
    /** Alignment of buffers, in bytes: one cache line. */
    CACHE_LINE = 64,
    /** Size of a memory page, in bytes. */
    PAGE_SIZE = 4096,
    /** Most blocked addresses shown in the login report. */
    MAX_REPORTED_BLOCKS = 500,
};

static_assert(RESP_BUF_SIZE >= 4 * HEAD_RESERVE, "RESP_BUF_SIZE too small");
static_assert(REQ_BUF_SIZE >= 512, "REQ_BUF_SIZE too small");
static_assert(BIG_BUF_SIZE >= REQ_BUF_SIZE && BIG_BUF_SIZE >= RESP_BUF_SIZE,
              "BIG_BUF_SIZE too small");
static_assert(BIG_BUFS_PER_WORKER >= 1, "BIG_BUFS_PER_WORKER must be at least 1");

// Connection / worker state.

typedef enum {
    /** Waiting for a whole request. */
    ST_READING,
    /** Sending the response. */
    ST_WRITING,
} Conn_State;

typedef struct Conn {
    int           fd;  /**< -1 when the slot is free. */
    uint32_t      events;  /**< Epoll events currently registered. */
    Conn_State    state;
    bool          keepalive;  /**< Whether the connection stays open after the response. */
    uint32_t      req_len;  /**< Bytes buffered in in. */
    uint32_t      resp_pos;  /**< Next byte of out to send. */
    uint32_t      resp_end;  /**< One past last byte of out. */
    int           file_fd;  /**< File streamed after resp, or -1. */
    off_t         file_pos;  /**< Next byte of file_fd to send. */
    off_t         file_end;  /**< One past last byte of file_fd to send. */
    int64_t       last_active;  /**< Monotonic seconds. */
    Ip_Address    peer;  /**< The address the connection comes from. */
    struct Conn  *next_free;
    uint8_t      *req;  /**< REQ_BUF_SIZE bytes, lives in the arena. */
    uint8_t      *resp;  /**< RESP_BUF_SIZE bytes, lives in the arena. */
    uint8_t      *in;  /**< Request bytes: req, or big while a large body arrives. */
    uint32_t      in_cap;
    uint8_t      *out;  /**< Response bytes: resp, or big for a large response. */
    uint32_t      out_cap;
    uint8_t      *big;  /**< Large buffer borrowed from the worker, or NULL. */
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
    uint8_t            *big_free[BIG_BUFS_PER_WORKER];  /**< Large buffers not lent out. */
    int                 big_free_count;
} Worker;

static volatile sig_atomic_t STOP_REQUESTED = 0;
static int ROOT_FD = -1;  // Directory static files are served from.
static int UPLOADS_FD = -1;  // Directory uploaded images are stored in.
static uint8_t JWT_KEY[SHA256_DIGEST_SIZE];  // Signs session tokens.

static Str const ADMIN_USERNAME = STR_INIT("hanna");

/*
 * The admin password, as a salted, slow hash read from KASIN_ADMIN_PASSWORD_HASH
 * at startup (see password.h and kasin-passwd). Without it nobody can log in.
 */
static Password_Hash ADMIN_PASSWORD_HASH;
static bool LOGIN_ENABLED = false;

static Login_Guard LOGIN_GUARD;  // Brute-force protection, shared by all workers.
static Geoip GEOIP;  // Countries of addresses; empty when no country file is given.

/*
 * Whether the server runs behind a proxy that adds the client's address to
 * X-Forwarded-For (KASIN_TRUST_PROXY=1), like the ingress of Azure Container
 * Apps. Otherwise the header is ignored, since any client can send one.
 */
static bool TRUST_PROXY = false;

/*
 * Large buffers. A few per worker are reserved at startup and lent to one
 * connection at a time, for request bodies or responses that do not fit the
 * connection's own small buffers. A connection holds at most one.
 */

static bool big_acquire(Worker *w, Conn *c) {
    if (c->big) {
        return false;
    }
    if (w->big_free_count == 0) {
        return false;
    }
    c->big = w->big_free[--w->big_free_count];
    return true;
}

// Returns the large buffer and points the connection back at its own buffers.
static void big_release(Worker *w, Conn *c) {
    if (!c->big) {
        return;
    }
    w->big_free[w->big_free_count++] = c->big;
    c->big = NULL;
    c->in = c->req;
    c->in_cap = REQ_BUF_SIZE;
    c->out = c->resp;
    c->out_cap = RESP_BUF_SIZE;
}

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
            .ext = STR_INIT(".webp"),
            .mime = STR_INIT("image/webp"),
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

// Headers of 405 responses: which methods the path does answer.
static Str const ALLOW_GET_HEAD = STR_INIT(
    "Cache-Control: no-store\r\nAllow: GET, HEAD\r\n");
static Str const ALLOW_GET_HEAD_POST = STR_INIT(
    "Cache-Control: no-store\r\nAllow: GET, HEAD, POST\r\n");
static Str const ALLOW_GET_HEAD_PUT_DELETE = STR_INIT(
    "Cache-Control: no-store\r\nAllow: GET, HEAD, PUT, DELETE\r\n");
static Str const ALLOW_POST = STR_INIT(
    "Cache-Control: no-store\r\nAllow: POST\r\n");
static Str const ALLOW_DELETE = STR_INIT(
    "Cache-Control: no-store\r\nAllow: DELETE\r\n");

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
    case 415:
        return STR("Unsupported Media Type");
    case 429:
        return STR("Too Many Requests");
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

/*
 * Security headers, sent with every response.
 *
 * The session token lives in the browser's localStorage, so any script that
 * got injected into a page could read it. The Content-Security-Policy is the
 * safety net: the pages have no inline scripts or styles, and load scripts,
 * styles, images and API calls only from this site (plus Google Fonts), so an
 * injected <script>, inline handler or style, or a request to another site
 * does not run. blob: images are the upload preview of browsers without
 * createImageBitmap(). frame-ancestors and X-Frame-Options (for older
 * browsers) keep other sites from framing the pages to trick clicks;
 * nosniff keeps browsers from reading an upload as anything but its declared
 * type. HSTS makes browsers use HTTPS for the site for a year; they ignore it
 * over plain HTTP, such as the dev server.
 */
static Str const SECURITY_HEADERS = STR_INIT(
    "Content-Security-Policy: default-src 'self'; script-src 'self'; "
    "style-src 'self' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; "
    "img-src 'self' blob:; connect-src 'self'; object-src 'none'; base-uri 'none'; "
    "form-action 'self'; frame-ancestors 'none'\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: strict-origin-when-cross-origin\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n");

/**
 * The parts of a response that vary: everything but the body.
 */
typedef struct {
    int  status;
    Str  content_type;
    /** Complete header lines, each ending in "\r\n"; may be empty. */
    Str  extra_headers;
    /** The request was HEAD: the head is sent without the body. */
    bool head_only;
} Reply;

static Reply make_reply(int status, Str content_type, bool head_only, Str extra_headers) {
    return (Reply) {
        .status = status,
        .content_type = content_type,
        .extra_headers = extra_headers,
        .head_only = head_only,
    };
}

/**
 * Appends a status line and the common headers, up to and including the blank line.
 */
static void write_head(Str_Builder *b, Conn const *c, Reply const *reply,
                       uint64_t content_length) {
    assert(b != NULL);
    str_builder_append(b, STR("HTTP/1.1 "));
    str_builder_append_u64(b, (uint64_t)reply->status);
    str_builder_append(b, STR(" "));
    str_builder_append(b, status_reason(reply->status));
    str_builder_append(b, STR("\r\nServer: kasin\r\nContent-Type: "));
    str_builder_append(b, reply->content_type);
    str_builder_append(b, STR("\r\nContent-Length: "));
    str_builder_append_u64(b, content_length);
    str_builder_append(b, STR("\r\nConnection: "));
    str_builder_append(b, c->keepalive ? STR("keep-alive") : STR("close"));
    str_builder_append(b, STR("\r\n"));
    str_builder_append(b, SECURITY_HEADERS);
    str_builder_append(b, reply->extra_headers);
    str_builder_append(b, STR("\r\n"));
}

static void respond_simple(Conn *c, Reply const *reply, Str body) {
    Str_Builder b;
    str_builder_init(&b, (char *)c->out, c->out_cap);
    write_head(&b, c, reply, body.len);
    if (!reply->head_only) {
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
    Reply const reply = make_reply(status, STR("text/plain; charset=utf-8"), head_only, extra);
    respond_simple(c, &reply, str_builder_str(&body));
}

/**
 * Starts a response body that is built in place in the response buffer,
 * behind HEAD_RESERVE bytes kept free for the head. Finish with respond_json().
 */
static void body_begin(Conn *c, Str_Builder *body) {
    assert(body != NULL);
    str_builder_init(body, (char *)c->out + HEAD_RESERVE, c->out_cap - HEAD_RESERVE);
}

/**
 * Writes the head right in front of a body started with body_begin(), so the
 * response is one contiguous span and Content-Length is exact.
 */
static void respond_json(Conn *c, int status, Str_Builder const *body, bool head_only,
                         Str extra_headers) {
    if (body->overflow) {
        respond_error(c, 500, head_only);
        return;
    }
    Reply const reply = make_reply(status, JSON_TYPE, head_only, extra_headers);
    char head_buf[HEAD_RESERVE];
    Str_Builder head;
    str_builder_init(&head, head_buf, sizeof head_buf);
    write_head(&head, c, &reply, body->len);
    if (head.overflow) {
        respond_error(c, 500, head_only);
        return;
    }
    c->resp_pos = HEAD_RESERVE - (uint32_t)head.len;
    memcpy(c->out + c->resp_pos, head.data, head.len);
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
    respond_json(c, status, &body, false, extra_headers);
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

/*
 * openat2() is used through syscall(): libc headers do not all declare it, and
 * Alpine's need an extra package for <linux/openat2.h>. The number and the
 * struct layout are the same on every architecture.
 */
#ifndef SYS_openat2
// The system call number of openat2(), the same on every architecture.
#define SYS_openat2 437
#endif

enum {
    /** openat2() resolve flag: refuse symbolic links anywhere on the path. */
    RESOLVE_NO_SYMLINKS_FLAG = 0x04,
    /** openat2() resolve flag: refuse paths that leave the starting directory. */
    RESOLVE_BENEATH_FLAG = 0x08,
};

typedef struct {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
} Open_How;

/**
 * Opens a file for reading inside a directory, and never outside it.
 *
 * The kernel resolves the path with RESOLVE_BENEATH and RESOLVE_NO_SYMLINKS:
 * an absolute path, ".." or a symbolic link anywhere on the way is refused,
 * however the path was put together. On a kernel without openat2() (before
 * Linux 5.6, or one that filters the call) the same rules are checked here
 * instead, apart from symbolic links in the middle of the path.
 *
 * @return The descriptor, or -1 with errno set; a path that would leave the
 *         directory fails with ENOENT, like a missing file.
 */
static int open_beneath(int dir_fd, char const *path) {
    int const flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    Open_How how = {
        .flags = (uint64_t)flags,
        .mode = 0,
        .resolve = RESOLVE_BENEATH_FLAG | RESOLVE_NO_SYMLINKS_FLAG,
    };
    long fd = syscall(SYS_openat2, dir_fd, path, &how, sizeof how);
    if (fd >= 0) {
        return (int)fd;
    }
    if (errno == EXDEV || errno == ELOOP) {
        errno = ENOENT;  // The path tried to leave the directory.
        return -1;
    }
    if (errno != ENOSYS && errno != EPERM) {
        return -1;
    }

    Str const p = str_from_cstr(path);
    if (p.len == 0 || p.data[0] == '/' || str_contains(p, STR(".."))) {
        errno = ENOENT;
        return -1;
    }
    return openat(dir_fd, path, flags);
}

/**
 * Opens a path under the docroot and stats it.
 *
 * @return The descriptor, or -1 with errno set.
 */
static int open_in_root(Str rel_path, struct stat *st) {
    assert(st != NULL);
    char rel_path_z[MAX_PATH_LEN];  // openat2() needs a NUL-terminated path.
    if (ROOT_FD < 0) {
        errno = ENOENT;
        return -1;
    }
    if (!str_to_cstr(rel_path, rel_path_z, sizeof rel_path_z)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open_beneath(ROOT_FD, rel_path_z);
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
 * Writes the response head and keeps the file open so its body can be streamed
 * with sendfile(), whatever its size. Takes ownership of fd.
 */
static void send_file(Conn *c, Reply const *reply, int fd, struct stat const *st) {
    Str_Builder hdr;
    str_builder_init(&hdr, (char *)c->out, c->out_cap);
    write_head(&hdr, c, reply, (uint64_t)st->st_size);
    if (hdr.overflow) {
        close(fd);
        respond_error(c, 500, reply->head_only);
        return;
    }
    c->resp_pos = 0;
    c->resp_end = (uint32_t)hdr.len;

    if (reply->head_only || st->st_size == 0) {
        close(fd);
        return;
    }
    c->file_fd = fd;  // Streamed by conn_advance() once the head is out.
    c->file_pos = 0;
    c->file_end = st->st_size;
}

/**
 * Serves a file from the docroot.
 *
 * Clean URLs are supported: a directory serves its index.html, and a missing
 * path without an extension falls back to the same path plus ".html".
 */
static void serve_file(Conn *c, int status, Str rel_path, bool head_only, bool is_root) {
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
            Reply const welcome = make_reply(200, STR("text/html; charset=utf-8"), head_only,
                                             STR(""));
            respond_simple(c, &welcome, WELCOME);
        } else {
            bool not_found = path.overflow || errno == ENOENT || errno == ENOTDIR;
            respond_error(c, not_found ? 404 : 500, head_only);
        }
        return;
    }
    Reply const reply = make_reply(status, mime_for(str_builder_str(&path)), head_only, STR(""));
    send_file(c, &reply, fd, &st);
}

/**
 * Serves an uploaded image from the uploads directory. Image ids are never
 * reused, so the file at a URL never changes and may be cached for good.
 */
static void serve_upload(Conn *c, Str file_name, bool head_only) {
    char name_z[64];  // openat2() needs a NUL-terminated name.
    Str number;
    Str extension;
    uint64_t id;
    bool valid = str_split_once(file_name, STR("."), &number, &extension) &&
                 str_to_u64(number, &id) && str_find_char(extension, '.') == STR_NPOS &&
                 str_to_cstr(file_name, name_z, sizeof name_z);
    int fd = valid && UPLOADS_FD >= 0 ? open_beneath(UPLOADS_FD, name_z) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) {
            close(fd);
        }
        respond_error(c, 404, head_only);
        return;
    }
    Reply const reply = make_reply(200, mime_for(file_name), head_only,
                                   STR("Cache-Control: public, max-age=31536000, immutable\r\n"));
    send_file(c, &reply, fd, &st);
}

// Request parsing.

typedef struct {
    bool     close;
    bool     keepalive;
    bool     chunked;
    bool     bad_content_length;
    uint64_t content_length;
    Str      authorization;
    Str      file_name;  /**< X-File-Name: an upload's original name, URL-encoded. */
    Str      forwarded_for;  /**< X-Forwarded-For, trusted only with TRUST_PROXY. */
    Str      user_agent;
} Request_Headers;

typedef struct {
    Str                    method;
    Str                    path;  /**< Target without query string or fragment. */
    Str                    body;
    bool                   head_only;
    Request_Headers const *headers;
    Ip_Address             client;  /**< The client's address (see TRUST_PROXY). */
} Request;

/**
 * Scans header lines (CRLF separated, without the final blank line) for the
 * headers the server acts on.
 */
static void scan_headers(Str headers, Request_Headers *h) {
    assert(h != NULL);
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
        } else if (str_eq_nocase(name, STR("x-file-name"))) {
            h->file_name = value;
        } else if (str_eq_nocase(name, STR("x-forwarded-for"))) {
            h->forwarded_for = value;
        } else if (str_eq_nocase(name, STR("user-agent"))) {
            h->user_agent = value;
        }
    }
}

// API.

static int64_t unix_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

static Str jwt_key(void) {
    return str_make((char const *)JWT_KEY, sizeof JWT_KEY);
}

static bool authenticate_headers(Request_Headers const *h, Jwt_Claims *claims) {
    Str auth = h->authorization;
    Str scheme = STR("Bearer ");
    if (auth.len <= scheme.len || !str_eq_nocase(str_slice(auth, 0, scheme.len), scheme)) {
        return false;
    }
    Str token = str_trim(str_slice(auth, scheme.len, auth.len));
    return jwt_verify(token, jwt_key(), unix_now(), claims);
}

static bool authenticate(Request const *r, Jwt_Claims *claims) {
    return authenticate_headers(r->headers, claims);
}

static void respond_unauthorized(Conn *c) {
    respond_json_error(c, 401, STR("login required"),
                       STR("Cache-Control: no-store\r\nWWW-Authenticate: Bearer\r\n"));
}

static void respond_id(Conn *c, int status, int64_t id) {
    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"id\":"));
    str_builder_append_u64(&body, (uint64_t)id);
    str_builder_append(&body, STR("}"));
    respond_json(c, status, &body, false, NO_STORE);
}

/**
 * Who is logging in, for logs and the database: the address in text form and
 * its country.
 */
typedef struct {
    char        ip_buf[IP_TEXT_SIZE];
    Str         ip;
    char        country_buf[2];
    Str         country;  /**< Empty when unknown. */
} Login_Client;

static void describe_client(Ip_Address const *address, Login_Client *client) {
    assert(client != NULL);
    Str_Builder ip;
    str_builder_init(&ip, client->ip_buf, sizeof client->ip_buf);
    ip_format(address, &ip);
    client->ip = str_builder_str(&ip);
    client->country = geoip_country(&GEOIP, address, client->country_buf)
                      ? str_make(client->country_buf, 2)
                      : STR("");
}

/**
 * Copies text for logs and the database: at most capacity bytes, cut at a
 * character boundary, with control characters (and with ascii_only, any byte
 * outside ASCII) replaced by "?".
 */
static Str clean_text(Str text, char *buf, size_t capacity, bool ascii_only) {
    assert(buf != NULL);
    size_t len = text.len < capacity ? text.len : capacity;
    while (len > 0 && len < text.len && ((unsigned char)text.data[len] & 0xC0) == 0x80) {
        len--;  // Do not split a UTF-8 character.
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char const b = (unsigned char)text.data[i];
        buf[i] = b < 0x20 || b == 0x7F || (ascii_only && b >= 0x80) ? '?' : (char)b;
    }
    return str_make(buf, len);
}

// Appends text in double quotes, with quotes and backslashes escaped, for log lines.
static void append_quoted(Str_Builder *out, Str text) {
    str_builder_append_char(out, '"');
    for (size_t i = 0; i < text.len; i++) {
        if (text.data[i] == '"' || text.data[i] == '\\') {
            str_builder_append_char(out, '\\');
        }
        str_builder_append_char(out, text.data[i]);
    }
    str_builder_append_char(out, '"');
}

static void append_client(Str_Builder *out, Login_Client const *client) {
    str_builder_append(out, client->ip);
    str_builder_append(out, STR(" ("));
    str_builder_append(out, client->country.len > 0 ? client->country : STR("country unknown"));
    str_builder_append(out, STR(")"));
}

// Writes a finished log line to stderr, which Azure Container Apps keeps as the console log.
static void log_line(Str_Builder const *line) {
    fprintf(stderr, "%.*s\n", STR_ARG(str_builder_str(line)));
}

static void log_login_refused(Login_Client const *client, Login_Decision const *d) {
    char buf[512];
    Str_Builder line;
    str_builder_init(&line, buf, sizeof buf);
    str_builder_append(&line, STR("login: refused attempt from "));
    append_client(&line, client);
    if (d->verdict == LOGIN_BUSY) {
        str_builder_append(&line, STR(": too many logins from all addresses in the last minute"));
    } else {
        Str const phrase = d->verdict == LOGIN_BANNED ? STR(": banned after ")
                                                      : STR(": must wait after ");
        str_builder_append(&line, phrase);
        str_builder_append_u64(&line, d->failures);
        str_builder_append(&line, STR(" failures"));
    }
    str_builder_append(&line, STR(", "));
    str_builder_append_u64(&line, (uint64_t)d->retry_after);
    str_builder_append(&line, STR(" s more ("));
    str_builder_append_u64(&line, d->refused);
    str_builder_append(&line, STR(" refused since the last such line)"));
    log_line(&line);
}

static void log_login_checked(Login_Client const *client, Str username, bool succeeded,
                              Login_Decision const *after) {
    char buf[512];
    Str_Builder line;
    str_builder_init(&line, buf, sizeof buf);
    str_builder_append(&line, succeeded ? STR("login: success for ") : STR("login: FAILED for "));
    append_quoted(&line, username);
    str_builder_append(&line, STR(" from "));
    append_client(&line, client);
    if (!succeeded) {
        str_builder_append(&line, STR("; "));
        str_builder_append_u64(&line, after->failures);
        Str const noun = after->failures == 1 ? STR(" recent failure") : STR(" recent failures");
        str_builder_append(&line, noun);
        if (after->verdict == LOGIN_BANNED) {
            str_builder_append(&line, STR(", now banned for "));
            str_builder_append_u64(&line, (uint64_t)after->retry_after);
            str_builder_append(&line, STR(" s"));
        } else if (after->verdict == LOGIN_DELAYED) {
            str_builder_append(&line, STR(", next attempt allowed in "));
            str_builder_append_u64(&line, (uint64_t)after->retry_after);
            str_builder_append(&line, STR(" s"));
        }
    }
    log_line(&line);
}

static void respond_login_refused(Conn *c, Login_Decision const *d) {
    char headers_buf[96];
    Str_Builder headers;
    str_builder_init(&headers, headers_buf, sizeof headers_buf);
    str_builder_append(&headers, STR("Cache-Control: no-store\r\nRetry-After: "));
    str_builder_append_u64(&headers, (uint64_t)d->retry_after);
    str_builder_append(&headers, STR("\r\n"));

    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"error\":\""));
    Str const message = d->verdict == LOGIN_BUSY
                        ? STR("too many login attempts right now")
                        : STR("too many failed logins from this address");
    str_builder_append(&body, message);
    str_builder_append(&body, STR("\",\"reason\":\""));
    str_builder_append(&body, d->verdict == LOGIN_BUSY     ? STR("busy")
                              : d->verdict == LOGIN_BANNED ? STR("banned")
                                                           : STR("delayed"));
    str_builder_append(&body, STR("\",\"retry_after\":"));
    str_builder_append_u64(&body, (uint64_t)d->retry_after);
    str_builder_append(&body, STR("}"));
    respond_json(c, 429, &body, false, str_builder_str(&headers));
}

/**
 * POST /api/login. An address that failed too often recently, or a burst of
 * attempts from everywhere, is refused at once (see login_guard.h). Every
 * password that is checked is recorded in the database and logged.
 */
static void api_login(Worker *w, Conn *c, Request const *r) {
    if (!LOGIN_ENABLED) {
        respond_json_error(c, 503, STR("login is not configured"), NO_STORE);
        return;
    }
    int64_t const now = unix_now();
    Login_Client client;
    describe_client(&r->client, &client);
    Login_Decision const decision = login_guard_check(&LOGIN_GUARD, &r->client, now);
    if (decision.verdict != LOGIN_ALLOWED) {
        if (decision.log_refusal) {
            log_login_refused(&client, &decision);
        }
        respond_login_refused(c, &decision);
        return;
    }

    char username_buf[128];
    char password_buf[128];
    Str_Builder username;
    Str_Builder password;
    str_builder_init(&username, username_buf, sizeof username_buf);
    str_builder_init(&password, password_buf, sizeof password_buf);
    Db_Status status = db_parse_login(&w->db, r->body, &username, &password);
    if (status != DB_OK) {
        explicit_bzero(password_buf, sizeof password_buf);
        respond_db_error(c, status);
        return;
    }
    // Both fields are checked every time, so timing does not reveal which was wrong.
    bool const user_ok = str_eq_constant_time(str_builder_str(&username), ADMIN_USERNAME);
    bool const password_ok = password_verify(str_builder_str(&password), &ADMIN_PASSWORD_HASH);
    explicit_bzero(password_buf, sizeof password_buf);
    bool const succeeded = user_ok && password_ok;

    char name_buf[100];
    char agent_buf[300];
    Db_Login_Attempt const attempt = {
        .ip = client.ip,
        .country = client.country,
        .username = clean_text(str_builder_str(&username), name_buf, sizeof name_buf, false),
        .user_agent = clean_text(r->headers->user_agent, agent_buf, sizeof agent_buf, true),
        .succeeded = succeeded,
        .at = now,
    };
    if (db_record_login(&w->db, &attempt) != DB_OK) {
        fprintf(stderr, "login: could not record the attempt from %.*s in the database\n",
                STR_ARG(client.ip));
    }

    if (!succeeded) {
        Login_Decision const after = login_guard_failed(&LOGIN_GUARD, &r->client, now);
        log_login_checked(&client, attempt.username, false, &after);
        Str_Builder body;
        body_begin(c, &body);
        str_builder_append(&body, STR("{\"error\":\"invalid username or password\","
                                      "\"retry_after\":"));
        str_builder_append_u64(&body, (uint64_t)after.retry_after);
        str_builder_append(&body, STR("}"));
        respond_json(c, 401, &body, false,
                     STR("Cache-Control: no-store\r\nWWW-Authenticate: Bearer\r\n"));
        return;
    }
    login_guard_succeeded(&LOGIN_GUARD, &r->client);
    log_login_checked(&client, attempt.username, true, &decision);

    int64_t expires_at = now + TOKEN_LIFETIME_SEC;
    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"token\":\""));
    if (!jwt_issue(&body, ADMIN_USERNAME, now, expires_at, jwt_key())) {
        respond_json_error(c, 500, STR("cannot issue token"), NO_STORE);
        return;
    }
    str_builder_append(&body, STR("\",\"expires_at\":"));
    str_builder_append_u64(&body, (uint64_t)expires_at);
    str_builder_append(&body, STR("}"));
    respond_json(c, 200, &body, false, NO_STORE);
}

/**
 * The login report: the addresses that must wait or are banned right now
 * (from memory) and the recorded clients and attempts (from the database).
 */
static Db_Status build_login_report(Worker *w, int64_t now, Str_Builder *body) {
    assert(body != NULL);
    Login_Block blocks[MAX_REPORTED_BLOCKS];
    size_t const count = login_guard_blocks(&LOGIN_GUARD, now, blocks, MAX_REPORTED_BLOCKS);
    str_builder_append(body, STR("{\"now\":"));
    str_builder_append_u64(body, (uint64_t)now);
    str_builder_append(body, STR(",\"blocked\":["));
    for (size_t i = 0; i < count; i++) {
        Login_Client client;
        describe_client(&blocks[i].ip, &client);
        str_builder_append(body, i == 0 ? STR("{\"ip\":\"") : STR(",{\"ip\":\""));
        str_builder_append(body, client.ip);
        str_builder_append(body, STR("\",\"failures\":"));
        str_builder_append_u64(body, blocks[i].failures);
        str_builder_append(body, STR(",\"until\":"));
        str_builder_append_u64(body, (uint64_t)blocks[i].until);
        Str const banned = blocks[i].banned ? STR(",\"banned\":true}")
                                            : STR(",\"banned\":false}");
        str_builder_append(body, banned);
    }
    str_builder_append(body, STR("],\"report\":"));
    Db_Status const status = db_login_report_json(&w->db, body);
    if (status != DB_OK) {
        return status;
    }
    str_builder_append(body, STR("}"));
    return body->overflow ? DB_TOO_LARGE : DB_OK;
}

// GET /api/login-attempts (auth): the admin's "Kirjautumiset" view.
static void api_login_report(Worker *w, Conn *c, Request const *r) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    int64_t const now = unix_now();
    Str_Builder body;
    body_begin(c, &body);
    Db_Status status = build_login_report(w, now, &body);
    if (status == DB_TOO_LARGE && big_acquire(w, c)) {
        c->out = c->big;
        c->out_cap = BIG_BUF_SIZE;
        body_begin(c, &body);
        status = build_login_report(w, now, &body);
    }
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_json(c, 200, &body, r->head_only, NO_STORE);
}

// DELETE /api/login-blocks/{client id} (auth): lifts an address's wait or ban.
static void api_login_unblock(Worker *w, Conn *c, Request const *r, int64_t id) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    char ip_buf[IP_TEXT_SIZE + 1];
    Str_Builder ip_text;
    str_builder_init(&ip_text, ip_buf, sizeof ip_buf);
    Db_Status const status = db_clear_login_client(&w->db, id, unix_now(), &ip_text);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    Ip_Address ip;
    if (ip_parse(str_builder_str(&ip_text), &ip)) {
        login_guard_clear(&LOGIN_GUARD, &ip);
    }
    fprintf(stderr, "login: an admin lifted the block of %.*s\n",
            STR_ARG(str_builder_str(&ip_text)));
    respond_id(c, 200, id);
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
    respond_json(c, 200, &body, r->head_only, NO_STORE);
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

static void api_list(Worker *w, Conn *c, Request const *r, Resource const *res) {
    Str_Builder body;
    body_begin(c, &body);
    Db_Status status = res->list(&w->db, &body);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_json(c, 200, &body, r->head_only, NO_STORE);
}

static void api_get(Worker *w, Conn *c, Request const *r, Resource const *res, int64_t id) {
    Str_Builder body;
    body_begin(c, &body);
    Db_Status status = res->get(&w->db, id, &body);
    // A long item (a recipe full of blocks) is built again in a large buffer.
    if (status == DB_TOO_LARGE && big_acquire(w, c)) {
        c->out = c->big;
        c->out_cap = BIG_BUF_SIZE;
        body_begin(c, &body);
        status = res->get(&w->db, id, &body);
    }
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_json(c, 200, &body, r->head_only, NO_STORE);
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

// The file extension for an image, judged by its first bytes; empty if unknown.
static Str image_extension(Str bytes) {
    if (str_starts_with(bytes, STR("\xFF\xD8\xFF"))) {
        return STR("jpg");
    }
    if (str_starts_with(bytes, STR("\x89PNG\r\n\x1A\n"))) {
        return STR("png");
    }
    if (str_starts_with(bytes, STR("GIF87a")) || str_starts_with(bytes, STR("GIF89a"))) {
        return STR("gif");
    }
    if (str_starts_with(bytes, STR("RIFF")) && str_eq(str_slice(bytes, 8, 12), STR("WEBP"))) {
        return STR("webp");
    }
    return STR("");
}

/*
 * The X-File-Name header holds the original name URL-encoded by the browser.
 * It is stored as sent, for display only, and dropped if it holds anything an
 * encoded name cannot.
 */
static Str clean_original_name(Str name) {
    if (name.len > 255 || str_find_any(name, STR("\"\\<> ")) != STR_NPOS) {
        return STR("");
    }
    for (size_t i = 0; i < name.len; i++) {
        if ((unsigned char)name.data[i] < 0x21 || (unsigned char)name.data[i] > 0x7E) {
            return STR("");
        }
    }
    return name;
}

// Writes bytes to a new file in the uploads directory, then moves it into place.
static bool write_upload(Str file_name, Str bytes) {
    char final_buf[64];
    char part_buf[64];
    Str_Builder final_name;
    Str_Builder part_name;
    str_builder_init(&final_name, final_buf, sizeof final_buf);
    str_builder_init(&part_name, part_buf, sizeof part_buf);
    str_builder_append(&final_name, file_name);
    str_builder_append(&part_name, file_name);
    str_builder_append(&part_name, STR(".part"));
    char const *final_z = str_builder_cstr(&final_name);  // openat() and renameat() need C strings.
    char const *part_z = str_builder_cstr(&part_name);
    if (!final_z || !part_z || UPLOADS_FD < 0) {
        return false;
    }

    int fd = openat(UPLOADS_FD, part_z, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    size_t written = 0;
    while (written < bytes.len) {
        ssize_t n = write(fd, bytes.data + written, bytes.len - written);
        if (n > 0) {
            written += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    bool ok = written == bytes.len && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && renameat(UPLOADS_FD, part_z, UPLOADS_FD, final_z) == 0;
    if (!ok) {
        unlinkat(UPLOADS_FD, part_z, 0);
    }
    return ok;
}

static void api_upload_image(Worker *w, Conn *c, Request const *r) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    Str extension = image_extension(r->body);
    if (extension.len == 0) {
        respond_json_error(c, 415, STR("only JPEG, PNG, GIF and WebP images are accepted"),
                           NO_STORE);
        return;
    }

    int64_t id;
    Str const original_name = clean_original_name(r->headers->file_name);
    Db_Status status = db_create_image(&w->db, extension, original_name, (int64_t)r->body.len,
                                       &id);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }

    char name_buf[64];
    Str_Builder file_name;
    str_builder_init(&file_name, name_buf, sizeof name_buf);
    str_builder_append_u64(&file_name, (uint64_t)id);
    str_builder_append(&file_name, STR("."));
    str_builder_append(&file_name, extension);
    if (!write_upload(str_builder_str(&file_name), r->body)) {
        fprintf(stderr, "upload: cannot write %.*s: %s\n", STR_ARG(str_builder_str(&file_name)),
                strerror(errno));
        db_forget_image(&w->db, id);
        respond_json_error(c, 500, STR("cannot store the image"), NO_STORE);
        return;
    }

    Str_Builder body;
    body_begin(c, &body);
    str_builder_append(&body, STR("{\"id\":"));
    str_builder_append_u64(&body, (uint64_t)id);
    str_builder_append(&body, STR(",\"url\":\"/uploads/"));
    str_builder_append(&body, str_builder_str(&file_name));
    str_builder_append(&body, STR("\"}"));
    respond_json(c, 201, &body, false, NO_STORE);
}

static void api_list_images(Worker *w, Conn *c, Request const *r) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    Str_Builder body;
    body_begin(c, &body);
    Db_Status status = db_list_images_json(&w->db, &body);
    if (status == DB_TOO_LARGE && big_acquire(w, c)) {
        c->out = c->big;
        c->out_cap = BIG_BUF_SIZE;
        body_begin(c, &body);
        status = db_list_images_json(&w->db, &body);
    }
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_json(c, 200, &body, r->head_only, NO_STORE);
}

static void api_delete_image(Worker *w, Conn *c, Request const *r, int64_t id) {
    if (!authenticate(r, NULL)) {
        respond_unauthorized(c);
        return;
    }
    char name_buf[64];
    Str_Builder file_name;
    str_builder_init(&file_name, name_buf, sizeof name_buf);
    Db_Status status = db_delete_image(&w->db, id, &file_name);
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    char const *name_z = str_builder_cstr(&file_name);  // unlinkat() needs a C string.
    if (name_z && unlinkat(UPLOADS_FD, name_z, 0) != 0 && errno != ENOENT) {
        fprintf(stderr, "upload: cannot remove %s: %s\n", name_z, strerror(errno));
    }
    respond_id(c, 200, id);
}

/**
 * Handles /api/images and /api/images/{id}. Images are uploaded as the raw
 * file in the request body, not as JSON.
 *
 * @return False when the path is something else.
 */
static bool handle_images(Worker *w, Conn *c, Request const *r) {
    Str collection = STR("/api/images");
    if (str_eq(r->path, collection)) {
        if (str_eq(r->method, STR("GET")) || r->head_only) {
            api_list_images(w, c, r);
        } else if (str_eq(r->method, STR("POST"))) {
            api_upload_image(w, c, r);
        } else {
            respond_method_not_allowed(c, ALLOW_GET_HEAD_POST);
        }
        return true;
    }
    Str item = str_slice(r->path, collection.len, r->path.len);
    if (!str_starts_with(r->path, collection) || !str_starts_with(item, STR("/"))) {
        return false;
    }
    uint64_t id;
    if (!str_to_u64(str_slice(item, 1, item.len), &id) || id == 0 || id > INT64_MAX) {
        respond_db_error(c, DB_NOT_FOUND);
    } else if (str_eq(r->method, STR("DELETE"))) {
        api_delete_image(w, c, r, (int64_t)id);
    } else {
        respond_method_not_allowed(c, ALLOW_DELETE);
    }
    return true;
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
            respond_method_not_allowed(c, ALLOW_GET_HEAD_POST);
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
        respond_method_not_allowed(c, ALLOW_GET_HEAD_PUT_DELETE);
    }
    return true;
}

static void api_get_recipe_by_slug(Worker *w, Conn *c, Request const *r, Str slug) {
    Str_Builder body;
    body_begin(c, &body);
    Db_Status status = db_get_recipe_by_slug_json(&w->db, slug, &body);
    if (status == DB_TOO_LARGE && big_acquire(w, c)) {
        c->out = c->big;
        c->out_cap = BIG_BUF_SIZE;
        body_begin(c, &body);
        status = db_get_recipe_by_slug_json(&w->db, slug, &body);
    }
    if (status != DB_OK) {
        respond_db_error(c, status);
        return;
    }
    respond_json(c, 200, &body, r->head_only, NO_STORE);
}

/**
 * Routes requests under /api/.
 *
 *   POST   /api/login             {username, password} -> {token, expires_at}
 *   GET    /api/session           (auth) -> {username, expires_at}
 *
 *   GET    /api/recipes           -> [{id, slug, name, image, excerpt, category_id, category}, ...]
 *   POST   /api/recipes           (auth) {slug, name, image, description, category_id} -> 201 {id},
 *                                 or 409 if another recipe has the slug
 *   GET    /api/recipes/{id}      -> {id, slug, name, image, description, category_id, category}
 *   GET    /api/recipes/slug/{slug} -> the same, found by slug
 *   PUT    /api/recipes/{id}      (auth) {slug, name, image, description, category_id} -> {id},
 *                                 or 409 if another recipe has the slug
 *   DELETE /api/recipes/{id}      (auth) -> {id}
 *
 *   GET    /api/categories        -> [{id, name, recipe_count}, ...] alphabetically
 *   POST   /api/categories        (auth) {name} -> 201 {id}, or 409 if the name is taken
 *   GET    /api/categories/{id}   -> {id, name, recipe_count}
 *   PUT    /api/categories/{id}   (auth) {name} -> {id}, or 409 if the name is taken
 *   DELETE /api/categories/{id}   (auth) -> {id}, or 409 while recipes use it
 *
 *   GET    /api/images            (auth) -> [{id, url, original_name, size, created_at, used,
 *                                 recipes}, ...]
 *   POST   /api/images            (auth) raw JPEG/PNG/GIF/WebP body, X-File-Name -> 201 {id, url}
 *   DELETE /api/images/{id}       (auth) -> {id}, or 409 while a recipe shows it
 *
 *   GET    /api/login-attempts    (auth) -> {now, blocked: [{ip, failures, until, banned}, ...],
 *                                 report: {clients: [...], attempts: [...]}}
 *   DELETE /api/login-blocks/{client id} (auth) -> {id}: lifts the address's wait or ban
 *
 * A login refused by the brute-force protection gets 429 with Retry-After and
 * {error, reason: "delayed" | "banned" | "busy", retry_after}; a wrong password
 * gets 401 with {error, retry_after}, the wait before the next attempt.
 */
static void handle_api(Worker *w, Conn *c, Request const *r) {
    if (str_eq(r->path, STR("/api/login"))) {
        if (str_eq(r->method, STR("POST"))) {
            api_login(w, c, r);
        } else {
            respond_method_not_allowed(c, ALLOW_POST);
        }
        return;
    }
    if (str_eq(r->path, STR("/api/session"))) {
        if (str_eq(r->method, STR("GET")) || r->head_only) {
            api_session(c, r);
        } else {
            respond_method_not_allowed(c, ALLOW_GET_HEAD);
        }
        return;
    }
    if (str_eq(r->path, STR("/api/login-attempts"))) {
        if (str_eq(r->method, STR("GET")) || r->head_only) {
            api_login_report(w, c, r);
        } else {
            respond_method_not_allowed(c, ALLOW_GET_HEAD);
        }
        return;
    }
    Str login_blocks = STR("/api/login-blocks/");
    if (str_starts_with(r->path, login_blocks)) {
        uint64_t id;
        Str const id_text = str_slice(r->path, login_blocks.len, r->path.len);
        if (!str_to_u64(id_text, &id) || id == 0 || id > INT64_MAX) {
            respond_db_error(c, DB_NOT_FOUND);
        } else if (str_eq(r->method, STR("DELETE"))) {
            api_login_unblock(w, c, r, (int64_t)id);
        } else {
            respond_method_not_allowed(c, ALLOW_DELETE);
        }
        return;
    }
    Str by_slug = STR("/api/recipes/slug/");
    if (str_starts_with(r->path, by_slug)) {
        if (str_eq(r->method, STR("GET")) || r->head_only) {
            api_get_recipe_by_slug(w, c, r, str_slice(r->path, by_slug.len, r->path.len));
        } else {
            respond_method_not_allowed(c, ALLOW_GET_HEAD);
        }
        return;
    }
    if (handle_images(w, c, r)) {
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
 * Moves a request whose body does not fit the connection's own buffer into a
 * large buffer, which only signed-in users may tie up; or refuses it.
 *
 * @return 0 when the request now waits in a large buffer for the rest of its
 *         body, otherwise head_len: an error was answered and the request is done.
 */
static size_t take_large_body(Worker *w, Conn *c, Request_Headers const *h, size_t head_len) {
    if (c->in == c->big || h->content_length > BIG_BUF_SIZE - head_len) {
        respond_error(c, 413, false);
        return head_len;
    }
    if (!authenticate_headers(h, NULL)) {
        respond_unauthorized(c);
        return head_len;
    }
    if (!big_acquire(w, c)) {
        respond_json_error(c, 503, STR("server busy, try again"),
                           STR("Cache-Control: no-store\r\nRetry-After: 2\r\n"));
        return head_len;
    }
    memcpy(c->big, c->in, c->req_len);
    c->in = c->big;
    c->in_cap = BIG_BUF_SIZE;
    return 0;  // Wait for the rest of the body.
}

/**
 * Answers a whole request, by its path.
 */
static void route_request(Worker *w, Conn *c, Request const *r) {
    /*
     * No percent-decoding: reject anything that could escape the root. An
     * empty segment ("//") is refused too: with the leading "/" dropped, the
     * path "//etc/passwd" would be the absolute path "/etc/passwd". Files are
     * also opened with open_beneath(), which keeps them inside the root even if
     * a path got past this check. The set below is length-based, so it
     * includes the NUL byte too.
     */
    if (r->path.len == 0 || r->path.data[0] != '/' || r->path.len >= MAX_PATH_LEN - 16 ||
        str_find_any(r->path, STR("%\\\0")) != STR_NPOS || str_contains(r->path, STR("..")) ||
        str_contains(r->path, STR("//"))) {
        respond_error(c, 400, r->head_only);
        return;
    }

    if (str_starts_with(r->path, STR("/api/"))) {
        handle_api(w, c, r);
        return;
    }

    if (!str_eq(r->method, STR("GET")) && !r->head_only) {
        respond_error(c, 405, false);
        return;
    }

    /*
     * Recipe pages, /reseptit/<slug>: one page shows every recipe and loads it
     * by the slug in its address. An unknown slug still gets the page (which
     * says so), but with status 404.
     */
    Str const recipes_prefix = STR("/reseptit/");
    if (str_starts_with(r->path, recipes_prefix)) {
        Str const slug = str_slice(r->path, recipes_prefix.len, r->path.len);
        bool exists = false;
        if (slug.len > 0 && str_find_char(slug, '/') == STR_NPOS &&
            db_recipe_slug_exists(&w->db, slug, &exists) != DB_OK) {
            exists = true;  // The page reports any database trouble itself.
        }
        serve_file(c, exists ? 200 : 404, STR("resepti.html"), r->head_only, false);
        return;
    }

    if (str_starts_with(r->path, STR("/uploads/"))) {
        serve_upload(c, str_slice(r->path, 9, r->path.len), r->head_only);
        return;
    }

    if (str_eq(r->path, STR("/health"))) {
        Reply const reply = make_reply(200, STR("text/plain; charset=utf-8"), r->head_only,
                                       STR(""));
        respond_simple(c, &reply, STR("ok\n"));
        return;
    }

    Str const rel = str_slice(r->path, 1, r->path.len);  // Drop leading '/'.
    char path_buf[MAX_PATH_LEN];
    Str_Builder path;
    str_builder_init(&path, path_buf, sizeof path_buf);
    str_builder_append(&path, rel);
    if (rel.len == 0 || str_ends_with(rel, STR("/"))) {
        str_builder_append(&path, STR("index.html"));
    }

    serve_file(c, 200, str_builder_str(&path), r->head_only, rel.len == 0);
}

/**
 * Reads the request whose head is buffered, and answers it when its body has
 * arrived as well.
 *
 * @param head The request line and headers, without the blank line after them.
 * @param head_len Length of the head with the blank line, in bytes.
 * @return How many buffered bytes the request used, or 0 when it is not complete yet.
 */
static size_t handle_request(Worker *w, Conn *c, Str head, size_t head_len) {
    Str const buffered = str_make((char const *)c->in, c->req_len);
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
        return head_len;
    }

    bool const http11 = str_eq(version, STR("HTTP/1.1"));
    bool const http10 = str_eq(version, STR("HTTP/1.0"));
    if (!http11 && !http10) {
        respond_error(c, 505, false);
        return head_len;
    }

    Request_Headers h = {
        .close = false,
        .keepalive = false,
        .chunked = false,
        .bad_content_length = false,
        .content_length = 0,
        .authorization = STR(""),
        .file_name = STR(""),
        .forwarded_for = STR(""),
        .user_agent = STR(""),
    };
    scan_headers(headers, &h);
    if (h.bad_content_length) {
        respond_error(c, 400, false);
        return head_len;
    }
    if (h.chunked) {
        respond_error(c, 411, false);
        return head_len;
    }
    if (h.content_length > c->in_cap - head_len) {
        return take_large_body(w, c, &h, head_len);
    }
    size_t const request_len = head_len + (size_t)h.content_length;
    if (buffered.len < request_len) {
        return 0;  // Wait for the rest of the body.
    }
    // After a large body the connection closes, so the large buffer is free at once.
    c->keepalive = (http11 ? !h.close : h.keepalive) && c->in != c->big;

    Request r = {
        .method = method,
        .path = target,
        .body = str_slice(buffered, head_len, request_len),
        .head_only = str_eq(method, STR("HEAD")),
        .headers = &h,
        .client = c->peer,
    };
    Ip_Address forwarded;
    if (TRUST_PROXY && ip_from_forwarded_for(h.forwarded_for, &forwarded)) {
        r.client = forwarded;
    }

    // Strip query string / fragment.
    size_t const query = str_find_any(r.path, STR("?#"));
    if (query != STR_NPOS) {
        r.path = str_slice(r.path, 0, query);
    }

    route_request(w, c, &r);
    return request_len;
}

/**
 * Answers the request at the start of the connection's input when it is
 * complete: fills the response and removes the request from the input.
 *
 * @return False when more bytes are needed first.
 */
static bool build_response(Worker *w, Conn *c) {
    Str const buffered = str_make((char const *)c->in, c->req_len);
    Str head;
    Str after_head;
    if (!str_split_once(buffered, STR("\r\n\r\n"), &head, &after_head)) {
        return false;
    }
    size_t const head_len = buffered.len - after_head.len;

    c->keepalive = false;  // Errors below close.
    size_t const consumed = handle_request(w, c, head, head_len);
    if (consumed == 0) {
        return false;
    }

    if (c->big && c->in == c->big) {
        // A large request: the connection closes after the response.
        c->keepalive = false;
        c->req_len = 0;
        c->in = c->req;
        c->in_cap = REQ_BUF_SIZE;
        if (c->out != c->big) {
            big_release(w, c);
        }
        return true;
    }
    // The request is fully handled; keep any pipelined bytes behind it.
    c->req_len -= (uint32_t)consumed;
    memmove(c->in, c->in + consumed, c->req_len);
    return true;
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
    big_release(w, c);
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
                if (c->req_len >= c->in_cap) {  // Headers too large.
                    c->keepalive = false;
                    c->req_len = 0;
                    respond_error(c, 431, false);
                } else {
                    conn_want(w, c, EPOLLIN);
                    return;
                }
            }
            c->state = ST_WRITING;
        }

        while (c->resp_pos < c->resp_end) {
            ssize_t n = send(c->fd, c->out + c->resp_pos, c->resp_end - c->resp_pos,
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
        if (c->big && c->out == c->big) {
            big_release(w, c);  // A large response has been sent.
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
    if (c->req_len < c->in_cap) {
        ssize_t n = recv(c->fd, c->in + c->req_len, c->in_cap - c->req_len, 0);
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
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        int fd = accept4(w->listen_fd, (struct sockaddr *)&peer, &peer_len,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
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
        c->keepalive = false;
        c->req_len = c->resp_pos = c->resp_end = 0;
        c->file_fd = -1;
        c->in = c->req;
        c->in_cap = REQ_BUF_SIZE;
        c->out = c->resp;
        c->out_cap = RESP_BUF_SIZE;
        c->big = NULL;
        c->file_pos = c->file_end = 0;
        c->last_active = now;
        c->peer = ip_from_v4((uint8_t const *)&peer.sin_addr.s_addr);
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

// Called for each address with recent failures in the database at startup.
static void restore_login_failures(void *context, Str ip_text, uint32_t failures,
                                   int64_t last_failure) {
    Ip_Address ip;
    if (ip_parse(ip_text, &ip)) {
        login_guard_restore(&LOGIN_GUARD, &ip, failures, last_failure);
        (*(size_t *)context)++;
    }
}

static void on_signal(int sig) {
    (void)sig;
    STOP_REQUESTED = 1;
}

static void usage(Str argv0) {
    fprintf(stderr,
            "usage: %.*s [-p port] [-t threads] [-r docroot] [-d database] [-m migrations]\n"
            "       [-u uploads] [-s local|share] [-g countries]\n"
            "  -m  directory of <number>__<name>.sql migrations (default: migrations)\n"
            "  -u  directory uploaded images are stored in, created if missing (default: uploads)\n"
            "  -s  where the database lives; use share for a network share such as Azure Files\n"
            "  -g  country file made by kasin-geoip, for the countries of login attempts\n"
            "environment:\n"
            "  KASIN_ADMIN_PASSWORD_HASH  the admin password hash made by kasin-passwd\n"
            "  KASIN_JWT_SECRET           signs sessions; random per start when unset\n"
            "  KASIN_TRUST_PROXY=1        take the client address from X-Forwarded-For\n",
            STR_ARG(argv0));
}

/**
 * What the command line asks for.
 */
typedef struct {
    int        port;
    /** Worker threads: one per processor unless -t says otherwise. */
    long       workers;
    Str        root;
    Str        db_path;
    Str        migrations_dir;
    Str        uploads_dir;
    Str        geoip_path;
    Db_Storage db_storage;
} Settings;

/**
 * Reads one option that takes an argument.
 *
 * @return False when the argument is not valid.
 */
static bool apply_option(int opt, Str argument, Settings *settings) {
    assert(settings != NULL);
    uint64_t value;
    switch (opt) {
    case 'p':
        if (!str_to_u64(argument, &value) || value > 65535) {
            return false;
        }
        settings->port = (int)value;
        return true;
    case 't':
        if (!str_to_u64(argument, &value) || value > MAX_WORKERS) {
            return false;
        }
        settings->workers = (long)value;
        return true;
    case 'r':
        settings->root = argument;
        return true;
    case 'd':
        settings->db_path = argument;
        return true;
    case 'm':
        settings->migrations_dir = argument;
        return true;
    case 'u':
        settings->uploads_dir = argument;
        return true;
    case 'g':
        settings->geoip_path = argument;
        return true;
    case 's':
        if (str_eq(argument, STR("local"))) {
            settings->db_storage = DB_STORAGE_LOCAL;
            return true;
        }
        if (str_eq(argument, STR("share"))) {
            settings->db_storage = DB_STORAGE_SHARE;
            return true;
        }
        return false;
    default:
        return false;
    }
}

/**
 * Reads the command line.
 *
 * @param settings Receives the settings.
 * @param status Receives the exit status when the server should not start.
 * @return True when the server should go on and start.
 */
static bool parse_settings(int argc, char **argv, Settings *settings, int *status) {
    assert(settings != NULL);
    assert(status != NULL);
    // CHECK What is _SC_NPROCESSORS_ONLN?
    *settings = (Settings) {
        .port = 8080,
        .workers = sysconf(_SC_NPROCESSORS_ONLN),
        .root = STR(""),
        .db_path = STR("kotoa.db"),
        .migrations_dir = STR("migrations"),
        .uploads_dir = STR("uploads"),
        .geoip_path = STR(""),
        .db_storage = DB_STORAGE_LOCAL,
    };
    // getopt() and argv are C strings; the options are kept as Str from here on.
    Str const program = str_from_cstr(argv[0]);
    int opt;
    // CHECK What does getopt do?
    while ((opt = getopt(argc, argv, "p:t:r:d:m:u:s:g:h")) != -1) {
        if (opt == 'h') {
            usage(program);
            *status = EXIT_SUCCESS;
            return false;
        }
        if (!apply_option(opt, str_from_cstr(optarg), settings)) {
            usage(program);
            *status = EX_USAGE;
            return false;
        }
    }
    if (settings->port <= 0 || settings->port > 65535) {
        usage(program);
        *status = EX_USAGE;
        return false;
    }
    if (settings->workers < 1) {
        settings->workers = 1;
    }
    if (settings->workers > MAX_WORKERS) {
        settings->workers = MAX_WORKERS;
    }
    return true;
}

/**
 * Opens the directory of static files and the one of uploaded images. Problems
 * are warned about; the server then runs without them.
 */
static void open_directories(Settings const *settings) {
    if (settings->root.len > 0) {
        char root_z[MAX_PATH_LEN];  // open() needs a NUL-terminated path.
        if (str_to_cstr(settings->root, root_z, sizeof root_z)) {
            ROOT_FD = open(root_z, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }
        if (ROOT_FD < 0) {
            fprintf(stderr, "warning: cannot open docroot '%.*s': %s\n",
                    STR_ARG(settings->root), strerror(errno));
        }
    }

    char uploads_z[MAX_PATH_LEN];  // mkdir() and open() need a NUL-terminated path.
    if (str_to_cstr(settings->uploads_dir, uploads_z, sizeof uploads_z)) {
        if (mkdir(uploads_z, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "warning: cannot create uploads directory '%s': %s\n",
                    uploads_z, strerror(errno));
        }
        UPLOADS_FD = open(uploads_z, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    if (UPLOADS_FD < 0) {
        fprintf(stderr, "warning: cannot open uploads directory '%.*s'; image uploads are off\n",
                STR_ARG(settings->uploads_dir));
    }
}

/**
 * The memory the server reserves at startup, in bytes.
 */
static size_t reserved_bytes(long workers) {
    size_t const big_bytes = (size_t)BIG_BUFS_PER_WORKER * arena_align_up(BIG_BUF_SIZE, CACHE_LINE);
    size_t const per_worker = arena_align_up(sizeof (Worker), CACHE_LINE) +
                              arena_align_up(sizeof (struct epoll_event) * EVENT_BATCH,
                                             CACHE_LINE) +
                              sizeof (Conn) * MAX_CONNS_PER_WORKER +
                              (size_t)MAX_CONNS_PER_WORKER * REQ_BUF_SIZE +
                              (size_t)MAX_CONNS_PER_WORKER * RESP_BUF_SIZE +
                              big_bytes;
    return arena_align_up(per_worker * (size_t)workers + DB_HEAP_SIZE + (size_t)CACHE_LINE * 8,
                          PAGE_SIZE);
}

/**
 * Sets up what logins need: the session key, the admin password hash, the
 * proxy setting, the brute-force protection and the country data.
 *
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int configure_login(Settings const *settings) {
    // Session tokens survive restarts only when the secret is configured.
    Str const secret = str_from_cstr(getenv("KASIN_JWT_SECRET"));
    if (secret.len > 0) {
        Sha256 key_hash;
        sha256_init(&key_hash);
        sha256_update(&key_hash, secret.data, secret.len);
        sha256_final(&key_hash, JWT_KEY);
    } else if (getrandom(JWT_KEY, sizeof JWT_KEY, 0) != (ssize_t)sizeof JWT_KEY) {
        perror("getrandom");
        return EX_OSERR;
    }

    // A missing hash leaves the site running with logins off; a malformed one is a mistake.
    Str const password_hash = str_from_cstr(getenv("KASIN_ADMIN_PASSWORD_HASH"));
    if (password_hash.len == 0) {
        fprintf(stderr, "warning: KASIN_ADMIN_PASSWORD_HASH is not set, so nobody can log in; "
                        "make one with kasin-passwd (see docs/admin-password.md)\n");
    } else if (password_hash_parse(str_trim(password_hash), &ADMIN_PASSWORD_HASH)) {
        LOGIN_ENABLED = true;
    } else {
        fprintf(stderr, "KASIN_ADMIN_PASSWORD_HASH is not a hash made by kasin-passwd "
                        "(pbkdf2-sha256:<iterations>:<salt>:<hash>)\n");
        return EX_CONFIG;
    }

    TRUST_PROXY = str_eq(str_from_cstr(getenv("KASIN_TRUST_PROXY")), STR("1"));
    login_guard_init(&LOGIN_GUARD, unix_now());
    char geoip_z[MAX_PATH_LEN];  // open() needs a NUL-terminated path.
    if (settings->geoip_path.len == 0) {
        fprintf(stderr, "warning: no country file (-g); "
                        "login attempts are recorded without countries\n");
    } else if (!str_to_cstr(settings->geoip_path, geoip_z, sizeof geoip_z) ||
               !geoip_open(&GEOIP, geoip_z)) {
        fprintf(stderr, "warning: login attempts are recorded without countries\n");
    }
    return EXIT_SUCCESS;
}

/**
 * Carves a worker's memory out of the arena and gives it its listening socket,
 * its epoll instance and its database connection.
 *
 * @param out Receives the worker.
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int create_worker(Arena *arena, Settings const *settings, int id, Worker **out) {
    assert(out != NULL);
    Worker *w = arena_alloc(arena, sizeof *w, CACHE_LINE);
    if (!w) {
        fputs("arena exhausted\n", stderr);
        return EX_SOFTWARE;
    }
    size_t const request_bytes = (size_t)MAX_CONNS_PER_WORKER * REQ_BUF_SIZE;
    size_t const response_bytes = (size_t)MAX_CONNS_PER_WORKER * RESP_BUF_SIZE;
    w->events = arena_alloc(arena, sizeof (struct epoll_event) * EVENT_BATCH, CACHE_LINE);
    w->conns = arena_alloc(arena, sizeof (Conn) * MAX_CONNS_PER_WORKER, CACHE_LINE);
    uint8_t *reqs = arena_alloc(arena, request_bytes, CACHE_LINE);
    uint8_t *resps = arena_alloc(arena, response_bytes, CACHE_LINE);
    if (!w->events || !w->conns || !reqs || !resps) {
        fputs("arena exhausted\n", stderr);
        return EX_SOFTWARE;
    }
    w->big_free_count = 0;
    for (int j = 0; j < BIG_BUFS_PER_WORKER; j++) {
        uint8_t *big = arena_alloc(arena, BIG_BUF_SIZE, CACHE_LINE);
        if (!big) {
            fputs("arena exhausted\n", stderr);
            return EX_SOFTWARE;
        }
        w->big_free[w->big_free_count++] = big;
    }

    w->id = id;
    w->free_list = NULL;
    for (int j = MAX_CONNS_PER_WORKER - 1; j >= 0; j--) {  // Build free list.
        Conn *c = &w->conns[j];
        c->fd = -1;
        c->req = reqs + (size_t)j * REQ_BUF_SIZE;
        c->resp = resps + (size_t)j * RESP_BUF_SIZE;
        c->next_free = w->free_list;
        w->free_list = c;
    }

    w->listen_fd = make_listener((uint16_t)settings->port);
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
    if (!db_open(&w->db, settings->db_path, settings->db_storage)) {
        return EX_SOFTWARE;
    }
    *out = w;
    return EXIT_SUCCESS;
}

// Waits and bans from before a restart still apply.
static void restore_login_state(Worker *w) {
    size_t restored = 0;
    if (db_recent_login_failures(&w->db, unix_now() - LOGIN_WINDOW_SEC, restore_login_failures,
                                 &restored) != DB_OK) {
        fprintf(stderr, "warning: could not read recent failed logins from the database\n");
    } else if (restored > 0) {
        fprintf(stderr, "login: restored recent failures of %zu address%s\n", restored,
                restored == 1 ? "" : "es");
    }
}

static void install_signal_handlers(void) {
    // No SA_RESTART so signals wake epoll_wait.
    struct sigaction sa = {
        .sa_handler = on_signal,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void print_banner(Settings const *settings, size_t reserved) {
    printf("kasin-httpd: port %d, %ld workers, %d conns/worker, req %d B, resp %d B, "
           "reserved %.1f MiB (SQLite heap %d MiB, %d x %d KiB large buffers per worker), "
           "docroot %.*s, database %.*s, uploads %.*s, client address from %s\n",
           settings->port, settings->workers, MAX_CONNS_PER_WORKER, REQ_BUF_SIZE, RESP_BUF_SIZE,
           (double)reserved / (1024.0 * 1024.0), DB_HEAP_SIZE / (1024 * 1024),
           BIG_BUFS_PER_WORKER, BIG_BUF_SIZE / 1024,
           STR_ARG(settings->root.len > 0 ? settings->root : STR("(none)")),
           STR_ARG(settings->db_path), STR_ARG(settings->uploads_dir),
           TRUST_PROXY ? "X-Forwarded-For" : "the connection");
    fflush(stdout);
}

/**
 * Runs the workers until a signal asks the server to stop, then closes their
 * databases. Worker 0 runs on the calling thread.
 */
static void run_workers(Worker *const *workers, long count) {
    long started = count;
    for (long i = 1; i < count; i++) {
        if (pthread_create(&workers[i]->thread, NULL, worker_main, workers[i]) != 0) {
            perror("pthread_create");
            STOP_REQUESTED = 1;
            started = i;
            break;
        }
    }

    worker_main(workers[0]);

    for (long i = 1; i < started; i++) {
        pthread_join(workers[i]->thread, NULL);
    }
    for (long i = 0; i < count; i++) {
        db_close(&workers[i]->db);
    }
}

int main(int argc, char **argv) {
    Settings settings;
    int status;
    if (!parse_settings(argc, argv, &settings, &status)) {
        return status;
    }
    open_directories(&settings);

    // The one and only memory reservation.
    size_t const total = reserved_bytes(settings.workers);
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return EX_OSERR;
    }
    madvise(mem, total, MADV_HUGEPAGE);  // Best effort.

    Arena arena;
    arena_init(&arena, mem, total);

    status = configure_login(&settings);
    if (status != EXIT_SUCCESS) {
        return status;
    }

    // SQLite gets a fixed heap out of the same reservation.
    void *db_heap = arena_alloc(&arena, DB_HEAP_SIZE, CACHE_LINE);
    if (!db_heap) {
        fputs("arena exhausted\n", stderr);
        return EX_SOFTWARE;
    }
    if (!db_global_init(db_heap, DB_HEAP_SIZE) ||
        !db_prepare_file(settings.db_path, settings.db_storage, settings.migrations_dir)) {
        return EX_SOFTWARE;
    }

    Worker *workers[MAX_WORKERS];
    for (long i = 0; i < settings.workers; i++) {
        status = create_worker(&arena, &settings, (int)i, &workers[i]);
        if (status != EXIT_SUCCESS) {
            return status;
        }
    }

    restore_login_state(workers[0]);
    install_signal_handlers();
    print_banner(&settings, total);
    run_workers(workers, settings.workers);
    puts("kasin-httpd: shut down");
    return EXIT_SUCCESS;
}
