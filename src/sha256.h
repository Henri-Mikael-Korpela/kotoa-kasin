/**
 * @file sha256.h
 * @brief SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), no dependencies.
 */

#ifndef __KASIN_SHA256_H__
#define __KASIN_SHA256_H__

#include <stddef.h>
#include <stdint.h>

enum {
    /** Size of a SHA-256 digest, in bytes. */
    SHA256_DIGEST_SIZE = 32,
    /** Size of one SHA-256 input block, in bytes. */
    SHA256_BLOCK_SIZE = 64,
};

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t  block[SHA256_BLOCK_SIZE];
    size_t   block_len;
} Sha256;

/**
 * Starts a new hash computation.
 */
void sha256_init(Sha256 *ctx);

/**
 * Feeds bytes into the hash.
 */
void sha256_update(Sha256 *ctx, void const *data, size_t len);

/**
 * Finishes the hash and writes the 32-byte digest.
 */
void sha256_final(Sha256 *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * Computes HMAC-SHA256 of a message with a key of any length.
 */
void hmac_sha256(void const *key, size_t key_len, void const *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

#endif  // __KASIN_SHA256_H__
