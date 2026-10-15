#include "geoip_build.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "geoip.h"
#include "str.h"

enum {
    /** Most ranges per address family; the CSV has well under a million. */
    MAX_RANGES = 4 * 1024 * 1024,
    /** Longest path the tool builds, with its terminator, in bytes. */
    PATH_CAPACITY = 4096,
};

static uint8_t const UNKNOWN[2] = {
    'Z',
    'Z',
};

/**
 * The ranges of one address family as they are collected.
 */
typedef struct {
    size_t   width;  /**< Bytes per address: 4 or 16. */
    uint8_t *starts;
    uint8_t *countries;
    size_t   count;
    uint8_t  next[16];  /**< Where the next range must start for there to be no gap. */
    bool     seen;  /**< Whether any range of this family was read. */
    bool     full;  /**< Whether the last range reached the highest address. */
} Family;

static bool family_init(Family *f, size_t width) {
    assert(f != NULL);
    f->width = width;
    f->count = 0;
    f->seen = false;
    f->full = false;
    memset(f->next, 0, sizeof f->next);
    // Reserved up front and only touched as filled, like every allocation in this project.
    void *starts = mmap(NULL, MAX_RANGES * width, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    void *countries = mmap(NULL, (size_t)MAX_RANGES * 2, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    f->starts = starts == MAP_FAILED ? NULL : starts;
    f->countries = countries == MAP_FAILED ? NULL : countries;
    if (f->starts == NULL || f->countries == NULL) {
        perror("geoip: mmap");
        return false;
    }
    return true;
}

static void family_free(Family *f) {
    if (f->starts != NULL) {
        munmap(f->starts, MAX_RANGES * f->width);
    }
    if (f->countries != NULL) {
        munmap(f->countries, (size_t)MAX_RANGES * 2);
    }
}

// Adds a range start, or nothing when it continues the previous range's country.
static bool add_start(Family *f, uint8_t const *start, uint8_t const country[2]) {
    assert(f != NULL);
    if (f->count > 0 && memcmp(f->countries + (f->count - 1) * 2, country, 2) == 0) {
        return true;
    }
    if (f->count == MAX_RANGES) {
        return false;
    }
    memcpy(f->starts + f->count * f->width, start, f->width);
    memcpy(f->countries + f->count * 2, country, 2);
    f->count++;
    return true;
}

// Adds one to a big-endian number; returns false when it wraps around to zero.
static bool increment(uint8_t *bytes, size_t width) {
    assert(bytes != NULL);
    for (size_t i = width; i > 0; i--) {
        if (++bytes[i - 1] != 0) {
            return true;
        }
    }
    return false;
}

/**
 * Adds a range, and the gap before it when there is one.
 *
 * @return NULL when the range was added, otherwise what is wrong with it.
 */
static char const *add_range(Family *f, uint8_t const *first, uint8_t const *last,
                             uint8_t const country[2]) {
    assert(f != NULL);
    if (f->full) {
        return "range after the end of the address space";
    }
    int const order = memcmp(first, f->next, f->width);
    if (order < 0) {
        return "ranges overlap or are not sorted";
    }
    if (memcmp(last, first, f->width) < 0) {
        return "range ends before it starts";
    }
    if ((order > 0 && !add_start(f, f->next, UNKNOWN)) || !add_start(f, first, country)) {
        return "too many ranges";
    }
    memcpy(f->next, last, f->width);
    f->full = !increment(f->next, f->width);
    f->seen = true;
    return NULL;
}

// Closes the table: a gap after the last range up to the highest address is unknown.
static bool family_finish(Family *f) {
    return !f->seen || f->full || add_start(f, f->next, UNKNOWN);
}

static bool write_all(int fd, void const *data, size_t len) {
    uint8_t const *bytes = data;
    size_t done = 0;
    while (done < len) {
        ssize_t const n = write(fd, bytes + done, len - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

static void put_u32_le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool write_output(char const *out_path, Family const *v4, Family const *v6) {
    char tmp[PATH_CAPACITY];
    Str_Builder tmp_path;
    str_builder_init(&tmp_path, tmp, sizeof tmp);
    str_builder_append(&tmp_path, str_from_cstr(out_path));
    str_builder_append(&tmp_path, STR(".tmp"));
    if (str_builder_cstr(&tmp_path) == NULL) {
        fprintf(stderr, "geoip: %s: path too long\n", out_path);
        return false;
    }
    int const fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "geoip: cannot create %s: %s\n", tmp, strerror(errno));
        return false;
    }
    uint8_t header[GEOIP_HEADER_SIZE];
    memcpy(header, GEOIP_MAGIC.data, GEOIP_MAGIC.len);
    put_u32_le(header + GEOIP_MAGIC_SIZE, (uint32_t)v4->count);
    put_u32_le(header + GEOIP_MAGIC_SIZE + 4, (uint32_t)v6->count);
    bool ok = write_all(fd, header, sizeof header) &&
              write_all(fd, v4->starts, v4->count * 4) &&
              write_all(fd, v4->countries, v4->count * 2) &&
              write_all(fd, v6->starts, v6->count * 16) &&
              write_all(fd, v6->countries, v6->count * 2);
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp, out_path) != 0) {
        fprintf(stderr, "geoip: cannot write %s: %s\n", out_path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

static Str unquote(Str s) {
    s = str_trim(s);
    if (s.len >= 2 && s.data[0] == '"' && s.data[s.len - 1] == '"') {
        s = str_slice(s, 1, s.len - 1);
    }
    return s;
}

static bool parse_address(Str text, int family, uint8_t *out) {
    char buf[IP_TEXT_SIZE];
    return str_to_cstr(text, buf, sizeof buf) && inet_pton(family, buf, out) == 1;
}

/**
 * Reads one CSV line into the family it belongs to.
 *
 * @return NULL when the line was read, otherwise what is wrong with it.
 */
static char const *parse_csv_line(Str line, Family *v4, Family *v6) {
    Str first_text;
    Str last_text;
    Str country_text;
    Str after;
    if (!str_split_once(line, STR(","), &first_text, &after) ||
        !str_split_once(after, STR(","), &last_text, &country_text)) {
        return "expected first,last,country";
    }
    first_text = unquote(first_text);
    last_text = unquote(last_text);
    country_text = unquote(country_text);

    bool const capitals = country_text.len == 2 &&
                          country_text.data[0] >= 'A' && country_text.data[0] <= 'Z' &&
                          country_text.data[1] >= 'A' && country_text.data[1] <= 'Z';
    if (!capitals) {
        return "country is not two capital letters";
    }
    bool const is_v6 = str_find_char(first_text, ':') != STR_NPOS;
    int const family = is_v6 ? AF_INET6 : AF_INET;
    uint8_t first[16];
    uint8_t last[16];
    if (!parse_address(first_text, family, first) || !parse_address(last_text, family, last)) {
        return "not an IP address";
    }
    return add_range(is_v6 ? v6 : v4, first, last, (uint8_t const *)country_text.data);
}

// Reads the CSV lines into the two families.
static bool read_ranges(Str csv, char const *csv_path, Family *v4, Family *v6) {
    size_t line_number = 0;
    Str rest = csv;
    while (rest.len > 0) {
        Str line;
        if (!str_split_once(rest, STR("\n"), &line, &rest)) {
            line = rest;
            rest = STR("");
        }
        line_number++;
        if (str_ends_with(line, STR("\r"))) {
            line = str_slice(line, 0, line.len - 1);
        }
        if (str_trim(line).len == 0) {
            continue;
        }
        char const *problem = parse_csv_line(line, v4, v6);
        if (problem != NULL) {
            fprintf(stderr, "geoip: %s:%zu: %s\n", csv_path, line_number, problem);
            return false;
        }
    }
    return true;
}

bool geoip_build_file(char const *csv_path, char const *out_path) {
    int const fd = open(csv_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "geoip: cannot open %s: %s\n", csv_path, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        fprintf(stderr, "geoip: %s is empty\n", csv_path);
        close(fd);
        return false;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "geoip: cannot map %s: %s\n", csv_path, strerror(errno));
        return false;
    }
    Str const csv = str_make(map, (size_t)st.st_size);

    Family v4 = {
        .starts = NULL,
        .countries = NULL,
    };
    Family v6 = {
        .starts = NULL,
        .countries = NULL,
    };
    bool ok = family_init(&v4, 4) && family_init(&v6, 16) &&
              read_ranges(csv, csv_path, &v4, &v6) && family_finish(&v4) && family_finish(&v6);
    if (ok && v4.count == 0 && v6.count == 0) {
        fprintf(stderr, "geoip: %s has no ranges\n", csv_path);
        ok = false;
    }
    ok = ok && write_output(out_path, &v4, &v6);
    if (ok) {
        printf("geoip: %s: %zu IPv4 and %zu IPv6 ranges\n", out_path, v4.count, v6.count);
    }
    family_free(&v4);
    family_free(&v6);
    munmap(map, (size_t)st.st_size);
    return ok;
}
