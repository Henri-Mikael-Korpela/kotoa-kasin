/**
 * @file jsbundle.c
 * @brief Builds the site's JavaScript bundles from the sources in js/.
 *
 * Usage: kasin-jsbundle [-n] [-w] -o <out_dir> <manifest>
 *
 * The manifest lists the bundles and, under each, its source files in load
 * order, relative to the manifest's directory:
 *
 *     # Comment.
 *     site:
 *         shared/blocks.js
 *         site/main.js
 *
 * Each bundle is written to <out_dir>/<name>.js. To keep the bundles apart, a
 * bundle may only use sources from shared/ and from the directory named after
 * it: the "site" bundle can never pick up admin code, nor the other way round.
 *
 * Every bundle runs in its own function scope in strict mode, so nothing it
 * declares becomes a global that another bundle could see or overwrite. All
 * files of a bundle share that scope, so the tool refuses to build a bundle in
 * which two files declare the same top-level name.
 *
 * The sources are minified (see jsmin.h) unless -n is given, which keeps them
 * as they are for debugging.
 *
 * With -w the tool keeps running and rebuilds the bundles whenever the manifest
 * or one of its sources changes. Errors are reported and the tool waits for the
 * next change. It polls file times instead of using inotify, which does not see
 * changes made through the Windows bind mounts of Docker Desktop.
 *
 * Like the server, the tool allocates nothing after startup: one mmap()
 * reserves all its memory, carved by an arena.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "jsmin.h"
#include "str.h"

#define MEMORY_SIZE      (64u * 1024 * 1024)
#define MAX_FILE_SIZE    (4u * 1024 * 1024)
#define MAX_BUNDLES      16
#define MAX_SOURCES      64
#define MAX_NAMES        4096
#define PATH_CAPACITY    4096
#define OUTPUT_OVERHEAD  4096  // Header comment, wrapper and separators.
#define WATCH_POLL_MS    300
#define HASH_START       14695981039346656037ull

typedef struct {
    Str    name;
    size_t line;  // Manifest line of the bundle's heading.
    Str    sources[MAX_SOURCES];
    size_t source_count;
} Bundle;

typedef struct {
    Bundle bundles[MAX_BUNDLES];
    size_t count;
} Manifest;

static Str const SHARED_DIR = STR_INIT("shared/");

static void usage(char const *program) {
    fprintf(stderr,
            "Usage: %s [-n] [-w] -o <out_dir> <manifest>\n"
            "  -o DIR  where the bundles are written, as <name>.js\n"
            "  -n      do not minify; keep the sources as they are\n"
            "  -w      keep running; rebuild when the manifest or a source changes\n",
            program);
}

/**
 * Joins a directory and a relative path into a NUL-terminated path for the OS.
 */
static bool join_path(Str dir, Str relative, Str suffix, char *buf, size_t capacity) {
    Str_Builder b;
    str_builder_init(&b, buf, capacity);
    if (dir.len > 0) {
        str_builder_append(&b, dir);
        str_builder_append_char(&b, '/');
    }
    str_builder_append(&b, relative);
    str_builder_append(&b, suffix);
    return str_builder_cstr(&b) != NULL;
}

static Str directory_of(Str path) {
    size_t const slash = str_rfind_char(path, '/');
    return slash == STR_NPOS ? STR(".") : str_slice(path, 0, slash);
}

/**
 * Reads a whole file into the arena.
 */
