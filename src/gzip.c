#include "gzip.h"

#include <assert.h>
#include <string.h>

enum {
    HASH_BITS = 12,
    HASH_SIZE = 1 << HASH_BITS,  // Entries in the table of last positions.
    MIN_MATCH = 3,  // Bytes; shorter repeats cost more than they save.
    MAX_MATCH = 258,  // Bytes; the longest match deflate can express.
    WINDOW_SIZE = 32768,  // Bytes; the farthest back deflate can reach.
    HEADER_SIZE = 10,  // Bytes of the gzip header.
    TRAILER_SIZE = 8,  // Bytes of the gzip trailer: CRC-32 and length.
    END_OF_BLOCK = 256,  // The deflate symbol that ends a block.
    LENGTH_CODES = 29,
    DISTANCE_CODES = 30,
};

// For each length code (symbol 257 + index): the smallest length it stands for, and how
// many extra bits follow it (RFC 1951, section 3.2.5).
static uint16_t const LENGTH_BASE[LENGTH_CODES] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static byte const LENGTH_EXTRA[LENGTH_CODES] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

// The same for the distance codes.
static uint16_t const DISTANCE_BASE[DISTANCE_CODES] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static byte const DISTANCE_EXTRA[DISTANCE_CODES] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13,
};

/**
 * Writes bits into a buffer, least significant bit first, as deflate wants them.
 */
typedef struct {
    byte *out;
    size_t   capacity;
    size_t   len;
    uint32_t bits;  // Bits not yet written as a whole byte.
    unsigned count;  // How many bits are waiting in bits (always under 8 between calls).
    bool     overflow;  // Set once a byte did not fit.
} Bit_Writer;

static void put_byte(Bit_Writer *w, byte value) {
    if (w->len >= w->capacity) {
        w->overflow = true;
        return;
    }
    w->out[w->len] = value;
    w->len++;
}

/**
 * Appends the low count bits of value (count at most 16).
 */
static void put_bits(Bit_Writer *w, uint32_t value, unsigned count) {
    assert(count <= 16);
    w->bits |= value << w->count;
    w->count += count;
    while (w->count >= 8) {
        put_byte(w, (byte)(w->bits & 0xffu));
        w->bits >>= 8;
        w->count -= 8;
    }
}

/**
 * Appends a Huffman code. Those are packed starting from their most significant bit,
 * so the bits go in reversed.
 */
static void put_code(Bit_Writer *w, uint32_t code, unsigned length) {
    uint32_t reversed = 0;
    for (unsigned i = 0; i < length; i++) {
        reversed = (reversed << 1) | ((code >> i) & 1u);
    }
    put_bits(w, reversed, length);
}

/**
 * Appends a literal or length symbol in the fixed Huffman code (RFC 1951, 3.2.6).
 */
static void put_symbol(Bit_Writer *w, unsigned symbol) {
    if (symbol < 144) {
        put_code(w, 0x30u + symbol, 8);
    } else if (symbol < 256) {
        put_code(w, 0x190u + (symbol - 144), 9);
    } else if (symbol < 280) {
        put_code(w, symbol - 256, 7);
    } else {
        put_code(w, 0xc0u + (symbol - 280), 8);
    }
}

/**
 * Appends a match: how many bytes to copy, from how far back.
 */
static void put_match(Bit_Writer *w, unsigned length, unsigned distance) {
    assert(length >= MIN_MATCH && length <= MAX_MATCH);
    assert(distance >= 1 && distance <= WINDOW_SIZE);
    unsigned code = LENGTH_CODES - 1;
    while (LENGTH_BASE[code] > length) {
        code--;
    }
    put_symbol(w, 257u + code);
    put_bits(w, length - LENGTH_BASE[code], LENGTH_EXTRA[code]);

    unsigned slot = DISTANCE_CODES - 1;
    while (DISTANCE_BASE[slot] > distance) {
        slot--;
    }
    put_code(w, slot, 5);
    put_bits(w, distance - DISTANCE_BASE[slot], DISTANCE_EXTRA[slot]);
}

static uint32_t hash_of(byte const *p) {
    uint32_t const key = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
    return (key * 2654435761u) >> (32 - HASH_BITS);
}

/**
 * How many bytes at the start of a and b are the same, up to limit.
 */
static size_t common_length(byte const *a, byte const *b, size_t limit) {
    size_t n = 0;
    while (n < limit && a[n] == b[n]) {
        n++;
    }
    return n;
}

static uint32_t crc32_of(byte const *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t polynomial = 0;
            if ((crc & 1u) != 0) {
                polynomial = 0xedb88320u;
            }
            crc = (crc >> 1) ^ polynomial;
        }
    }
    return ~crc;
}

static void put_u32_le(Bit_Writer *w, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        put_byte(w, (byte)((value >> (8 * i)) & 0xffu));
    }
}

size_t gzip_compress(Str input, byte *out, size_t capacity) {
    assert(out != NULL);
    if (input.len > GZIP_MAX_INPUT || capacity < HEADER_SIZE + TRAILER_SIZE + 2) {
        return 0;
    }
    byte const *const in = (byte const *)input.data;
    size_t const n = input.len;

    Bit_Writer w = {
        .out = out,
        .capacity = capacity,
        .len = 0,
        .bits = 0,
        .count = 0,
        .overflow = false,
    };
    // Magic, deflate, no flags, no time, no extra flags, Unix.
    static byte const HEADER[HEADER_SIZE] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    for (size_t i = 0; i < HEADER_SIZE; i++) {
        put_byte(&w, HEADER[i]);
    }

    put_bits(&w, 1, 1);  // The last block...
    put_bits(&w, 1, 2);  // ...and it uses the fixed Huffman codes.

    // The position after the latest one seen for each hash; 0 means none.
    uint16_t last[HASH_SIZE];
    memset(last, 0, sizeof last);

    size_t i = 0;
    while (i < n && !w.overflow) {
        size_t length = 0;
        size_t distance = 0;
        if (i + MIN_MATCH <= n) {
            uint32_t const h = hash_of(in + i);
            size_t const found = last[h];
            last[h] = (uint16_t)(i + 1);
            if (found != 0 && i - (found - 1) <= WINDOW_SIZE) {
                size_t limit = MAX_MATCH;
                if (n - i < MAX_MATCH) {
                    limit = n - i;
                }
                length = common_length(in + (found - 1), in + i, limit);
                distance = i - (found - 1);
            }
        }
        if (length >= MIN_MATCH) {
            put_match(&w, (unsigned)length, (unsigned)distance);
            // Remember the positions inside the match too, so later text can find them.
            for (size_t k = 1; k < length && i + k + MIN_MATCH <= n; k++) {
                last[hash_of(in + i + k)] = (uint16_t)(i + k + 1);
            }
            i += length;
        } else {
            put_symbol(&w, in[i]);
            i++;
        }
    }
    put_symbol(&w, END_OF_BLOCK);
    if (w.count > 0) {
        put_bits(&w, 0, 8 - w.count);  // Pad to a whole byte.
    }
    put_u32_le(&w, crc32_of(in, n));
    put_u32_le(&w, (uint32_t)n);
    if (w.overflow) {
        return 0;
    }
    return w.len;
}
