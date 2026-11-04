/**
 * @file gzip_test.c
 * @brief Tests for the gzip compressor: it is decoded again here and compared.
 *
 * Run with ctest, or directly: gzip-test. Exits with EXIT_FAILURE when a test fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gzip.h"

static int FAILURES = 0;

static void check(char const *label, bool ok) {
    if (!ok) {
        printf("FAIL %s\n", label);
        FAILURES++;
    }
}

// Same tables as RFC 1951 gives; the compressor has its own copy, this is the decoder's.
static unsigned const LENGTH_BASE[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static unsigned const LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static unsigned const DISTANCE_BASE[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static unsigned const DISTANCE_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

typedef struct {
    uint8_t const *data;
    size_t         size;
    size_t         bit;  // Index of the next bit.
    bool           failed;
} Bit_Reader;

static unsigned read_bit(Bit_Reader *r) {
    if (r->bit / 8 >= r->size) {
        r->failed = true;
        return 0;
    }
    unsigned const value = (r->data[r->bit / 8] >> (r->bit % 8)) & 1u;
    r->bit++;
    return value;
}

static unsigned read_bits(Bit_Reader *r, unsigned count) {
    unsigned value = 0;
    for (unsigned i = 0; i < count; i++) {
        value |= read_bit(r) << i;
    }
    return value;
}

/**
 * Reads a literal or length symbol written with the fixed Huffman code.
 *
 * @return The symbol, or 0xffff when the bits are not a code.
 */
static unsigned read_symbol(Bit_Reader *r) {
    unsigned code = 0;
    for (unsigned length = 1; length <= 9; length++) {
        code = (code << 1) | read_bit(r);
        if (length == 7 && code <= 0x17) {
            return 256 + code;
        }
        if (length == 8 && code >= 0x30 && code <= 0xbf) {
            return code - 0x30;
        }
        if (length == 8 && code >= 0xc0 && code <= 0xc7) {
            return 280 + (code - 0xc0);
        }
        if (length == 9 && code >= 0x190 && code <= 0x1ff) {
            return 144 + (code - 0x190);
        }
    }
    return 0xffff;
}

/**
 * Decodes a gzip stream made of one fixed Huffman block, and checks its header and
 * trailer.
 *
 * @return The decoded length, or SIZE_MAX when the stream is not valid.
 */
static size_t gunzip(uint8_t const *stream, size_t size, uint8_t *out, size_t capacity) {
    if (size < 18 || stream[0] != 0x1f || stream[1] != 0x8b || stream[2] != 8 || stream[3] != 0) {
        return SIZE_MAX;
    }
    Bit_Reader r = {
        .data = stream + 10,
        .size = size - 18,
        .bit = 0,
        .failed = false,
    };
    if (read_bits(&r, 1) != 1 || read_bits(&r, 2) != 1) {
        return SIZE_MAX;
    }
    size_t len = 0;
    for (;;) {
        unsigned const symbol = read_symbol(&r);
        if (r.failed || symbol == 0xffff) {
            return SIZE_MAX;
        }
        if (symbol == 256) {
            break;
        }
        if (symbol < 256) {
            if (len >= capacity) {
                return SIZE_MAX;
            }
            out[len++] = (uint8_t)symbol;
            continue;
        }
        if (symbol > 285) {
            return SIZE_MAX;
        }
        unsigned const length = LENGTH_BASE[symbol - 257] + read_bits(&r, LENGTH_EXTRA[symbol - 257]);
        unsigned slot = 0;
        for (unsigned i = 0; i < 5; i++) {
            slot = (slot << 1) | read_bit(&r);
        }
        if (slot >= 30) {
            return SIZE_MAX;
        }
        unsigned const distance = DISTANCE_BASE[slot] + read_bits(&r, DISTANCE_EXTRA[slot]);
        if (distance > len || len + length > capacity) {
            return SIZE_MAX;
        }
        for (unsigned i = 0; i < length; i++) {
            out[len] = out[len - distance];
            len++;
        }
    }
    // Whole bytes only after the padding, then the trailer, whose length must match.
    size_t const used = (r.bit + 7) / 8;
    uint8_t const *trailer = stream + 10 + used;
    if (10 + used + 8 != size) {
        return SIZE_MAX;
    }
    uint32_t const stored = (uint32_t)trailer[4] | ((uint32_t)trailer[5] << 8) |
                            ((uint32_t)trailer[6] << 16) | ((uint32_t)trailer[7] << 24);
    return stored == len ? len : SIZE_MAX;
}

