/**
 * @file passwd.c
 * @brief kasin-passwd: makes the admin password hash for KASIN_ADMIN_PASSWORD_HASH.
 *
 * Usage: kasin-passwd [-g] [-i iterations]
 *
 * Asks for the new password twice (without showing it) and prints
 *
 *     KASIN_ADMIN_PASSWORD_HASH=pbkdf2-sha256:...
 *
 * ready to be put in .env (local development) or deploy/secrets.env (Azure).
 * When the input is not a terminal, the password is read from its first line.
 * With -g a strong random password is made instead and shown once on stderr.
 *
 * The hash is salted and slow (see password.h); only the hash is stored, so
 * the password itself is kept nowhere.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sysexits.h>
#include <termios.h>
#include <unistd.h>

#include "password.h"
#include "str.h"

#define MIN_PASSWORD_LENGTH 12
#define MAX_PASSWORD_LENGTH 128
#define GENERATED_LENGTH    24

/**
 * Letters and digits for generated passwords, without ones easily confused
 * when read or typed: 0/O, 1/l/I.
 */
static Str const GENERATED_ALPHABET = STR_INIT("abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789");

static void usage(char const *program) {
    fprintf(stderr,
            "Usage: %s [-g] [-i iterations]\n"
            "  Prints KASIN_ADMIN_PASSWORD_HASH=<hash> for the password typed in.\n"
            "  -g  make a strong random password instead, shown once on stderr\n"
            "  -i  PBKDF2 iterations (default %d, at least %d)\n",
            program, PASSWORD_DEFAULT_ITERATIONS, PASSWORD_MIN_ITERATIONS);
}

/**
 * Reads one line into buf, without its line break.
 *
 * @return False at end of input or when the line does not fit.
 */
static bool read_line(char *buf, size_t capacity, size_t *len) {
    if (fgets(buf, (int)capacity, stdin) == NULL) {
        return false;
    }
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') {
        n--;
    } else if (!feof(stdin)) {
        return false;  // Longer than the buffer.
    }
    if (n > 0 && buf[n - 1] == '\r') {
        n--;
    }
    *len = n;
    return true;
}

/**
 * Asks for a password on the terminal with echo turned off.
 */
static bool prompt_password(char const *prompt, char *buf, size_t capacity, size_t *len) {
    fputs(prompt, stderr);
    fflush(stderr);
    struct termios saved;
    bool const terminal = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (terminal) {
        struct termios quiet = saved;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    bool const ok = read_line(buf, capacity, len);
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        fputc('\n', stderr);
    }
    return ok;
}

/**
 * Fills buf with a random password from GENERATED_ALPHABET. Bytes that would
 * make some letters more likely than others are drawn again.
 */
static bool generate_password(char *buf, size_t length) {
    size_t const limit = 256 - 256 % GENERATED_ALPHABET.len;
    size_t filled = 0;
    while (filled < length) {
        uint8_t random[64];
        if (getrandom(random, sizeof random, 0) != (ssize_t)sizeof random) {
            return false;
        }
        for (size_t i = 0; i < sizeof random && filled < length; i++) {
            if (random[i] < limit) {
                buf[filled++] = GENERATED_ALPHABET.data[random[i] % GENERATED_ALPHABET.len];
            }
        }
    }
    return true;
}

int main(int argc, char **argv) {
    bool generate = false;
    uint64_t iterations = PASSWORD_DEFAULT_ITERATIONS;
    int opt;
    while ((opt = getopt(argc, argv, "gi:h")) != -1) {
        switch (opt) {
            case 'g':
                generate = true;
                break;
            case 'i':
                if (!str_to_u64(str_from_cstr(optarg), &iterations) ||
                    iterations < PASSWORD_MIN_ITERATIONS || iterations > PASSWORD_MAX_ITERATIONS) {
                    fprintf(stderr, "iterations must be between %d and %d\n",
                            PASSWORD_MIN_ITERATIONS, PASSWORD_MAX_ITERATIONS);
                    return EX_USAGE;
                }
                break;
            default:
                usage(argv[0]);
                return opt == 'h' ? EXIT_SUCCESS : EX_USAGE;
        }
    }
    if (optind != argc) {
        usage(argv[0]);
        return EX_USAGE;
    }

    char password[MAX_PASSWORD_LENGTH + 2];  // The line break and the terminator.
    size_t length = 0;
    if (generate) {
        length = GENERATED_LENGTH;
        if (!generate_password(password, length)) {
            perror("getrandom");
            return EX_OSERR;
        }
        fprintf(stderr, "Password: %.*s\nStore it in a password manager; it is not shown again.\n",
                (int)length, password);
    } else if (isatty(STDIN_FILENO)) {
        char again[MAX_PASSWORD_LENGTH + 2];
        size_t again_length = 0;
        if (!prompt_password("New admin password: ", password, sizeof password, &length) ||
            !prompt_password("Again: ", again, sizeof again, &again_length)) {
            fprintf(stderr, "no password read, or longer than %d characters\n", MAX_PASSWORD_LENGTH);
            return EX_DATAERR;
        }
        bool const same = str_eq(str_make(password, length), str_make(again, again_length));
        explicit_bzero(again, sizeof again);
        if (!same) {
            fprintf(stderr, "the passwords differ\n");
            explicit_bzero(password, sizeof password);
            return EX_DATAERR;
        }
    } else if (!read_line(password, sizeof password, &length)) {
        fprintf(stderr, "no password read, or longer than %d characters\n", MAX_PASSWORD_LENGTH);
        return EX_DATAERR;
    }

    if (length < MIN_PASSWORD_LENGTH) {
        fprintf(stderr, "use at least %d characters; a few unrelated words make a good password\n",
                MIN_PASSWORD_LENGTH);
        explicit_bzero(password, sizeof password);
        return EX_DATAERR;
    }

    Password_Hash hash;
    bool const created = password_hash_create(str_make(password, length), (uint32_t)iterations, &hash);
    explicit_bzero(password, sizeof password);
    if (!created) {
        perror("getrandom");
        return EX_OSERR;
    }
    char text_buf[PASSWORD_HASH_TEXT_SIZE];
    Str_Builder text;
    str_builder_init(&text, text_buf, sizeof text_buf);
    if (!password_hash_format(&hash, &text)) {
        return EX_SOFTWARE;
    }
    printf("KASIN_ADMIN_PASSWORD_HASH=%.*s\n", STR_ARG(str_builder_str(&text)));
    return EXIT_SUCCESS;
}
