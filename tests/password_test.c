/**
 * @file password_test.c
 * @brief Tests for PBKDF2-HMAC-SHA256 and the password hash text form.
 *
 * Run with ctest, or directly: password-test. Exits with EXIT_FAILURE when a test fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/password.h"

static int FAILURES = 0;

static void check(char const *label, bool ok) {
    if (!ok) {
        printf("FAIL %s\n", label);
        FAILURES++;
    }
}

static void to_hex(uint8_t const *bytes, size_t size, char *out) {
    static char const digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * size] = '\0';
}

// Published PBKDF2-HMAC-SHA256 test vectors with a 32-byte output.
static void expect_pbkdf2(char const *password, char const *salt, uint32_t iterations,
                          char const *expected) {
    uint8_t key[SHA256_DIGEST_SIZE];
    pbkdf2_sha256(str_from_cstr(password), (uint8_t const *)salt, strlen(salt), iterations, key);
    char hex[2 * SHA256_DIGEST_SIZE + 1];
    to_hex(key, sizeof key, hex);
    if (strcmp(hex, expected) != 0) {
        printf("FAIL pbkdf2(%s, %s, %u)\n  got  %s\n  want %s\n",
               password,
               salt,
               iterations,
               hex,
               expected);
        FAILURES++;
    }
}

int main(void) {
    expect_pbkdf2("password",
                  "salt",
                  1,
                  "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    expect_pbkdf2("password",
                  "salt",
                  2,
                  "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    expect_pbkdf2("password",
                  "salt",
                  4096,
                  "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    expect_pbkdf2("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096,
                  "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1");
    // A key longer than one block is hashed first (expected value from Python's hashlib).
    expect_pbkdf2("0123456789012345678901234567890123456789"
                  "012345678901234567890123456789", "salt", 1,
                  "91a3e6790f124879f39ddd8731cd6b0a810b2318c899908a4cad24a4f56d9f6d");

    // Create, write, read back and verify.
    Password_Hash hash;
    check("create",
          password_hash_create(STR("correct horse battery"), PASSWORD_MIN_ITERATIONS, &hash));
    check("right password verifies", password_verify(STR("correct horse battery"), &hash));
    check("wrong password fails", !password_verify(STR("correct horse batterY"), &hash));
    check("empty password fails", !password_verify(STR(""), &hash));

    char text_buf[PASSWORD_HASH_TEXT_SIZE];
    Str_Builder text;
    str_builder_init(&text, text_buf, sizeof text_buf);
    check("format", password_hash_format(&hash, &text));
    check("text form", str_starts_with(str_builder_str(&text), STR("pbkdf2-sha256:100000:")) &&
                       str_builder_str(&text).len == 14 + 6 + 1 + 32 + 1 + 64);
    Password_Hash parsed;
    check("parse back", password_hash_parse(str_builder_str(&text), &parsed));
    check("parsed verifies", password_verify(STR("correct horse battery"), &parsed));

    Password_Hash other;
    check("salts differ", password_hash_create(STR("correct horse battery"),
                                               PASSWORD_MIN_ITERATIONS, &other) &&
                          memcmp(other.salt, hash.salt, sizeof hash.salt) != 0 &&
                          memcmp(other.hash, hash.hash, sizeof hash.hash) != 0);

    // Malformed text is refused.
    char const *bad[] = {
        "",
        "koti",
        "pbkdf2-sha256:600000:00112233445566778899aabbccddeeff",
        "pbkdf2-sha1:600000:00112233445566778899aabbccddeeff:"
        "0011223344556677889900112233445566778899001122334455667788990011",
        "pbkdf2-sha256:99:00112233445566778899aabbccddeeff:"
        "0011223344556677889900112233445566778899001122334455667788990011",
        "pbkdf2-sha256:600000:00112233445566778899AABBCCDDEEFF:"
        "0011223344556677889900112233445566778899001122334455667788990011",
        "pbkdf2-sha256:600000:00112233445566778899aabbccddeeff:00112233",
        "pbkdf2-sha256:600000:00112233445566778899aabbccddeeff:"
        "0011223344556677889900112233445566778899001122334455667788990011:x",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Password_Hash h;
        if (password_hash_parse(str_from_cstr(bad[i]), &h)) {
            printf("FAIL parsed malformed hash: %s\n", bad[i]);
            FAILURES++;
        }
    }

    if (FAILURES > 0) {
        printf("%d failure%s\n", FAILURES, FAILURES == 1 ? "" : "s");
        return EXIT_FAILURE;
    }
    printf("all password tests passed\n");
    return EXIT_SUCCESS;
}
