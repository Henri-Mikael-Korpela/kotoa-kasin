/**
 * @file htmlgen.c
 * @brief kasin-htmlgen: generates the site's HTML pages from the templates in templates/.
 *
 * Usage: kasin-htmlgen [-w] -o <out_dir> <templates_dir>
 *
 * Every .html file under <templates_dir>/pages/ is processed and written to the same
 * path under <out_dir>: pages/index.html becomes <out_dir>/index.html and
 * pages/hallinta/kuvat.html becomes <out_dir>/hallinta/kuvat.html. {{include "x.html"}}
 * looks for the template in <templates_dir>/fragments/. The syntax is in template.h and
 * docs/html-templating.md.
 *
 * With -w the tool keeps running and generates the pages again whenever a file under
 * <templates_dir> changes, a file is added or one is removed. Errors are reported and the
 * tool waits for the next change; the pages already generated stay in place. It polls file
 * times instead of using inotify, which does not see changes made through the Windows bind
 * mounts of Docker Desktop.
 *
 * Like the server, the tool allocates nothing after startup: one mmap() reserves all its
 * memory, carved by an arena.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "str.h"
#include "template.h"

enum {
    /** Memory the tool reserves at startup, in bytes. */
    MEMORY_SIZE = 32 * 1024 * 1024,
    /** Largest template the tool reads, in bytes. */
    MAX_FILE_SIZE = 1024 * 1024,
    /** Largest page the tool makes, in bytes. */
    MAX_PAGE_SIZE = 1024 * 1024,
    /** Most files under the templates directory. */
    MAX_FILES = 256,
    /** Most directories under the templates directory. */
    MAX_DIRECTORIES = 32,
    /** Longest path the tool builds, with its terminator, in bytes. */
    PATH_CAPACITY = 4096,
    /** How often watch mode looks at the files, in milliseconds. */
    WATCH_POLL_MS = 300,
};

/** The FNV-1a offset basis, the start value of the hash of the files' state. */
static uint64_t const HASH_START = 14695981039346656037ull;

/** Where the pages and the templates they include are, below the templates directory. */
static Str const PAGES_DIR = STR_INIT("pages");
static Str const FRAGMENTS_DIR = STR_INIT("fragments");

/** The paths of the files under a directory, relative to it. */
typedef struct {
    Str    paths[MAX_FILES];
    size_t count;
} File_List;

/** What {{include}} needs to find a template. */
typedef struct {
    Arena *arena;
    Str    fragments_dir;
} Loader_Context;

static void usage(void) {
    fprintf(stderr,
            "Usage: kasin-htmlgen [-w] -o <out_dir> <templates_dir>\n"
            "  -o DIR  where the pages are written\n"
            "  -w      keep running; generate the pages again when a template changes\n");
}

/**
 * Joins a directory and a relative path into a NUL-terminated path for the OS. Either
 * can be empty.
 */
static bool join_path(Str dir, Str relative, char *buf, size_t capacity) {
    assert(buf != NULL);
    Str_Builder b;
    str_builder_init(&b, buf, capacity);
    str_builder_append(&b, dir);
    if (dir.len > 0 && relative.len > 0) {
        str_builder_append_char(&b, '/');
    }
    str_builder_append(&b, relative);
    return str_builder_cstr(&b) != NULL;
}

/**
 * Joins a directory and a relative path into the arena.
 */
static bool arena_join(Arena *arena, Str dir, Str relative, Str *joined) {
    assert(joined != NULL);
    size_t const capacity = dir.len + relative.len + 2;
    char *buf = arena_alloc(arena, capacity, 1);
    if (buf == NULL) {
        fprintf(stderr, "out of memory\n");
        return false;
    }
    Str_Builder b;
    str_builder_init(&b, buf, capacity);
    str_builder_append(&b, dir);
    if (dir.len > 0 && relative.len > 0) {
        str_builder_append_char(&b, '/');
    }
    str_builder_append(&b, relative);
    *joined = str_builder_str(&b);
    return true;
}

