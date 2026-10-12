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
 * REQ_BUF_SIZE; a served file must fit in RESP_BUF_SIZE - HDR_RESERVE.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

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

#define MAX_WORKERS       64
#define EVENT_BATCH       128
#define ACCEPT_BURST      64
#define LISTEN_BACKLOG    1024
#define IDLE_TIMEOUT_SEC  15
#define HDR_RESERVE       512  // Space in front of a file body for its header.
#define MAX_PATH_LEN      1024

static_assert (RESP_BUF_SIZE >= 4 * HDR_RESERVE, "RESP_BUF_SIZE too small");
static_assert (REQ_BUF_SIZE >= 512, "REQ_BUF_SIZE too small");

#define CACHE_LINE 64

// Arena allocator.

typedef struct {
    uint8_t *base;
    size_t   capacity;
    size_t   used;
} Arena;

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

static void *arena_alloc(Arena *a, size_t size, size_t align) {
    size_t p = align_up(a->used, align);
    if (p > a->capacity || size > a->capacity - p) {
        return NULL;
    }
    a->used = p + size;
    return a->base + p;
}

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
} Worker;

static volatile sig_atomic_t STOP_REQUESTED = 0;
static int ROOT_FD = -1;  // Directory static files are served from.

// Small helpers.

static int64_t mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec;
}

static int ci_contains(char const *s, size_t n, char const *tok) {
    size_t tl = strlen(tok);
    for (size_t i = 0; i + tl <= n; i++) {
        if (strncasecmp(s + i, tok, tl) == 0) {
            return 1;
        }
    }
    return 0;
}

static char const *mime_for(char const *path) {
    static struct {
        char const *ext;
        char const *mime;
    } const MIME_TABLE[] = {
        {
            .ext = ".html",
            .mime = "text/html; charset=utf-8",
        },
        {
            .ext = ".htm",
            .mime = "text/html; charset=utf-8",
        },
        {
            .ext = ".css",
            .mime = "text/css; charset=utf-8",
        },
        {
            .ext = ".js",
            .mime = "text/javascript; charset=utf-8",
        },
        {
            .ext = ".json",
            .mime = "application/json",
        },
        {
            .ext = ".txt",
            .mime = "text/plain; charset=utf-8",
        },
        {
            .ext = ".svg",
            .mime = "image/svg+xml",
        },
        {
            .ext = ".png",
            .mime = "image/png",
        },
        {
            .ext = ".jpg",
            .mime = "image/jpeg",
        },
        {
            .ext = ".jpeg",
            .mime = "image/jpeg",
        },
        {
            .ext = ".gif",
            .mime = "image/gif",
        },
        {
            .ext = ".ico",
            .mime = "image/x-icon",
        },
        {
            .ext = ".wasm",
            .mime = "application/wasm",
        },
    };
    char const *dot = strrchr(path, '.');
    if (dot) {
        for (size_t i = 0; i < sizeof MIME_TABLE / sizeof MIME_TABLE[0]; i++) {
            if (strcasecmp(dot, MIME_TABLE[i].ext) == 0) {
                return MIME_TABLE[i].mime;
            }
        }
    }
    return "application/octet-stream";
}

// Response building.

static void respond_simple(Conn *c, int status, char const *reason, char const *ctype,
                           char const *body, size_t body_len, int head_only,
                           char const *extra_headers) {
    char *out = (char *)c->resp;
    int n = snprintf(out, RESP_BUF_SIZE,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: kasin\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "%s\r\n",
                     status, reason, ctype, body_len,
                     c->keepalive ? "keep-alive" : "close",
                     extra_headers ? extra_headers : "");
    size_t len = (size_t)n;
    if (!head_only && body_len && len + body_len <= RESP_BUF_SIZE) {
        memcpy(out + len, body, body_len);
        len += body_len;
    }
    c->resp_pos = 0;
    c->resp_end = (uint32_t)len;
}

static void respond_error(Conn *c, int status, int head_only) {
    char const *reason;
    char const *extra = NULL;
    switch (status) {
    case 400:
        reason = "Bad Request";
        break;
    case 404:
        reason = "Not Found";
        break;
    case 405:
        reason = "Method Not Allowed";
        extra = "Allow: GET, HEAD\r\n";
        break;
    case 431:
        reason = "Request Header Fields Too Large";
        break;
    case 505:
        reason = "HTTP Version Not Supported";
        break;
    default:
        status = 500;
        reason = "Internal Server Error";
        break;
    }
    char body[96];
    int bl = snprintf(body, sizeof body, "%d %s\n", status, reason);
    respond_simple(c, status, reason, "text/plain; charset=utf-8", body, (size_t)bl,
                   head_only, extra);
}