static bool read_file(Arena *arena, char const *path, Str *contents) {
    int const fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (off_t)MAX_FILE_SIZE) {
        fprintf(stderr, "%s: not a regular file of at most %u bytes\n", path, MAX_FILE_SIZE);
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
            fprintf(stderr, "%s: %s\n", path, n < 0 ? strerror(errno) : "file shrank while reading");
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
    char tmp[PATH_CAPACITY];
    if (!join_path(STR(""), str_from_cstr(path), STR(".tmp"), tmp, sizeof tmp)) {
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

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static bool is_bundle_name(Str name) {
    if (name.len == 0) {
        return false;
    }
    for (size_t i = 0; i < name.len; i++) {
        if (!is_name_char(name.data[i])) {
            return false;
        }
    }
    return true;
}

/**
 * Whether a bundle may use a source: one in shared/ or in the bundle's own
 * directory, ending in .js, without "." or ".." segments that could lead
 * elsewhere.
 */
static bool source_allowed(Str bundle, Str source) {
    Str dir;
    Str rest;
    if (!str_split_once(source, STR("/"), &dir, &rest)) {
        return false;
    }
    if (!str_eq(dir, bundle) && !str_eq(dir, str_slice(SHARED_DIR, 0, SHARED_DIR.len - 1))) {
        return false;
    }
    if (!str_ends_with(rest, STR(".js")) || str_contains(source, STR("\\"))) {
        return false;
    }
    Str segment;
    while (str_split_once(rest, STR("/"), &segment, &rest)) {
        if (segment.len == 0 || str_eq(segment, STR(".")) || str_eq(segment, STR(".."))) {
            return false;
        }
    }
    return true;
}

static bool manifest_error(char const *path, size_t line, char const *message, Str detail) {
    fprintf(stderr, "%s:%zu: %s%.*s\n", path, line, message, STR_ARG(detail));
    return false;
}

static bool parse_manifest(char const *path, Str text, Manifest *manifest) {
    manifest->count = 0;
    Bundle *bundle = NULL;
    size_t line_number = 0;
    Str rest = text;
    while (rest.len > 0) {
        Str line;
        if (!str_split_once(rest, STR("\n"), &line, &rest)) {
            line = rest;
            rest = STR("");
        }
        line_number++;
        line = str_trim(line);
        if (str_ends_with(line, STR("\r"))) {
            line = str_trim(str_slice(line, 0, line.len - 1));
        }
        if (line.len == 0 || line.data[0] == '#') {
            continue;
        }

        if (str_ends_with(line, STR(":"))) {
            Str const name = str_slice(line, 0, line.len - 1);
            if (!is_bundle_name(name)) {
                return manifest_error(path, line_number, "bundle names use a-z, 0-9, - and _: ", name);
            }
            if (str_eq(name, str_slice(SHARED_DIR, 0, SHARED_DIR.len - 1))) {
                return manifest_error(path, line_number, "\"shared\" holds sources for all bundles; it cannot be a bundle", STR(""));
            }
            for (size_t i = 0; i < manifest->count; i++) {
                if (str_eq(manifest->bundles[i].name, name)) {
                    return manifest_error(path, line_number, "bundle listed twice: ", name);
                }
            }
            if (manifest->count == MAX_BUNDLES) {
                return manifest_error(path, line_number, "too many bundles", STR(""));
            }
            bundle = &manifest->bundles[manifest->count++];
            bundle->name = name;
            bundle->line = line_number;
            bundle->source_count = 0;
            continue;
        }

        if (bundle == NULL) {
            return manifest_error(path, line_number, "a source before any bundle heading: ", line);
        }
        if (!source_allowed(bundle->name, line)) {
            fprintf(stderr, "%s:%zu: bundle \"%.*s\" may only use .js files in shared/ and %.*s/, not %.*s\n",
                    path, line_number, STR_ARG(bundle->name), STR_ARG(bundle->name), STR_ARG(line));
            return false;
        }
        for (size_t i = 0; i < bundle->source_count; i++) {
            if (str_eq(bundle->sources[i], line)) {
                return manifest_error(path, line_number, "source listed twice: ", line);
            }
        }
        if (bundle->source_count == MAX_SOURCES) {
            return manifest_error(path, line_number, "too many sources in one bundle", STR(""));
        }
        bundle->sources[bundle->source_count++] = line;
    }

    if (manifest->count == 0) {
        return manifest_error(path, line_number, "no bundles", STR(""));
    }
    for (size_t i = 0; i < manifest->count; i++) {
        if (manifest->bundles[i].source_count == 0) {
            return manifest_error(path, manifest->bundles[i].line, "bundle without sources: ", manifest->bundles[i].name);
        }
    }
    return true;
}

static void print_js_error(Str source_path, Js_Error const *error) {
    if (error->line > 0) {
        fprintf(stderr, "%.*s:%zu: %.*s\n", STR_ARG(source_path), error->line, STR_ARG(error->message));
    } else {
        fprintf(stderr, "%.*s: %.*s\n", STR_ARG(source_path), STR_ARG(error->message));
    }
}

/**
 * Checks that no two sources of a bundle declare the same top-level name.
 */
static bool check_names(Arena *arena, Bundle const *bundle, Str const *contents) {
    Js_Name *names = arena_alloc(arena, MAX_NAMES * sizeof *names, _Alignof(Js_Name));
    size_t *owner = arena_alloc(arena, MAX_NAMES * sizeof *owner, _Alignof(size_t));
    if (names == NULL || owner == NULL) {
        fprintf(stderr, "out of memory\n");
        return false;
    }
    size_t total = 0;
    bool ok = true;
    for (size_t i = 0; i < bundle->source_count; i++) {
        Js_Error error;
        size_t count;
        if (!js_top_level_names(contents[i], names + total, MAX_NAMES - total, &count, &error)) {
            print_js_error(bundle->sources[i], &error);
            return false;
        }
        for (size_t n = total; n < total + count; n++) {
            owner[n] = i;
            for (size_t m = 0; m < n; m++) {
                if (owner[m] != i && str_eq(names[m].name, names[n].name)) {
                    fprintf(stderr, "bundle \"%.*s\": %.*s:%zu and %.*s:%zu both declare \"%.*s\" at the top level\n",
                            STR_ARG(bundle->name),
                            STR_ARG(bundle->sources[owner[m]]), names[m].line,
                            STR_ARG(bundle->sources[i]), names[n].line,
                            STR_ARG(names[n].name));
                    ok = false;
                }
            }
        }
        total += count;
    }
    return ok;
}

/**
 * Builds one bundle: reads and checks its sources and writes <out_dir>/<name>.js.
 *
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int build_bundle(Arena *arena, Bundle const *bundle, Str source_dir, Str out_dir, bool minify) {
    Str contents[MAX_SOURCES];
    size_t input_size = 0;
    for (size_t i = 0; i < bundle->source_count; i++) {
        char path[PATH_CAPACITY];
        if (!join_path(source_dir, bundle->sources[i], STR(""), path, sizeof path)) {
            fprintf(stderr, "%.*s: path too long\n", STR_ARG(bundle->sources[i]));
            return EX_USAGE;
        }
        if (!read_file(arena, path, &contents[i])) {
            return EX_NOINPUT;
        }
        input_size += contents[i].len;
    }
    if (!check_names(arena, bundle, contents)) {
        return EX_DATAERR;
    }

    // Minified code is never longer than its source, so this always fits.
    size_t const capacity = input_size + OUTPUT_OVERHEAD + bundle->source_count * PATH_CAPACITY;
    char *buf = arena_alloc(arena, capacity, 1);
    if (buf == NULL) {
        fprintf(stderr, "bundle \"%.*s\": out of memory\n", STR_ARG(bundle->name));
        return EX_SOFTWARE;
    }
    Str_Builder out;
    str_builder_init(&out, buf, capacity);

    str_builder_append(&out, STR("// Built by kasin-jsbundle from "));
    for (size_t i = 0; i < bundle->source_count; i++) {
        str_builder_append(&out, i == 0 ? STR("js/") : STR(", js/"));
        str_builder_append(&out, bundle->sources[i]);
    }
    str_builder_append(&out, STR(". Edit those, not this file.\n"));
    str_builder_append(&out, minify ? STR("(function(){'use strict';\n") : STR("(function () {\n'use strict';\n"));

    for (size_t i = 0; i < bundle->source_count; i++) {
        if (minify) {
            // Files are separated by a line break, or by ";" where needed (below).
            size_t const separator = out.len;
            if (i > 0) {
                str_builder_append_char(&out, '\n');
            }
            Js_Error error;
            if (!js_minify(contents[i], &out, &error)) {
                print_js_error(bundle->sources[i], &error);
                return EX_DATAERR;
            }
            /*
             * A file that starts with ( [ ` + - or / could continue the
             * previous file's last statement across the line break when that
             * did not end with ";": then the separator becomes ";".
             */
            if (i > 0 && out.len > separator + 1 && !out.overflow &&
                str_find_char(STR("([`+-/"), out.data[separator + 1]) != STR_NPOS &&
                out.data[separator - 1] != ';') {
                out.data[separator] = ';';
            }
        } else {
            str_builder_append(&out, STR("\n// ---- js/"));
            str_builder_append(&out, bundle->sources[i]);
            str_builder_append(&out, STR(" ----\n\n"));
            str_builder_append(&out, contents[i]);
            str_builder_append(&out, STR("\n;"));
        }
    }
    // A last statement without ";" would otherwise run into the wrapper's end.
    Str const written = str_builder_str(&out);
    if (minify && !out.overflow && written.data[written.len - 1] != ';' && written.data[written.len - 1] != '}') {
        str_builder_append_char(&out, ';');
    }
    str_builder_append(&out, STR("\n})();\n"));
    if (out.overflow) {
        fprintf(stderr, "bundle \"%.*s\": output buffer full\n", STR_ARG(bundle->name));
        return EX_SOFTWARE;
    }

    char path[PATH_CAPACITY];
    if (!join_path(out_dir, bundle->name, STR(".js"), path, sizeof path)) {
        fprintf(stderr, "bundle \"%.*s\": output path too long\n", STR_ARG(bundle->name));
        return EX_USAGE;
    }
    if (!write_file(path, str_builder_str(&out))) {
        return EX_CANTCREAT;
    }
    printf("%s: %zu file%s, %zu -> %zu bytes\n", path, bundle->source_count,
           bundle->source_count == 1 ? "" : "s", input_size, out.len);
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
 * Mixes a file's modification time and size into the hash; a missing file
 * counts as a state of its own.
 */