/**
 * Reads a whole file into the arena.
 */
static bool read_file(Arena *arena, char const *path, Str *contents) {
    assert(contents != NULL);
    int const fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (off_t)MAX_FILE_SIZE) {
        fprintf(stderr, "%s: not a regular file of at most %d bytes\n", path, MAX_FILE_SIZE);
        close(fd);
        return false;
    }
    size_t const size = (size_t)st.st_size;
    char *data = arena_alloc(arena, size + 1, 1);
    if (data == NULL) {
        fprintf(stderr, "%s: out of memory\n", path);
        close(fd);
        return false;
    }
    size_t done = 0;
    while (done < size) {
        ssize_t const n = read(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            char const *reason = "file shrank while reading";
            if (n < 0) {
                reason = strerror(errno);
            }
            fprintf(stderr, "%s: %s\n", path, reason);
            close(fd);
            return false;
        }
        done += (size_t)n;
    }
    close(fd);
    *contents = str_make(data, size);
    return true;
}

/**
 * Writes a file through a temporary name, so a reader never sees half of it.
 */
static bool write_file(char const *path, Str contents) {
    assert(path != NULL);
    char tmp[PATH_CAPACITY];
    Str_Builder name;
    str_builder_init(&name, tmp, sizeof tmp);
    str_builder_append(&name, str_from_cstr(path));
    str_builder_append(&name, STR(".tmp"));
    if (str_builder_cstr(&name) == NULL) {
        fprintf(stderr, "%s: path too long\n", path);
        return false;
    }
    int const fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", tmp, strerror(errno));
        return false;
    }
    size_t done = 0;
    while (done < contents.len) {
        ssize_t const n = write(fd, contents.data + done, contents.len - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "%s: %s\n", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return false;
        }
        done += (size_t)n;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

/**
 * Creates the directories above a file, like mkdir -p.
 */
static bool make_parent_directories(char const *file) {
    assert(file != NULL);
    char buf[PATH_CAPACITY];
    Str const full = str_from_cstr(file);
    for (size_t end = 1; end < full.len; end++) {
        if (full.data[end] != '/') {
            continue;
        }
        if (!str_to_cstr(str_slice(full, 0, end), buf, sizeof buf)) {
            fprintf(stderr, "%s: path too long\n", file);
            return false;
        }
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "%s: %s\n", buf, strerror(errno));
            return false;
        }
    }
    return true;
}

/**
 * Finds every file below a directory, however deep, without recursion: the directories
 * found are queued and visited in turn. Names that start with a dot are left out.
 *
 * @param list Receives the paths of the files, relative to root.
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int list_files(Arena *arena, Str root, File_List *list) {
    assert(list != NULL);
    list->count = 0;
    Str directories[MAX_DIRECTORIES];
    size_t directory_count = 1;
    directories[0] = STR("");
    for (size_t d = 0; d < directory_count; d++) {
        char path[PATH_CAPACITY];
        if (!join_path(root, directories[d], path, sizeof path)) {
            fprintf(stderr, "%.*s: path too long\n", STR_ARG(root));
            return EX_USAGE;
        }
        DIR *dir = opendir(path);
        if (dir == NULL) {
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            return EX_NOINPUT;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            Str const name = str_from_cstr(entry->d_name);
            if (name.len == 0 || name.data[0] == '.') {
                continue;
            }
            Str relative;
            if (!arena_join(arena, directories[d], name, &relative)) {
                closedir(dir);
                return EX_SOFTWARE;
            }
            char entry_path[PATH_CAPACITY];
            struct stat st;
            if (!join_path(root, relative, entry_path, sizeof entry_path)
                || stat(entry_path, &st) != 0) {
                fprintf(stderr, "%.*s/%.*s: cannot be read\n", STR_ARG(root),
                        STR_ARG(relative));
                closedir(dir);
                return EX_NOINPUT;
            }
            if (S_ISDIR(st.st_mode) && directory_count < MAX_DIRECTORIES) {
                directories[directory_count++] = relative;
            } else if (S_ISREG(st.st_mode) && list->count < MAX_FILES) {
                list->paths[list->count++] = relative;
            } else if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
                fprintf(stderr, "%.*s: too many files or directories\n", STR_ARG(root));
                closedir(dir);
                return EX_DATAERR;
            }
        }
        closedir(dir);
    }
    return EXIT_SUCCESS;
}

/**
 * Finds an included template in the fragments directory.
 */
