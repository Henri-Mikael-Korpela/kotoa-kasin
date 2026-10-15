/**
 * @file password.h
 * @brief Salted, slow password hashes (PBKDF2-HMAC-SHA256).
 *
 * A hash is kept as text in the form
 *
 *     pbkdf2-sha256:<iterations>:<salt as hex>:<hash as hex>
 *
 * with a random 16-byte salt and a 32-byte hash. ":" separates the parts
 * rather than the more common "$", so the text can be put in a .env file, a
 * shell or YAML without being taken for a variable.
 *
 * The iteration count makes each guess slow for anyone who gets hold of the
 * hash; the salt makes each hash unique, so guesses cannot be shared between
 * hashes.
 */

#ifndef __KASIN_PASSWORD_H__
#define __KASIN_PASSWORD_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"
#include "str.h"

enum {
    /** Size of the random salt, in bytes. */
    PASSWORD_SALT_SIZE = 16,
    /** Iterations of a new hash: OWASP's recommendation for PBKDF2-HMAC-SHA256. */
    PASSWORD_DEFAULT_ITERATIONS = 600000,
    /** Fewest iterations a hash may have, so a weak hash is never accepted. */
    PASSWORD_MIN_ITERATIONS = 100000,
    /** Most iterations a hash may have, so a login cannot be made to take forever. */
    PASSWORD_MAX_ITERATIONS = 10000000,
    /** Room for the text form of a hash with its terminator, in bytes. */
    PASSWORD_HASH_TEXT_SIZE = 160,
};

typedef struct {
    uint32_t iterations;
    uint8_t  salt[PASSWORD_SALT_SIZE];
    uint8_t  hash[SHA256_DIGEST_SIZE];
} Password_Hash;

/**
 * Derives a 32-byte key with PBKDF2-HMAC-SHA256 (RFC 8018).
 *
 * @param password The password.
 * @param salt The salt.
 * @param salt_len Length of the salt in bytes.
 * @param iterations Number of iterations, at least 1.
 * @param out Receives the derived key.
 */
void pbkdf2_sha256(Str password, uint8_t const *salt, size_t salt_len, uint32_t iterations,
                   uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * Hashes a password with a new random salt.
 *
 * @param iterations Between PASSWORD_MIN_ITERATIONS and PASSWORD_MAX_ITERATIONS.
 * @return False when the salt could not be drawn or iterations is out of range.
 */
bool password_hash_create(Str password, uint32_t iterations, Password_Hash *out);

/**
 * Reads a hash in its text form.
 *
 * @return False when the text is not a well-formed hash.
 */
bool password_hash_parse(Str text, Password_Hash *out);

/**
 * Writes a hash in its text form.
 *
 * @return False when it does not fit.
 */
bool password_hash_format(Password_Hash const *hash, Str_Builder *out);

/**
 * Checks a password against a hash. The comparison takes the same time
 * wherever the derived key differs.
 *
 * @return True when the password matches.
 */
bool password_verify(Str password, Password_Hash const *hash);

#endif  // __KASIN_PASSWORD_H__
