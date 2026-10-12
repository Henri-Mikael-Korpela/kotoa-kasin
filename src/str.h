/**
 * @file str.h
 * @brief Length-based string slices that do not rely on NUL terminators.
 *
 * A Str never owns its bytes. It points into memory owned by someone else
 * (a string literal, a request buffer, an arena block) and carries its own
 * length, so slicing and splitting never copy or allocate.
 *
 * A Str_Builder appends into a fixed caller-provided buffer and never grows.
 */

#ifndef STR_H
#define STR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char const *data;
    size_t      len;
} Str;

typedef struct {
    char   *data;
    size_t  len;
    size_t  capacity;
    bool    overflow;  /**< Set once an append did not fit. */
} Str_Builder;

/**
 * Returned by the search functions when nothing was found.
 */
#define STR_NPOS SIZE_MAX

/**
 * Initializer for a Str from a string literal, usable in static initializers.
 */
#define STR_INIT(literal) {                \
        .data = (literal),                 \
        .len = sizeof (literal) - 1,       \
    }

/**
 * Str value from a string literal, usable in expressions.
 */
#define STR(literal) ((Str) STR_INIT(literal))

/**
 * printf support: printf("%.*s", STR_ARG(s)).
 */
#define STR_ARG(s) (int)(s).len, (s).data

/**
 * Makes a Str from a pointer and a length.
 *
 * @param data First byte of the string.
 * @param len Number of bytes.
 * @return The slice.
 */
Str str_make(char const *data, size_t len);

/**
 * Makes a Str from a NUL-terminated C string, for OS and libc boundaries.
 *
 * @param cstr NUL-terminated string, may be NULL.
 * @return The slice, without the terminator.
 */
Str str_from_cstr(char const *cstr);

/**
 * Returns the bytes in [start, end), clamped to the string.
 *
 * @param s Source string.
 * @param start First byte index.
 * @param end One past the last byte index.
 * @return The sub-slice.
 */
Str str_slice(Str s, size_t start, size_t end);

/**
 * Removes leading and trailing spaces and tabs.
 *
 * @param s Source string.
 * @return The trimmed slice.
 */
Str str_trim(Str s);

/**
 * Compares two strings byte for byte.
 *
 * @return True when both have the same length and bytes.
 */
bool str_eq(Str a, Str b);

/**
 * Compares two strings ignoring ASCII case.
 *
 * @return True when both are equal ignoring ASCII case.
 */
bool str_eq_nocase(Str a, Str b);

/**
 * Checks whether s ends with suffix.
 *
 * @return True when the last bytes of s equal suffix.
 */
bool str_ends_with(Str s, Str suffix);

/**
 * Finds the first occurrence of a byte.
 *
 * @return Its index, or STR_NPOS.
 */
size_t str_find_char(Str s, char c);

/**
 * Finds the last occurrence of a byte.
 *
 * @return Its index, or STR_NPOS.
 */
size_t str_rfind_char(Str s, char c);

/**
 * Finds the first byte of s that appears anywhere in chars.
 *
 * @return Its index, or STR_NPOS.
 */
size_t str_find_any(Str s, Str chars);

/**
 * Finds the first occurrence of needle in haystack.
 *
 * @return Its index, or STR_NPOS. An empty needle is found at index 0.
 */
size_t str_find(Str haystack, Str needle);

/**
 * Checks whether needle occurs in haystack.
 *
 * @return True when found.
 */
bool str_contains(Str haystack, Str needle);

/**
 * Checks whether needle occurs in haystack, ignoring ASCII case.
 *
 * @return True when found.
 */
bool str_contains_nocase(Str haystack, Str needle);

/**
 * Splits s around the first occurrence of sep.
 *
 * @param s String to split.
 * @param sep Separator to look for.
 * @param before Receives the bytes before sep.
 * @param after Receives the bytes after sep.
 * @return False when sep is not found; before and after are then left untouched.
 */
bool str_split_once(Str s, Str sep, Str *before, Str *after);

/**
 * Parses an unsigned decimal number made only of digits.
 *
 * @param s Digits, without sign or surrounding whitespace.
 * @param out Receives the value on success.
 * @return False when s is empty, has a non-digit or overflows.
 */
bool str_to_u64(Str s, uint64_t *out);

/**
 * Copies s into buf and NUL-terminates it, for OS and libc boundaries.
 *
 * @param s Source string.
 * @param buf Destination buffer.
 * @param capacity Size of buf in bytes, including room for the terminator.
 * @return False when s does not fit or contains a NUL byte.
 */
bool str_to_cstr(Str s, char *buf, size_t capacity);

/**
 * Starts an empty builder over a fixed buffer.
 *
 * @param b Builder to initialize.
 * @param buf Destination buffer, owned by the caller.
 * @param capacity Size of buf in bytes.
 */
void str_builder_init(Str_Builder *b, char *buf, size_t capacity);

/**
 * Appends a string. Nothing is written if it does not fit.
 *
 * @return False, and sets overflow, when s does not fit.
 */
bool str_builder_append(Str_Builder *b, Str s);

/**
 * Appends an unsigned number in decimal. Nothing is written if it does not fit.
 *
 * @return False, and sets overflow, when the digits do not fit.
 */
bool str_builder_append_u64(Str_Builder *b, uint64_t value);

/**
 * Returns the bytes written so far.
 *
 * @return A slice over the builder's buffer.
 */
Str str_builder_str(Str_Builder const *b);

#endif