static bool load_fragment(void *context, Str name, Str *contents) {
    Loader_Context const *loader = context;
    char path[PATH_CAPACITY];
    if (!join_path(loader->fragments_dir, name, path, sizeof path)) {
        fprintf(stderr, "%.*s: path too long\n", STR_ARG(name));
        return false;
    }
    return read_file(loader->arena, path, contents);
}

/**
 * Generates one page: reads its template, processes it and writes the result.
 *
 * @param relative The page's path below the pages directory, which is also its path
 *                 below the output directory.
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int build_page(Arena *arena, Str templates_dir, Str out_dir, Str relative) {
    Str pages_dir;
    if (!arena_join(arena, templates_dir, PAGES_DIR, &pages_dir)) {
        return EX_SOFTWARE;
    }
    char source_path[PATH_CAPACITY];
    char out_path[PATH_CAPACITY];
    if (!join_path(pages_dir, relative, source_path, sizeof source_path)
        || !join_path(out_dir, relative, out_path, sizeof out_path)) {
        fprintf(stderr, "%.*s: path too long\n", STR_ARG(relative));
        return EX_USAGE;
    }
    Str source;
    if (!read_file(arena, source_path, &source)) {
        return EX_NOINPUT;
    }

    char *buf = arena_alloc(arena, MAX_PAGE_SIZE, 1);
    if (buf == NULL) {
        fprintf(stderr, "%s: out of memory\n", source_path);
        return EX_SOFTWARE;
    }
    Str_Builder out;
    str_builder_init(&out, buf, MAX_PAGE_SIZE);
    Loader_Context loader = {
        .arena = arena,
        .fragments_dir = STR(""),
    };
    if (!arena_join(arena, templates_dir, FRAGMENTS_DIR, &loader.fragments_dir)) {
        return EX_SOFTWARE;
    }
    Template_Error error;
    if (!template_render(str_from_cstr(source_path), source, load_fragment, &loader, &out,
                         &error)) {
        template_print_error(&error);
        return EX_DATAERR;
    }

    if (!make_parent_directories(out_path) || !write_file(out_path, str_builder_str(&out))) {
        return EX_CANTCREAT;
    }
    printf("%s: %zu bytes\n", out_path, out.len);
    return EXIT_SUCCESS;
}

/**
 * Mixes a value into a running FNV-1a hash.
 */
static uint64_t hash_mix(uint64_t hash, uint64_t value) {
    for (int i = 0; i < 8; i++) {
        hash ^= (value >> (i * 8)) & 0xFF;
        hash *= 1099511628211ull;
    }
    return hash;
}

/**
 * Summarizes the names, sizes and modification times of every file under the templates
 * directory, so that a change, an added file or a removed one changes the value.
 */
static uint64_t watch_state(Arena *arena, Str templates_dir) {
    size_t const mark = arena->used;
    uint64_t hash = HASH_START;
    File_List *list = arena_alloc(arena, sizeof *list, _Alignof(File_List));
    if (list != NULL && list_files(arena, templates_dir, list) == EXIT_SUCCESS) {
        for (size_t i = 0; i < list->count; i++) {
            char path[PATH_CAPACITY];
            struct stat st;
            if (!join_path(templates_dir, list->paths[i], path, sizeof path)
                || stat(path, &st) != 0) {
                continue;
            }
            for (size_t n = 0; n < list->paths[i].len; n++) {
                hash = hash_mix(hash, (uint64_t)(byte)list->paths[i].data[n]);
            }
            hash = hash_mix(hash, (uint64_t)st.st_mtim.tv_sec);
            hash = hash_mix(hash, (uint64_t)st.st_mtim.tv_nsec);
            hash = hash_mix(hash, (uint64_t)st.st_size);
        }
    } else {
        hash = hash_mix(hash, UINT64_MAX);
    }
    arena->used = mark;
    return hash;
}