static uint32_t crc_in_trailer(uint8_t const *stream, size_t size) {
    uint8_t const *t = stream + size - 8;
    return (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
}

static void expect_round_trip(char const *label, uint8_t const *input, size_t len) {
    static uint8_t packed[200000];
    static uint8_t unpacked[200000];
    size_t const packed_len = gzip_compress(str_make((char const *)input, len),
                                            packed,
                                            sizeof packed);
    check(label, packed_len > 0);
    if (packed_len == 0) {
        return;
    }
    size_t const got = gunzip(packed, packed_len, unpacked, sizeof unpacked);
    bool const same = got == len && memcmp(unpacked, input, len) == 0;
    if (!same) {
        printf("FAIL %s: decoded %zu bytes of %zu, or they differ\n", label, got, len);
        FAILURES++;
    }
}

static void expect_text_round_trip(char const *label, char const *text) {
    expect_round_trip(label, (uint8_t const *)text, strlen(text));
}

int main(void) {
    expect_text_round_trip("empty", "");
    expect_text_round_trip("one byte", "a");
    expect_text_round_trip("short", "abc");
    expect_text_round_trip("text", "The quick brown fox jumps over the lazy dog.");
    expect_text_round_trip("repeats",
                           "<li><a href=\"/a\">A</a></li><li><a href=\"/b\">B</a></li>"
                           "<li><a href=\"/c\">C</a></li><li><a href=\"/d\">D</a></li>");

    // A long run is one byte followed by a match that overlaps itself, at the longest length.
    static uint8_t run[5000];
    memset(run, 'x', sizeof run);
    expect_round_trip("run", run, sizeof run);

    // Bytes that do not repeat: the stream is longer than the input, but still valid.
    static uint8_t noise[20000];
    uint32_t state = 12345;
    for (size_t i = 0; i < sizeof noise; i++) {
        state = state * 1664525u + 1013904223u;
        noise[i] = (uint8_t)(state >> 24);
    }
    expect_round_trip("noise", noise, sizeof noise);

    // Matches reach back as far as the window allows.
    static uint8_t far[40000];
    for (size_t i = 0; i < sizeof far; i++) {
        state = state * 1664525u + 1013904223u;
        far[i] = (uint8_t)(i < 30000 ? state >> 24 : far[i - 30000]);
    }
    expect_round_trip("far", far, sizeof far);

    // HTML-like text must get much smaller.
    static char page[20000];
    size_t used = 0;
    while (used + 80 < sizeof page) {
        used += (size_t)snprintf(page + used,
                                 sizeof page - used,
                                 "<div class=\"card\"><a href=\"/reseptit/%zu\">Resepti %zu</a></div>\n",
                                 used % 7,
                                 used % 13);
    }
    static uint8_t packed[GZIP_MAX_INPUT];
    size_t const packed_len = gzip_compress(str_make(page, used), packed, sizeof packed);
    check("html shrinks", packed_len > 0 && packed_len < used / 3);

    // The checksum is the standard CRC-32: its check value for "123456789".
    size_t const crc_len = gzip_compress(STR("123456789"), packed, sizeof packed);
    check("crc-32", crc_len > 0 && crc_in_trailer(packed, crc_len) == 0xcbf43926u);

    // Too small a buffer is refused, not overrun.
    uint8_t small[32];
    check("does not fit", gzip_compress(str_make((char const *)noise, 1000), small, sizeof small) == 0);
    check("too long", gzip_compress(str_make(page, GZIP_MAX_INPUT + 1), packed, sizeof packed) == 0);

    if (FAILURES == 0) {
        printf("all gzip tests passed\n");
    }
    return FAILURES == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