static char const WELCOME[] =
    "<!doctype html><meta charset=utf-8><title>kasin-httpd</title>"
    "<h1>kasin-httpd is running</h1><p>Put files in the served directory.</p>\n";

/**
 * Reads the file into resp at HDR_RESERVE, then writes the header right in front
 * of it so Content-Length is exact and the whole response is one contiguous span.
 */
static void serve_file(Conn *c, char const *rel_path, int head_only, int is_root) {
    int fd = -1;
    if (ROOT_FD >= 0) {
        fd = openat(ROOT_FD, rel_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    }
    if (fd < 0) {
        if (is_root) {  // No index.html yet: show a built-in page.
            respond_simple(c, 200, "OK", "text/html; charset=utf-8",
                           WELCOME, sizeof WELCOME - 1, head_only, NULL);
        } else {
            int not_found = ROOT_FD < 0 || errno == ENOENT || errno == ENOTDIR;
            respond_error(c, not_found ? 404 : 500, head_only);
        }
        return;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        respond_error(c, 404, head_only);
        return;
    }
    if ((uint64_t)st.st_size > RESP_BUF_SIZE - HDR_RESERVE) {
        close(fd);
        respond_error(c, 500, head_only);  // Does not fit the fixed response buffer.
        return;
    }

    size_t body_len = (size_t)st.st_size;
    if (!head_only) {
        size_t got = 0;
        while (got < body_len) {
            ssize_t r = pread(fd, c->resp + HDR_RESERVE + got, body_len - got, (off_t)got);
            if (r > 0) {
                got += (size_t)r;
            } else if (r < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        body_len = got;  // File may have shrunk underneath us.
    }
    close(fd);

    char hdr[HDR_RESERVE];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\n"
                      "Server: kasin\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: %s\r\n"
                      "\r\n",
                      mime_for(rel_path), head_only ? (size_t)st.st_size : body_len,
                      c->keepalive ? "keep-alive" : "close");
    if (hl <= 0 || hl >= HDR_RESERVE) {
        respond_error(c, 500, head_only);
        return;
    }

    c->resp_pos = HDR_RESERVE - (uint32_t)hl;
    memcpy(c->resp + c->resp_pos, hdr, (size_t)hl);
    c->resp_end = HDR_RESERVE + (head_only ? 0 : (uint32_t)body_len);
}

// Request parsing / routing.

typedef struct {
    int close;
    int keepalive;
    int has_body;
} Header_Flags;

static void scan_headers(char const *p, char const *end, Header_Flags *f) {
    while (p < end) {
        char const *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) {
            eol = end;
        }
        size_t len = (size_t)(eol - p);
        char const *colon = memchr(p, ':', len);
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            char const *v = colon + 1;
            size_t vlen = len - nlen - 1;
            if (nlen == 10 && strncasecmp(p, "connection", 10) == 0) {
                if (ci_contains(v, vlen, "close")) {
                    f->close = 1;
                } else if (ci_contains(v, vlen, "keep-alive")) {
                    f->keepalive = 1;
                }
            } else if (nlen == 14 && strncasecmp(p, "content-length", 14) == 0) {
                for (size_t i = 0; i < vlen; i++) {
                    if (v[i] >= '1' && v[i] <= '9') {
                        f->has_body = 1;
                        break;
                    }
                }
            } else if (nlen == 17 && strncasecmp(p, "transfer-encoding", 17) == 0) {
                f->has_body = 1;
            }
        }
        p = eol + 1;
    }
}

/**
 * Returns 1 and fills c->resp if a full request head is buffered, else 0.
 */
