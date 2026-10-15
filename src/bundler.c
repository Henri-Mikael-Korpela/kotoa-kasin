#include "bundler.h"

#include <assert.h>
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

enum {
    /** Memory the tool reserves at startup, in bytes. */
    MEMORY_SIZE = 64 * 1024 * 1024,
    /** Largest file the tool reads, in bytes. */
    MAX_FILE_SIZE = 4 * 1024 * 1024,
    /** Most bundles in one manifest. */
    MAX_BUNDLES = 16,
    /** Longest path the tool builds, with its terminator, in bytes. */
    PATH_CAPACITY = 4096,
    /** Room in a bundle for its header comment, wrapper and separators, in bytes. */
    OUTPUT_OVERHEAD = 4096,
    /** How often watch mode looks at the files, in milliseconds. */
    WATCH_POLL_MS = 300,
};

/** The FNV-1a offset basis, the start value of the hash of the files' state. */
static uint64_t const HASH_START = 14695981039346656037ull;

typedef struct {
    Bundle bundles[MAX_BUNDLES];
    size_t count;
} Manifest;

/**
 * Everything a build needs, so that its functions take few parameters.
 */
typedef struct {
    Bundle_Kind const *kind;
    Arena             *arena;
    /** Arena use below which the manifest lives; every build starts at this mark. */
    size_t             base;
    Manifest          *manifest;
    char const        *manifest_path;
    Str                out_dir;
    bool               minify;
} Bundler;

/** The directory of sources every bundle may use. */
static Str const SHARED_DIR = STR_INIT("shared");

static void usage(Bundle_Kind const *kind) {
    fprintf(stderr,
            "Usage: %s [-n] [-w] -o <out_dir> <manifest>\n"
            "  -o DIR  where the bundles are written, as <name>%.*s\n"
            "  -n      do not minify; keep the sources as they are\n"
            "  -w      keep running; rebuild when the manifest or a source changes\n",
            kind->program, STR_ARG(kind->extension));
}

/**
 * Joins a directory and a relative path into a NUL-terminated path for the OS.
 */
