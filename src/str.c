#include "str.h"

#include <string.h>

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

Str str_make(char const *data, size_t len) {
    return (Str) {
        .data = data,
        .len = len,
    };
}

Str str_from_cstr(char const *cstr) {
    if (!cstr) {
        return str_make("", 0);
    }
    return str_make(cstr, strlen(cstr));
}

Str str_slice(Str s, size_t start, size_t end) {
    if (end > s.len) {
        end = s.len;
    }
    if (start > end) {
        start = end;
    }
    return str_make(s.data + start, end - start);
}

Str str_trim(Str s) {
    size_t start = 0;
    size_t end = s.len;
    while (start < end && (s.data[start] == ' ' || s.data[start] == '\t')) {
        start++;
    }
    while (end > start && (s.data[end - 1] == ' ' || s.data[end - 1] == '\t')) {
        end--;
    }
    return str_slice(s, start, end);
}

bool str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

bool str_eq_nocase(Str a, Str b) {
    if (a.len != b.len) {
        return false;
    }
    for (size_t i = 0; i < a.len; i++) {
        if (ascii_lower(a.data[i]) != ascii_lower(b.data[i])) {
            return false;
        }
    }
    return true;
}

bool str_ends_with(Str s, Str suffix) {
    return s.len >= suffix.len && str_eq(str_slice(s, s.len - suffix.len, s.len), suffix);
}

size_t str_find_char(Str s, char c) {
    if (s.len == 0) {
        return STR_NPOS;
    }
    char const *hit = memchr(s.data, c, s.len);
    return hit ? (size_t)(hit - s.data) : STR_NPOS;
}

size_t str_rfind_char(Str s, char c) {
    for (size_t i = s.len; i > 0; i--) {
        if (s.data[i - 1] == c) {
            return i - 1;
        }
    }
    return STR_NPOS;
}

size_t str_find_any(Str s, Str chars) {
    for (size_t i = 0; i < s.len; i++) {
        if (str_find_char(chars, s.data[i]) != STR_NPOS) {
            return i;
        }
    }
    return STR_NPOS;
}

size_t str_find(Str haystack, Str needle) {
    if (needle.len == 0) {
        return 0;
    }
    size_t pos = 0;
    while (haystack.len - pos >= needle.len) {
        // Jump to the next candidate first byte, then compare the rest.
        Str rest = str_slice(haystack, pos, haystack.len - needle.len + 1);
        size_t hit = str_find_char(rest, needle.data[0]);
        if (hit == STR_NPOS) {
            return STR_NPOS;
        }
        pos += hit;
        if (memcmp(haystack.data + pos, needle.data, needle.len) == 0) {
            return pos;
        }
        pos++;
    }
    return STR_NPOS;
}

bool str_contains(Str haystack, Str needle) {
    return str_find(haystack, needle) != STR_NPOS;
}

bool str_contains_nocase(Str haystack, Str needle) {
    for (size_t i = 0; i + needle.len <= haystack.len; i++) {
        if (str_eq_nocase(str_slice(haystack, i, i + needle.len), needle)) {
            return true;
        }
    }
    return false;
}

bool str_split_once(Str s, Str sep, Str *before, Str *after) {
    size_t pos = str_find(s, sep);
    if (pos == STR_NPOS) {
        return false;
    }
    *before = str_slice(s, 0, pos);
    *after = str_slice(s, pos + sep.len, s.len);
    return true;
}

bool str_to_u64(Str s, uint64_t *out) {
    if (s.len == 0) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < s.len; i++) {
        char c = s.data[i];
        if (c < '0' || c > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

bool str_to_cstr(Str s, char *buf, size_t capacity) {
    if (s.len >= capacity || str_find_char(s, '\0') != STR_NPOS) {
        return false;
    }
    if (s.len > 0) {
        memcpy(buf, s.data, s.len);
    }
    buf[s.len] = '\0';
    return true;
}

void str_builder_init(Str_Builder *b, char *buf, size_t capacity) {
    b->data = buf;
    b->len = 0;
    b->capacity = capacity;
    b->overflow = false;
}

bool str_builder_append(Str_Builder *b, Str s) {
    if (s.len > b->capacity - b->len) {
        b->overflow = true;
        return false;
    }
    if (s.len > 0) {
        memcpy(b->data + b->len, s.data, s.len);
    }
    b->len += s.len;
    return true;
}

bool str_builder_append_u64(Str_Builder *b, uint64_t value) {
    char digits[20];
    size_t n = 0;
    do {
        digits[sizeof digits - 1 - n] = (char)('0' + value % 10);
        value /= 10;
        n++;
    } while (value != 0);
    return str_builder_append(b, str_make(digits + sizeof digits - n, n));
}

Str str_builder_str(Str_Builder const *b) {
    return str_make(b->data, b->len);
}