/**
 * Generates every page. Memory above the mark is given back when done, so the same call
 * serves every round of watch mode.
 *
 * @param state Receives the state of the files as it was before the build, so that a change
 *              made during the build causes another build.
 * @return EXIT_SUCCESS, or the sysexits.h code for the first page that failed. The pages
 *         after it are still generated.
 */
static int build_all(Arena *arena, Str templates_dir, Str out_dir, uint64_t *state) {
    assert(state != NULL);
    size_t const base = arena->used;
    *state = watch_state(arena, templates_dir);

    File_List *list = arena_alloc(arena, sizeof *list, _Alignof(File_List));
    Str pages_dir;
    if (list == NULL || !arena_join(arena, templates_dir, PAGES_DIR, &pages_dir)) {
        arena->used = base;
        return EX_SOFTWARE;
    }
    int status = list_files(arena, pages_dir, list);
    if (status != EXIT_SUCCESS) {
        arena->used = base;
        return status;
    }
    size_t built = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (!str_ends_with(list->paths[i], STR(".html"))) {
            continue;
        }
        // Each page's templates and output are discarded once it is written.
        size_t const mark = arena->used;
        int const page_status = build_page(arena, templates_dir, out_dir, list->paths[i]);
        arena->used = mark;
        if (page_status == EXIT_SUCCESS) {
            built++;
        } else if (status == EXIT_SUCCESS) {
            status = page_status;
        }
    }
    if (status == EXIT_SUCCESS && built == 0) {
        fprintf(stderr, "%.*s: no .html pages\n", STR_ARG(pages_dir));
        status = EX_NOINPUT;
    }
    arena->used = base;
    return status;
}

/**
 * Generates the pages again whenever the templates change; does not return.
 */
static void watch(Arena *arena, Str templates_dir, Str out_dir) {
    while (true) {
        uint64_t state;
        int const status = build_all(arena, templates_dir, out_dir, &state);
        if (status == EXIT_SUCCESS) {
            printf("Watching %.*s for changes.\n", STR_ARG(templates_dir));
        } else {
            printf("Build failed; waiting for changes to %.*s.\n", STR_ARG(templates_dir));
        }
        fflush(stdout);
        fflush(stderr);

        while (watch_state(arena, templates_dir) == state) {
            struct timespec const pause = {
                .tv_sec = 0,
                .tv_nsec = WATCH_POLL_MS * 1000000L,
            };
            nanosleep(&pause, NULL);
        }
        printf("Change detected.\n");
    }
}

int main(int argc, char **argv) {
    char const *out_dir = NULL;
    bool watching = false;
    int opt;
    while ((opt = getopt(argc, argv, "wo:h")) != -1) {
        switch (opt) {
            case 'w':
                watching = true;
                break;
            case 'o':
                out_dir = optarg;
                break;
            default:
                usage();
                if (opt == 'h') {
                    return EXIT_SUCCESS;
                }
                return EX_USAGE;
        }
    }
    if (out_dir == NULL || optind != argc - 1) {
        usage();
        return EX_USAGE;
    }
    Str const templates_dir = str_from_cstr(argv[optind]);

    void *memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return EX_OSERR;
    }
    Arena arena;
    arena_init(&arena, memory, MEMORY_SIZE);

    if (watching) {
        watch(&arena, templates_dir, str_from_cstr(out_dir));
        return EXIT_SUCCESS;  // Not reached.
    }
    uint64_t state;
    return build_all(&arena, templates_dir, str_from_cstr(out_dir), &state);
}