static bool join_path(Str dir, Str relative, Str suffix, char *buf, size_t capacity) {
    assert(buf != NULL);
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
            char const *reason = n < 0 ? strerror(errno) : "file shrank while reading";
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
 * directory, with the tool's extension, without "." or ".." segments that
 * could lead elsewhere.
 */
static bool source_allowed(Bundle_Kind const *kind, Str bundle, Str source) {
    Str dir;
    Str rest;
    if (!str_split_once(source, STR("/"), &dir, &rest)) {
        return false;
    }
    if (!str_eq(dir, bundle) && !str_eq(dir, SHARED_DIR)) {
        return false;
    }
    if (!str_ends_with(rest, kind->extension) || str_contains(source, STR("\\"))) {
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

static bool parse_manifest(Bundle_Kind const *kind, char const *path, Str text,
                           Manifest *manifest) {
    assert(manifest != NULL);
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
                return manifest_error(path, line_number,
                                      "bundle names use a-z, 0-9, - and _: ", name);
            }
            if (str_eq(name, SHARED_DIR)) {
                return manifest_error(path, line_number,
                                      "\"shared\" holds sources for all bundles; "
                                      "it cannot be a bundle", STR(""));
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
        if (!source_allowed(kind, bundle->name, line)) {
            fprintf(stderr,
                    "%s:%zu: bundle \"%.*s\" may only use %.*s files in shared/ and %.*s/, "
                    "not %.*s\n",
                    path, line_number, STR_ARG(bundle->name), STR_ARG(kind->extension),
                    STR_ARG(bundle->name), STR_ARG(line));
            return false;
        }
        for (size_t i = 0; i < bundle->source_count; i++) {
            if (str_eq(bundle->sources[i], line)) {
                return manifest_error(path, line_number, "source listed twice: ", line);
            }
        }
        if (bundle->source_count == BUNDLER_MAX_SOURCES) {
            return manifest_error(path, line_number, "too many sources in one bundle", STR(""));
        }
        bundle->sources[bundle->source_count++] = line;
    }

    if (manifest->count == 0) {
        return manifest_error(path, line_number, "no bundles", STR(""));
    }
    for (size_t i = 0; i < manifest->count; i++) {
        if (manifest->bundles[i].source_count == 0) {
            return manifest_error(path, manifest->bundles[i].line,
                                  "bundle without sources: ", manifest->bundles[i].name);
        }
    }
    return true;
}

void bundler_print_error(Str source, Minify_Error const *error) {
    assert(error != NULL);
    if (error->line > 0) {
        fprintf(stderr, "%.*s:%zu: %.*s\n", STR_ARG(source), error->line, STR_ARG(error->message));
    } else {
        fprintf(stderr, "%.*s: %.*s\n", STR_ARG(source), STR_ARG(error->message));
    }
}

/**
 * Reads the sources of a bundle into the arena.
 *
 * @param contents Receives the contents of each source, in order.
 * @param input_size Receives the total size of the sources, in bytes.
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int read_sources(Bundler const *b, Bundle const *bundle, Str source_dir, Str *contents,
                        size_t *input_size) {
    assert(contents != NULL);
    assert(input_size != NULL);
    *input_size = 0;
    for (size_t i = 0; i < bundle->source_count; i++) {
        char path[PATH_CAPACITY];
        if (!join_path(source_dir, bundle->sources[i], STR(""), path, sizeof path)) {
            fprintf(stderr, "%.*s: path too long\n", STR_ARG(bundle->sources[i]));
            return EX_USAGE;
        }
        if (!read_file(b->arena, path, &contents[i])) {
            return EX_NOINPUT;
        }
        *input_size += contents[i].len;
    }
    return EXIT_SUCCESS;
}

/**
 * Writes the comment that says where a bundle comes from, and the prologue.
 */
static void write_header(Bundler const *b, Bundle const *bundle, Str_Builder *out) {
    assert(out != NULL);
    Bundle_Kind const *kind = b->kind;
    str_builder_append(out, kind->comment_start);
    str_builder_append(out, STR("Built by "));
    str_builder_append(out, str_from_cstr(kind->program));
    str_builder_append(out, STR(" from "));
    for (size_t i = 0; i < bundle->source_count; i++) {
        if (i > 0) {
            str_builder_append(out, STR(", "));
        }
        str_builder_append(out, kind->source_root);
        str_builder_append_char(out, '/');
        str_builder_append(out, bundle->sources[i]);
    }
    str_builder_append(out, STR(". Edit those, not this file."));
    str_builder_append(out, kind->comment_end);
    str_builder_append(out, b->minify ? kind->prologue : kind->prologue_plain);
}

/**
 * Appends the sources, minified and joined or as they are with a comment before
 * each, and then the epilogue.
 *
 * @return EXIT_SUCCESS, or EX_DATAERR when a source cannot be minified.
 */
static int write_body(Bundler const *b, Bundle const *bundle, Str const *contents,
                      Str_Builder *out) {
    assert(out != NULL);
    Bundle_Kind const *kind = b->kind;
    for (size_t i = 0; i < bundle->source_count; i++) {
        if (b->minify) {
            size_t const separator = out->len;
            if (i > 0) {
                str_builder_append(out, kind->separator);
            }
            Minify_Error error;
            if (!kind->minify(contents[i], out, &error)) {
                bundler_print_error(bundle->sources[i], &error);
                return EX_DATAERR;
            }
            if (kind->after_source != NULL && !out->overflow) {
                kind->after_source(out, separator, i);
            }
        } else {
            str_builder_append_char(out, '\n');
            str_builder_append(out, kind->comment_start);
            str_builder_append(out, STR("---- "));
            str_builder_append(out, kind->source_root);
            str_builder_append_char(out, '/');
            str_builder_append(out, bundle->sources[i]);
            str_builder_append(out, STR(" ----"));
            str_builder_append(out, kind->comment_end);
            str_builder_append_char(out, '\n');
            str_builder_append(out, contents[i]);
            str_builder_append(out, kind->plain_source_end);
        }
    }
    if (b->minify && kind->after_sources != NULL && !out->overflow) {
        kind->after_sources(out);
    }
    str_builder_append(out, b->minify ? kind->epilogue : kind->epilogue_plain);
    return EXIT_SUCCESS;
}

/**
 * Builds one bundle: reads and checks its sources and writes
 * <out_dir>/<name><extension>.
 *
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int build_bundle(Bundler const *b, Bundle const *bundle, Str source_dir) {
    Str contents[BUNDLER_MAX_SOURCES];
    size_t input_size;
    int status = read_sources(b, bundle, source_dir, contents, &input_size);
    if (status != EXIT_SUCCESS) {
        return status;
    }
    if (b->kind->check != NULL && !b->kind->check(b->arena, bundle, contents)) {
        return EX_DATAERR;
    }

    // Minified code is never longer than its source, so this always fits.
    size_t const capacity = input_size + OUTPUT_OVERHEAD + bundle->source_count * PATH_CAPACITY;
    char *buf = arena_alloc(b->arena, capacity, 1);
    if (buf == NULL) {
        fprintf(stderr, "bundle \"%.*s\": out of memory\n", STR_ARG(bundle->name));
        return EX_SOFTWARE;
    }
    Str_Builder out;
    str_builder_init(&out, buf, capacity);
    write_header(b, bundle, &out);
    status = write_body(b, bundle, contents, &out);
    if (status != EXIT_SUCCESS) {
        return status;
    }
    if (out.overflow) {
        fprintf(stderr, "bundle \"%.*s\": output buffer full\n", STR_ARG(bundle->name));
        return EX_SOFTWARE;
    }

    char path[PATH_CAPACITY];
    if (!join_path(b->out_dir, bundle->name, b->kind->extension, path, sizeof path)) {
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
            Str const source = manifest->bundles[i].sources[n];
            if (join_path(source_dir, source, STR(""), path, sizeof path)) {
                hash = hash_file(hash, path);
            }
        }
    }
    return hash;
}

/**
 * Reads the manifest and builds every bundle.
 *
 * Memory above b->base is used for the build and given back at the start of the
 * next call. The manifest struct itself is below it and stays valid, so the
 * watcher can look at the sources of the last build.
 *
 * @param state Receives the state of the files as they were before the build,
 *              so a change made during the build causes another build.
 * @return EXIT_SUCCESS, or the sysexits.h code for what went wrong.
 */
static int build_all(Bundler const *b, uint64_t *state) {
    assert(state != NULL);
    Arena *arena = b->arena;
    Manifest *manifest = b->manifest;
    arena->used = b->base;
    manifest->count = 0;
    *state = hash_file(HASH_START, b->manifest_path);
    Str manifest_text;
    if (!read_file(arena, b->manifest_path, &manifest_text)) {
        return EX_NOINPUT;
    }
    bool const parsed = parse_manifest(b->kind, b->manifest_path, manifest_text, manifest);
    *state = watch_state(b->manifest_path, manifest);
    if (!parsed) {
        return EX_DATAERR;
    }

    Str const source_dir = directory_of(str_from_cstr(b->manifest_path));
    for (size_t i = 0; i < manifest->count; i++) {
        // Each bundle's files and output are discarded once it is written.
        size_t const mark = arena->used;
        int const status = build_bundle(b, &manifest->bundles[i], source_dir);
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
static void watch(Bundler const *b) {
    for (;;) {
        uint64_t state;
        int const status = build_all(b, &state);
        if (status == EXIT_SUCCESS) {
            printf("Watching %s and its sources for changes.\n", b->manifest_path);
        } else {
            printf("Build failed; waiting for changes to %s or its sources.\n",
                   b->manifest_path);
        }
        fflush(stdout);
        fflush(stderr);

        while (watch_state(b->manifest_path, b->manifest) == state) {
            struct timespec const pause = {
                .tv_sec = 0,
                .tv_nsec = WATCH_POLL_MS * 1000000L,
            };
            nanosleep(&pause, NULL);
        }
        printf("Change detected.\n");
    }
}

/**
 * Creates a directory and any missing parents, like mkdir -p.
 */
static bool make_directories(char const *path) {
    assert(path != NULL);
    char buf[PATH_CAPACITY];
    Str const full = str_from_cstr(path);
    for (size_t end = 1; end <= full.len; end++) {
        if (end < full.len && full.data[end] != '/') {
            continue;
        }
        if (!str_to_cstr(str_slice(full, 0, end), buf, sizeof buf)) {
            fprintf(stderr, "%s: path too long\n", path);
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
 * The command line.
 */
typedef struct {
    char const *out_dir;
    char const *manifest_path;
    bool        minify;
    bool        watching;
} Options;

/**
 * Reads the command line.
 *
 * @param options Receives the options.
 * @param status Receives the exit status when the tool should stop right away.
 * @return True when the tool should go on and build.
 */
static bool parse_options(int argc, char **argv, Bundle_Kind const *kind, Options *options,
                          int *status) {
    assert(options != NULL);
    assert(status != NULL);
    *options = (Options) {
        .out_dir = NULL,
        .manifest_path = NULL,
        .minify = true,
        .watching = false,
    };
    int opt;
    while ((opt = getopt(argc, argv, "nwo:h")) != -1) {
        switch (opt) {
            case 'n':
                options->minify = false;
                break;
            case 'w':
                options->watching = true;
                break;
            case 'o':
                options->out_dir = optarg;
                break;
            default:
                usage(kind);
                *status = opt == 'h' ? EXIT_SUCCESS : EX_USAGE;
                return false;
        }
    }
    if (options->out_dir == NULL || optind != argc - 1) {
        usage(kind);
        *status = EX_USAGE;
        return false;
    }
    options->manifest_path = argv[optind];
    return true;
}

int bundler_main(int argc, char **argv, Bundle_Kind const *kind) {
    Options options;
    int status;
    if (!parse_options(argc, argv, kind, &options, &status)) {
        return status;
    }

    // The output directory is generated, so it may not exist yet.
    if (!make_directories(options.out_dir)) {
        return EX_CANTCREAT;
    }

    void *memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
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

    Bundler const bundler = {
        .kind = kind,
        .arena = &arena,
        .base = arena.used,
        .manifest = manifest,
        .manifest_path = options.manifest_path,
        .out_dir = str_from_cstr(options.out_dir),
        .minify = options.minify,
    };
    if (options.watching) {
        watch(&bundler);
        return EXIT_SUCCESS;  // Not reached.
    }
    uint64_t state;
    return build_all(&bundler, &state);
}
