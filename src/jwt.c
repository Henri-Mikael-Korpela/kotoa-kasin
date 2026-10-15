#include "jwt.h"

#include <assert.h>
#include <string.h>

#include "sha256.h"

// base64url('{"alg":"HS256","typ":"JWT"}').
static Str const HEADER_B64 = STR_INIT("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");

static Str const BASE64URL = STR_INIT(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");

enum {
    /** Largest token payload, before base64url, in bytes. */
    MAX_PAYLOAD = 256,
};

// Appends the base64url character for the low six bits of a value.
static void append_sextet(Str_Builder *out, uint32_t value) {
    str_builder_append_char(out, BASE64URL.data[value & 63]);
}

static bool base64url_encode(Str_Builder *out, uint8_t const *data, size_t len) {
    assert(out != NULL);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
        append_sextet(out, v >> 18);
        append_sextet(out, v >> 12);
        append_sextet(out, v >> 6);
        append_sextet(out, v);
        i += 3;
    }
    size_t rest = len - i;
    if (rest == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        append_sextet(out, v >> 18);
        append_sextet(out, v >> 12);
    } else if (rest == 2) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8;
        append_sextet(out, v >> 18);
        append_sextet(out, v >> 12);
        append_sextet(out, v >> 6);
    }
    return !out->overflow;
}

static int base64url_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

static bool base64url_decode(Str in, uint8_t *out, size_t capacity, size_t *out_len) {
    assert(out != NULL);
    assert(out_len != NULL);
    if (in.len % 4 == 1) {
        return false;
    }
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < in.len; i++) {
        int v = base64url_value(in.data[i]);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == capacity) {
                return false;
            }
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = n;
    return true;
}

static bool sign(Str_Builder *out, Str signing_input, Str key) {
    uint8_t mac[SHA256_DIGEST_SIZE];
    hmac_sha256(key.data, key.len, signing_input.data, signing_input.len, mac);
    return base64url_encode(out, mac, sizeof mac);
}

// Reads a non-negative integer claim like "exp":123 from a payload we issued.
static bool claim_int(Str payload, Str name, int64_t *value) {
    assert(value != NULL);
    size_t at = str_find(payload, name);
    if (at == STR_NPOS) {
        return false;
    }
    Str rest = str_slice(payload, at + name.len, payload.len);
    size_t end = 0;
    while (end < rest.len && rest.data[end] >= '0' && rest.data[end] <= '9') {
        end++;
    }
    uint64_t parsed;
    if (!str_to_u64(str_slice(rest, 0, end), &parsed) || parsed > INT64_MAX) {
        return false;
    }
    *value = (int64_t)parsed;
    return true;
}

bool jwt_issue(Str_Builder *out, Str subject, int64_t issued_at, int64_t expires_at, Str key) {
    assert(out != NULL);
    if (subject.len == 0 || subject.len >= JWT_MAX_SUBJECT ||
        str_find_any(subject, STR("\"\\")) != STR_NPOS || issued_at < 0 || expires_at < 0) {
        return false;
    }

    char payload_buf[MAX_PAYLOAD];
    Str_Builder payload;
    str_builder_init(&payload, payload_buf, sizeof payload_buf);
    str_builder_append(&payload, STR("{\"sub\":\""));
    str_builder_append(&payload, subject);
    str_builder_append(&payload, STR("\",\"iat\":"));
    str_builder_append_u64(&payload, (uint64_t)issued_at);
    str_builder_append(&payload, STR(",\"exp\":"));
    str_builder_append_u64(&payload, (uint64_t)expires_at);
    str_builder_append(&payload, STR("}"));
    if (payload.overflow) {
        return false;
    }

    size_t start = out->len;
    str_builder_append(out, HEADER_B64);
    str_builder_append(out, STR("."));
    base64url_encode(out, (uint8_t const *)payload.data, payload.len);
    if (out->overflow) {
        return false;
    }
    Str signing_input = str_make(out->data + start, out->len - start);
    str_builder_append(out, STR("."));
    return sign(out, signing_input, key);
}

bool jwt_verify(Str token, Str key, int64_t now, Jwt_Claims *claims) {
    Str header;
    Str rest;
    Str payload_b64;
    Str signature;
    if (!str_split_once(token, STR("."), &header, &rest) ||
        !str_split_once(rest, STR("."), &payload_b64, &signature) ||
        !str_eq(header, HEADER_B64)) {
        return false;
    }

    char expected_buf[64];
    Str_Builder expected;
    str_builder_init(&expected, expected_buf, sizeof expected_buf);
    Str signing_input = str_make(token.data, header.len + 1 + payload_b64.len);
    if (!sign(&expected, signing_input, key) ||
        !str_eq_constant_time(str_builder_str(&expected), signature)) {
        return false;
    }

    uint8_t payload_buf[MAX_PAYLOAD];
    size_t payload_len;
    if (!base64url_decode(payload_b64, payload_buf, sizeof payload_buf, &payload_len)) {
        return false;
    }
    Str payload = str_make((char const *)payload_buf, payload_len);

    int64_t issued_at;
    int64_t expires_at;
    Str subject_key = STR("\"sub\":\"");
    size_t sub_at = str_find(payload, subject_key);
    if (!claim_int(payload, STR("\"iat\":"), &issued_at) ||
        !claim_int(payload, STR("\"exp\":"), &expires_at) || sub_at == STR_NPOS) {
        return false;
    }
    if (now >= expires_at) {
        return false;
    }

    Str subject = str_slice(payload, sub_at + subject_key.len, payload.len);
    size_t quote = str_find_char(subject, '"');
    if (quote == STR_NPOS || quote >= JWT_MAX_SUBJECT) {
        return false;
    }
    if (claims) {
        memcpy(claims->subject, subject.data, quote);
        claims->subject[quote] = '\0';
        claims->subject_len = quote;
        claims->issued_at = issued_at;
        claims->expires_at = expires_at;
    }
    return true;
}