static uint64_t hash_file(uint64_t hash, char const *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return hash_mix(hash, UINT64_MAX);
    }
    hash = hash_mix(hash, (uint64_t)st.st_mtim.tv_sec);
    hash = hash_mix(hash, (uint64_t)st.st_mtim.tv_nsec);
    return hash_mix(hash, (uint64_t)st.st_size);
}

/**
 * Summarizes the state of the manifest and of every source it lists, so a change
 * to any of them changes the value. A change to the manifest itself is a change
 * too, which is how new sources are noticed.
 */
static uint64_t watch_state(char const *manifest_path, Manifest const *manifest) {
    Str const source_dir = directory_of(str_from_cstr(manifest_path));
    uint64_t hash = hash_file(HASH_START, manifest_path);
    for (size_t i = 0; i < manifest->count; i++) {
        for (size_t n = 0; n < manifest->bundles[i].source_count; n++) {
            char path[PATH_CAPACITY];
            if (join_path(source_dir, manifest->bundles[i].sources[n], STR(""), path, sizeof path)) {
                hash = hash_file(hash, path);
            }
        }
    }
    return hash;
}

/**
 * Reads the manifest and builds every bundle.
 *
 * Memory above base is used for the build and given back at the start of the
 * next call. The manifest struct itself is below base and stays valid, so the
 * watcher can look at the sources of the last build.
 *
 * @param state Receives the state of the files as they were before the build,
 *              so a change made during the build causes another build.
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int build_all(Arena *arena, size_t base, Manifest *manifest, char const *manifest_path,
                     Str out_dir, bool minify, uint64_t *state) {
    arena->used = base;
    manifest->count = 0;
    *state = hash_file(HASH_START, manifest_path);
    Str manifest_text;
    if (!read_file(arena, manifest_path, &manifest_text)) {
        return EX_NOINPUT;
    }
    bool const parsed = parse_manifest(manifest_path, manifest_text, manifest);
    *state = watch_state(manifest_path, manifest);
    if (!parsed) {
        return EX_DATAERR;
    }

    Str const source_dir = directory_of(str_from_cstr(manifest_path));
    for (size_t i = 0; i < manifest->count; i++) {
        // Each bundle's files and output are discarded once it is written.
        size_t const mark = arena->used;
        int const status = build_bundle(arena, &manifest->bundles[i], source_dir, out_dir, minify);
        if (status != EXIT_SUCCESS) {
            return status;
        }
        arena->used = mark;
    }
    return EXIT_SUCCESS;
}

/**
 * Rebuilds whenever the files change; does not return.
 */
