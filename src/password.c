#include "password.h"

#include <assert.h>
#include <string.h>
#include <sys/random.h>

static Str const PREFIX = STR_INIT("pbkdf2-sha256:");
static Str const HEX_DIGITS = STR_INIT("0123456789abcdef");

/**
 * HMAC-SHA256 with the key's two padded blocks hashed once up front, so each
 * message costs two block compressions instead of four. PBKDF2 runs hundreds
 * of thousands of HMACs with the same key.
 */
typedef struct {
    Sha256 inner;  /**< State after the key XOR ipad block. */
    Sha256 outer;  /**< State after the key XOR opad block. */
} Hmac_Key;

static void hmac_key_init(Hmac_Key *k, Str key) {
    assert(k != NULL);
    uint8_t block[SHA256_BLOCK_SIZE];
    memset(block, 0, sizeof block);
    if (key.len > SHA256_BLOCK_SIZE) {
        Sha256 ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, key.data, key.len);
        sha256_final(&ctx, block);
    } else {
        memcpy(block, key.data, key.len);
    }

    uint8_t pad[SHA256_BLOCK_SIZE];
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        pad[i] = block[i] ^ 0x36;
    }
    sha256_init(&k->inner);
    sha256_update(&k->inner, pad, sizeof pad);
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        pad[i] = block[i] ^ 0x5c;
    }
    sha256_init(&k->outer);
    sha256_update(&k->outer, pad, sizeof pad);
}

static void hmac_with_key(Hmac_Key const *k, void const *msg, size_t msg_len,
                          uint8_t out[SHA256_DIGEST_SIZE]) {
    assert(out != NULL);
    Sha256 ctx = k->inner;
    sha256_update(&ctx, msg, msg_len);
    uint8_t inner_hash[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, inner_hash);
    ctx = k->outer;
    sha256_update(&ctx, inner_hash, sizeof inner_hash);
    sha256_final(&ctx, out);
}

void pbkdf2_sha256(Str password, uint8_t const *salt, size_t salt_len, uint32_t iterations,
                   uint8_t out[SHA256_DIGEST_SIZE]) {
    assert(salt != NULL);
    assert(out != NULL);
    Hmac_Key key;
    hmac_key_init(&key, password);

    // The first block only: T1 = U1 ^ U2 ^ ... with U1 = HMAC(P, S || INT(1)).
    Sha256 first = key.inner;
    uint8_t const block_index[4] = {
        0,
        0,
        0,
        1,
    };
    sha256_update(&first, salt, salt_len);
    sha256_update(&first, block_index, sizeof block_index);
    uint8_t inner_hash[SHA256_DIGEST_SIZE];
    sha256_final(&first, inner_hash);
    Sha256 outer = key.outer;
    sha256_update(&outer, inner_hash, sizeof inner_hash);

    uint8_t u[SHA256_DIGEST_SIZE];
    sha256_final(&outer, u);
    memcpy(out, u, sizeof u);
    for (uint32_t i = 1; i < iterations; i++) {
        hmac_with_key(&key, u, sizeof u, u);
        for (size_t b = 0; b < SHA256_DIGEST_SIZE; b++) {
            out[b] ^= u[b];
        }
    }
}

bool password_hash_create(Str password, uint32_t iterations, Password_Hash *out) {
    assert(out != NULL);
    if (iterations < PASSWORD_MIN_ITERATIONS || iterations > PASSWORD_MAX_ITERATIONS) {
        return false;
    }
    if (getrandom(out->salt, sizeof out->salt, 0) != (ssize_t)sizeof out->salt) {
        return false;
    }
    out->iterations = iterations;
    pbkdf2_sha256(password, out->salt, sizeof out->salt, iterations, out->hash);
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_hex(Str text, uint8_t *out, size_t size) {
    assert(out != NULL);
    if (text.len != size * 2) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        int const high = hex_value(text.data[2 * i]);
        int const low = hex_value(text.data[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)(high << 4 | low);
    }
    return true;
}

static void append_hex(Str_Builder *out, uint8_t const *bytes, size_t size) {
    assert(out != NULL);
    for (size_t i = 0; i < size; i++) {
        str_builder_append_char(out, HEX_DIGITS.data[bytes[i] >> 4]);
        str_builder_append_char(out, HEX_DIGITS.data[bytes[i] & 15]);
    }
}

bool password_hash_parse(Str text, Password_Hash *out) {
    assert(out != NULL);
    if (!str_starts_with(text, PREFIX)) {
        return false;
    }
    Str rest = str_slice(text, PREFIX.len, text.len);
    Str iterations_text;
    Str salt_text;
    Str hash_text;
    if (!str_split_once(rest, STR(":"), &iterations_text, &rest) ||
        !str_split_once(rest, STR(":"), &salt_text, &hash_text)) {
        return false;
    }
    uint64_t iterations;
    if (!str_to_u64(iterations_text, &iterations) ||
        iterations < PASSWORD_MIN_ITERATIONS || iterations > PASSWORD_MAX_ITERATIONS) {
        return false;
    }
    if (!parse_hex(salt_text, out->salt, sizeof out->salt) ||
        !parse_hex(hash_text, out->hash, sizeof out->hash)) {
        return false;
    }
    out->iterations = (uint32_t)iterations;
    return true;
}

bool password_hash_format(Password_Hash const *hash, Str_Builder *out) {
    assert(out != NULL);
    str_builder_append(out, PREFIX);
    str_builder_append_u64(out, hash->iterations);
    str_builder_append_char(out, ':');
    append_hex(out, hash->salt, sizeof hash->salt);
    str_builder_append_char(out, ':');
    append_hex(out, hash->hash, sizeof hash->hash);
    return !out->overflow;
}

bool password_verify(Str password, Password_Hash const *hash) {
    uint8_t derived[SHA256_DIGEST_SIZE];
    pbkdf2_sha256(password, hash->salt, sizeof hash->salt, hash->iterations, derived);
    return str_eq_constant_time(str_make((char const *)derived, sizeof derived),
                                str_make((char const *)hash->hash, sizeof hash->hash));
}
