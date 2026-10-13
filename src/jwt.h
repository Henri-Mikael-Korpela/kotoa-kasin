/**
 * @file jwt.h
 * @brief Minimal JSON Web Tokens (RFC 7519) signed with HS256.
 *
 * Only tokens issued by jwt_issue() are accepted by jwt_verify(): the header
 * must be exactly the HS256 header this module writes, which also rules out
 * algorithm confusion ("alg": "none" and friends).
 */

#ifndef __KASIN_JWT_H__
#define __KASIN_JWT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "str.h"

#define JWT_MAX_SUBJECT 64

typedef struct {
    char    subject[JWT_MAX_SUBJECT];
    size_t  subject_len;
    int64_t issued_at;
    int64_t expires_at;
} Jwt_Claims;

/**
 * Appends a signed token to out.
 *
 * @param out Destination; check out->overflow afterwards.
 * @param subject User the token is for; must not contain '"' or '\\'.
 * @param issued_at Unix time the token is issued at.
 * @param expires_at Unix time after which the token is rejected.
 * @param key Signing key.
 * @param key_len Signing key length in bytes.
 * @return False when the subject is invalid or out overflowed.
 */
bool jwt_issue(Str_Builder *out, Str subject, int64_t issued_at, int64_t expires_at,
               uint8_t const *key, size_t key_len);

/**
 * Checks a token's signature and expiry.
 *
 * @param token Token as received, without the "Bearer " prefix.
 * @param key Signing key.
 * @param key_len Signing key length in bytes.
 * @param now Current Unix time.
 * @param claims Receives the token's claims when valid; may be NULL.
 * @return True when the token was issued with this key and has not expired.
 */
bool jwt_verify(Str token, uint8_t const *key, size_t key_len, int64_t now, Jwt_Claims *claims);

#endif  // __KASIN_JWT_H__