static int build_response(Conn *c) {
    char const *req = (char const *)c->req;
    char const *term = memmem(req, c->req_len, "\r\n\r\n", 4);
    if (!term) {
        return 0;
    }

    size_t head_len = (size_t)(term - req) + 4;
    char const *eol = memchr(req, '\r', head_len);  // End of request line.
    size_t line_len = (size_t)(eol - req);

    c->keepalive = 0;  // Errors below close.

    char const *sp1 = memchr(req, ' ', line_len);
    char const *sp2 = sp1 ? memchr(sp1 + 1, ' ', line_len - (size_t)(sp1 + 1 - req)) : NULL;
    if (!sp1 || !sp2) {
        respond_error(c, 400, 0);
        goto done;
    }

    char const *method = req;
    size_t method_len = (size_t)(sp1 - req);
    char const *target = sp1 + 1;
    size_t target_len = (size_t)(sp2 - target);
    char const *ver = sp2 + 1;
    size_t ver_len = (size_t)(eol - ver);

    int http11 = ver_len == 8 && memcmp(ver, "HTTP/1.1", 8) == 0;
    int http10 = ver_len == 8 && memcmp(ver, "HTTP/1.0", 8) == 0;
    if (!http11 && !http10) {
        respond_error(c, 505, 0);
        goto done;
    }

    Header_Flags f = {
        .close = 0,
        .keepalive = 0,
        .has_body = 0,
    };
    scan_headers(eol + 2, req + head_len - 2, &f);
    c->keepalive = (http11 ? !f.close : f.keepalive) && !f.has_body;

    int is_get = method_len == 3 && memcmp(method, "GET", 3) == 0;
    int is_head = method_len == 4 && memcmp(method, "HEAD", 4) == 0;
    if (!is_get && !is_head) {
        c->keepalive = 0;
        respond_error(c, 405, 0);
        goto done;
    }

    // Strip query string / fragment.
    for (size_t i = 0; i < target_len; i++) {
        if (target[i] == '?' || target[i] == '#') {
            target_len = i;
            break;
        }
    }

    // No percent-decoding: reject anything that could escape the root.
    if (target_len == 0 || target[0] != '/' || target_len >= MAX_PATH_LEN - 16 ||
        memchr(target, '%', target_len) || memchr(target, '\\', target_len) ||
        memchr(target, '\0', target_len) || memmem(target, target_len, "..", 2)) {
        respond_error(c, 400, is_head);
        goto done;
    }

    if (target_len == 7 && memcmp(target, "/health", 7) == 0) {
        respond_simple(c, 200, "OK", "text/plain; charset=utf-8", "ok\n", 3, is_head, NULL);
        goto done;
    }

    char path[MAX_PATH_LEN];
    size_t plen = target_len - 1;  // Drop leading '/'.
    memcpy(path, target + 1, plen);
    int is_root = 0;
    if (plen == 0 || path[plen - 1] == '/') {
        memcpy(path + plen, "index.html", 11);  // Incl. NUL.
        is_root = (plen == 0);
    } else {
        path[plen] = '\0';
    }

    serve_file(c, path, is_head, is_root);

done:
    // The request head is fully handled; keep any pipelined bytes behind it.
    c->req_len -= (uint32_t)head_len;
    memmove(c->req, c->req + head_len, c->req_len);
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
            if (!build_response(c)) {
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
    int opt;
    // CHECK What does getopt do?
    while ((opt = getopt(argc, argv, "p:t:r:h")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            break;
        case 't':
            nworkers = atol(optarg);
            break;
        case 'r':
            root = optarg;
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
    size_t per_worker = align_up(sizeof (Worker), CACHE_LINE)
                      + align_up(sizeof (struct epoll_event) * EVENT_BATCH, CACHE_LINE)
                      + sizeof (Conn) * MAX_CONNS_PER_WORKER
                      + (size_t)MAX_CONNS_PER_WORKER * REQ_BUF_SIZE
                      + (size_t)MAX_CONNS_PER_WORKER * RESP_BUF_SIZE;
    size_t total = align_up(per_worker * (size_t)nworkers + CACHE_LINE * 4, 4096);

    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return EX_OSERR;
    }
    madvise(mem, total, MADV_HUGEPAGE);  // Best effort.

    Arena arena = {
        .base = mem,
        .capacity = total,
        .used = 0,
    };

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
           "reserved %.1f MiB, docroot %s\n",
           port, nworkers, MAX_CONNS_PER_WORKER, REQ_BUF_SIZE, RESP_BUF_SIZE,
           (double)total / (1024.0 * 1024.0), root ? root : "(none)");
    fflush(stdout);

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
    puts("kasin-httpd: shut down");
    return EXIT_SUCCESS;
}