static void watch(Arena *arena, size_t base, Manifest *manifest, char const *manifest_path,
                 Str out_dir, bool minify) {
    for (;;) {
        uint64_t state;
        int const status = build_all(arena, base, manifest, manifest_path, out_dir, minify, &state);
        if (status == EXIT_SUCCESS) {
            printf("Watching %s and its sources for changes.\n", manifest_path);
        } else {
            printf("Build failed; waiting for changes to %s or its sources.\n", manifest_path);
        }
        fflush(stdout);
        fflush(stderr);

        while (watch_state(manifest_path, manifest) == state) {
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
    char const *out_dir_arg = NULL;
    bool minify = true;
    bool watching = false;
    int opt;
    while ((opt = getopt(argc, argv, "nwo:h")) != -1) {
        switch (opt) {
            case 'n':
                minify = false;
                break;
            case 'w':
                watching = true;
                break;
            case 'o':
                out_dir_arg = optarg;
                break;
            default:
                usage(argv[0]);
                return opt == 'h' ? EXIT_SUCCESS : EX_USAGE;
        }
    }
    if (out_dir_arg == NULL || optind != argc - 1) {
        usage(argv[0]);
        return EX_USAGE;
    }
    char const *manifest_path = argv[optind];

    // The output directory is generated, so it may not exist yet.
    if (mkdir(out_dir_arg, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "%s: %s\n", out_dir_arg, strerror(errno));
        return EX_CANTCREAT;
    }

    void *memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return EX_OSERR;
    }
    Arena arena;
    arena_init(&arena, memory, MEMORY_SIZE);
    Manifest *manifest = arena_alloc(&arena, sizeof *manifest, _Alignof(Manifest));
    if (manifest == NULL) {
        return EX_SOFTWARE;
    }
    size_t const base = arena.used;

    Str const out_dir = str_from_cstr(out_dir_arg);
    if (watching) {
        watch(&arena, base, manifest, manifest_path, out_dir, minify);
        return EXIT_SUCCESS;  // Not reached.
    }
    uint64_t state;
    return build_all(&arena, base, manifest, manifest_path, out_dir, minify, &state);
}
