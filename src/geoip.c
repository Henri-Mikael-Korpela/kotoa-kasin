#include "geoip.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t read_u32_le(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void clear(Geoip *g) {
    *g = (Geoip) {
        .map = NULL,
        .map_size = 0,
        .v4_count = 0,
        .v6_count = 0,
        .v4_starts = NULL,
        .v4_countries = NULL,
        .v6_starts = NULL,
        .v6_countries = NULL,
    };
}

bool geoip_open(Geoip *g, char const *path) {
    clear(g);
    int const fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "geoip: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < GEOIP_HEADER_SIZE) {
        fprintf(stderr, "geoip: %s is not a country file\n", path);
        close(fd);
        return false;
    }
    size_t const size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "geoip: cannot map %s: %s\n", path, strerror(errno));
        return false;
    }
    uint8_t const *bytes = map;
    uint64_t const v4 = read_u32_le(bytes + GEOIP_MAGIC_SIZE);
    uint64_t const v6 = read_u32_le(bytes + GEOIP_MAGIC_SIZE + 4);
    uint64_t const expected = GEOIP_HEADER_SIZE + v4 * (4 + 2) + v6 * (16 + 2);
    if (memcmp(bytes, GEOIP_MAGIC, GEOIP_MAGIC_SIZE) != 0 || expected != size) {
        fprintf(stderr, "geoip: %s is not a country file made by kasin-geoip\n", path);
        munmap(map, size);
        return false;
    }
    g->map = bytes;
    g->map_size = size;
    g->v4_count = (uint32_t)v4;
    g->v6_count = (uint32_t)v6;
    g->v4_starts = bytes + GEOIP_HEADER_SIZE;
    g->v4_countries = g->v4_starts + v4 * 4;
    g->v6_starts = g->v4_countries + v4 * 2;
    g->v6_countries = g->v6_starts + v6 * 16;
    return true;
}

void geoip_close(Geoip *g) {
    if (g->map != NULL) {
        munmap((void *)g->map, g->map_size);
    }
    clear(g);
}

/**
 * The index of the last range that starts at or before key, or -1.
 */
static int64_t find_range(uint8_t const *starts, uint32_t count, size_t width, uint8_t const *key) {
    int64_t low = 0;
    int64_t high = (int64_t)count - 1;
    int64_t found = -1;
    while (low <= high) {
        int64_t const mid = low + (high - low) / 2;
        if (memcmp(starts + (size_t)mid * width, key, width) <= 0) {
            found = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return found;
}

bool geoip_country(Geoip const *g, Ip_Address const *ip, char country[2]) {
    if (g->map == NULL) {
        return false;
    }
    bool const v4 = ip_is_v4(ip);
    int64_t const i = v4 ? find_range(g->v4_starts, g->v4_count, 4, ip->bytes + 12)
                         : find_range(g->v6_starts, g->v6_count, 16, ip->bytes);
    if (i < 0) {
        return false;
    }
    uint8_t const *code = (v4 ? g->v4_countries : g->v6_countries) + (size_t)i * 2;
    if (code[0] < 'A' || code[0] > 'Z' || code[1] < 'A' || code[1] > 'Z' || (code[0] == 'Z' && code[1] == 'Z')) {
        return false;
    }
    country[0] = (char)code[0];
    country[1] = (char)code[1];
    return true;
}
