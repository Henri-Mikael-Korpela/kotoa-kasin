/**
 * @file gzip.h
 * @brief A small gzip (RFC 1952) compressor, for responses sent to browsers.
 *
 * It finds repeated text with a hash of three bytes (LZ77) and writes the result with
 * the fixed Huffman codes of RFC 1951. That is a little less compact than zlib at its
 * best, but HTML shrinks to a third or so, it needs no allocation and no library, and
 * its work is bounded by the size of the input.
 */

#ifndef __KASIN_GZIP_H__
#define __KASIN_GZIP_H__

#include <stddef.h>
#include <stdint.h>

#include "byte.h"
#include "str.h"

/**
 * Largest input gzip_compress() takes, in bytes. The match finder keeps positions in
 * 16 bits.
 */
enum {
    GZIP_MAX_INPUT = 65534,
};

/**
 * Compresses a string into a complete gzip stream (header, data and checksum).
 *
 * @param input The bytes to compress, at most GZIP_MAX_INPUT.
 * @param out Where to write the stream, which must not overlap input.
 * @param capacity Size of out in bytes.
 * @return The length of the stream, or 0 when the input is too long or the stream does
 *         not fit in out (the caller then sends the input as it is).
 */
size_t gzip_compress(Str input, byte *out, size_t capacity);

#endif  // __KASIN_GZIP_H__
