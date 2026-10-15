#include "cssmin.h"

#include <assert.h>

/**
 * The pieces a style sheet is read in. The minifier works on these rather than
 * on full CSS tokens: only whitespace and comments need to be told apart from
 * the rest, and strings, escapes and url() values must be kept whole.
 */
typedef enum {
    PIECE_END,
    PIECE_SPACE,    /**< A run of whitespace. */
    PIECE_COMMENT,  /**< A comment. */
    PIECE_STRING,   /**< A quoted string, with its quotes. */
    PIECE_URL,      /**< url( with an unquoted value ), whole. */
    PIECE_ESCAPE,   /**< A backslash escape, including the space that may end a hex escape. */
    PIECE_CHAR,     /**< Any other single byte. */
} Piece_Kind;

typedef struct {
    Piece_Kind kind;
    Str        text;
    size_t     offset;  /**< Where the piece starts in the source. */
} Piece;

typedef struct {
    Str           source;
    size_t        pos;
    Minify_Error *error;
} Scanner;

static unsigned char byte_at(Str s, size_t i) {
    return i < s.len ? (unsigned char)s.data[i] : 0;
}

static bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex(unsigned char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/**
 * Whether a byte can be part of a name or number, so that two of them side by
 * side would read as one.
 */
static bool is_name_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) ||
           c == '-' || c == '_' || c == '\\' || c >= 0x80;
}

static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool starts_with_nocase(Str s, size_t pos, Str prefix) {
    if (pos > s.len || s.len - pos < prefix.len) {
        return false;
    }
    for (size_t i = 0; i < prefix.len; i++) {
        if (lower(byte_at(s, pos + i)) != (unsigned char)prefix.data[i]) {
            return false;
        }
    }
    return true;
}

static Str trim_space(Str s) {
    size_t start = 0;
    size_t end = s.len;
    while (start < end && is_space(byte_at(s, start))) {
        start++;
    }
    while (end > start && is_space(byte_at(s, end - 1))) {
        end--;
    }
    return str_slice(s, start, end);
}

static size_t line_of(Str s, size_t offset) {
    size_t line = 1;
    for (size_t i = 0; i < offset && i < s.len; i++) {
        if (s.data[i] == '\n') {
            line++;
        }
    }
    return line;
}

static bool fail(Minify_Error *error, Str source, size_t offset, Str message) {
    assert(error != NULL);
    error->line = line_of(source, offset);
    error->message = message;
    return false;
}

static size_t scan_string(Scanner *sc, size_t start, bool *ok) {
    assert(ok != NULL);
    Str const s = sc->source;
    unsigned char const quote = byte_at(s, start);
    size_t pos = start + 1;
    while (pos < s.len) {
        unsigned char const c = byte_at(s, pos);
        if (c == quote) {
            return pos + 1;
        }
        if (c == '\\') {
            // An escaped "\r\n" is one line continuation.
            pos += byte_at(s, pos + 1) == '\r' && byte_at(s, pos + 2) == '\n' ? 3 : 2;
        } else if (c == '\n' || c == '\r' || c == '\f') {
            break;
        } else {
            pos++;
        }
    }
    *ok = fail(sc->error, s, start, STR("unterminated string"));
    return pos;
}

static size_t scan_escape(Scanner *sc, size_t start) {
    Str const s = sc->source;
    size_t pos = start + 1;
    if (pos >= s.len) {
        return pos;
    }
    if (!is_hex(byte_at(s, pos))) {
        return pos + 1;
    }
    // Up to six hex digits, and one whitespace that ends the escape and belongs to it.
    size_t const digits_end = pos + 6;
    while (pos < s.len && pos < digits_end && is_hex(byte_at(s, pos))) {
        pos++;
    }
    if (byte_at(s, pos) == '\r' && byte_at(s, pos + 1) == '\n') {
        return pos + 2;
    }
    return is_space(byte_at(s, pos)) ? pos + 1 : pos;
}

/**
 * Scans url( with an unquoted value. Returns 0 when the value is quoted: then
 * "url(" is read as ordinary bytes and the string as a string.
 */
static size_t scan_url(Scanner *sc, size_t start, bool *ok) {
    assert(ok != NULL);
    Str const s = sc->source;
    size_t pos = start + 4;
    while (pos < s.len && is_space(byte_at(s, pos))) {
        pos++;
    }
    if (byte_at(s, pos) == '"' || byte_at(s, pos) == '\'') {
        return 0;
    }
    while (pos < s.len && byte_at(s, pos) != ')') {
        pos += byte_at(s, pos) == '\\' ? 2 : 1;
    }
    if (pos >= s.len) {
        *ok = fail(sc->error, s, start, STR("unterminated url()"));
        return s.len;
    }
    return pos + 1;
}

static bool next_piece(Scanner *sc, Piece *p) {
    assert(p != NULL);
    Str const s = sc->source;
    size_t const start = sc->pos;
    p->offset = start;
    if (start >= s.len) {
        p->kind = PIECE_END;
        p->text = STR("");
        return true;
    }
    unsigned char const c = byte_at(s, start);
    size_t end = start + 1;
    bool ok = true;
    if (is_space(c)) {
        while (end < s.len && is_space(byte_at(s, end))) {
            end++;
        }
        p->kind = PIECE_SPACE;
    } else if (c == '/' && byte_at(s, start + 1) == '*') {
        size_t const close = str_find(str_slice(s, start + 2, s.len), STR("*/"));
        if (close == STR_NPOS) {
            return fail(sc->error, s, start, STR("unterminated comment"));
        }
        end = start + 2 + close + 2;
        p->kind = PIECE_COMMENT;
    } else if (c == '"' || c == '\'') {
        end = scan_string(sc, start, &ok);
        p->kind = PIECE_STRING;
    } else if (c == '\\') {
        end = scan_escape(sc, start);
        p->kind = PIECE_ESCAPE;
    } else if (starts_with_nocase(s, start, STR("url(")) &&
               (start == 0 || !is_name_byte(byte_at(s, start - 1)))) {
        size_t const url_end = scan_url(sc, start, &ok);
        if (url_end != 0) {
            end = url_end;
            p->kind = PIECE_URL;
        } else {
            p->kind = PIECE_CHAR;
        }
    } else {
        p->kind = PIECE_CHAR;
    }
    if (!ok) {
        return false;
    }
    p->text = str_slice(s, start, end);
    sc->pos = end;
    return true;
}

/**
 * Whether the 0 at offset can be left out: it starts a number like 0.5, and
 * is not part of a longer name or number such as 10.5 or x0.5.
 */
static bool is_leading_zero(Str s, size_t offset) {
    bool const decimal = byte_at(s, offset) == '0' && byte_at(s, offset + 1) == '.' &&
                         is_digit(byte_at(s, offset + 2));
    if (!decimal) {
        return false;
    }
    if (offset == 0) {
        return true;
    }
    unsigned char const before = byte_at(s, offset - 1);
    bool const signed_number = (before == '-' || before == '+') &&
                               (offset == 1 || !is_name_byte(byte_at(s, offset - 2)));
    if (signed_number) {
        return true;  // A sign: -0.5 is -.5.
    }
    return !is_name_byte(before) && before != '.' && before != '#';
}

/**
 * Whether a space must be written between the bytes last and next.
 *
 * @param whitespace Whether whitespace separated them in the source; if not,
 *                   only a comment did.
 */
static bool needs_space(unsigned char last, unsigned char next, bool whitespace) {
    if (!whitespace) {
        return is_name_byte(last) && is_name_byte(next);  // A comment kept "1px/**/2px" apart.
    }
    if (str_find_char(STR("{};,>~(:!"), (char)last) != STR_NPOS) {
        return false;
    }
    if (str_find_char(STR("{};,>~)!"), (char)next) != STR_NPOS) {
        return false;
    }
    return true;
}

/**
 * Whether the ":" just written ends the name of a custom property: "--name:"
 * right after "{" or ";".
 */
static bool ends_custom_property_name(Str_Builder const *out) {
    size_t start = out->len - 1;  // At the ":".
    while (start > 0 && is_name_byte((unsigned char)out->data[start - 1])) {
        start--;
    }
    if (out->len - 1 - start < 2 || out->data[start] != '-' || out->data[start + 1] != '-') {
        return false;
    }
    return start == 0 || out->data[start - 1] == '{' || out->data[start - 1] == ';';
}

/**
 * Copies the value of a custom property as it is, only without the whitespace
 * around it: browsers keep these values as written, so even a space after a
 * comma is part of the value scripts see. The value ends at the ";" or "}"
 * that is not inside brackets, which is left for the caller to read.
 */
static bool copy_custom_property_value(Scanner *sc, Str_Builder *out, unsigned char *last) {
    assert(out != NULL);
    assert(last != NULL);
    size_t const start = sc->pos;
    size_t nesting = 0;
    Piece p;
    for (;;) {
        size_t const before = sc->pos;
        if (!next_piece(sc, &p)) {
            return false;
        }
        if (p.kind == PIECE_END) {
            break;
        }
        if (p.kind != PIECE_CHAR) {
            continue;
        }
        unsigned char const c = (unsigned char)p.text.data[0];
        if (c == '(' || c == '[' || c == '{') {
            nesting++;
        } else if ((c == ')' || c == ']' || c == '}') && nesting > 0) {
            nesting--;
        } else if (nesting == 0 && (c == ';' || c == '}')) {
            sc->pos = before;
            break;
        }
    }
    Str const value = trim_space(str_slice(sc->source, start, sc->pos));
    str_builder_append(out, value);
    if (value.len > 0) {
        *last = (unsigned char)value.data[value.len - 1];
    }
    return true;
}

bool css_minify(Str source, Str_Builder *out, Minify_Error *error) {
    assert(out != NULL);
    assert(error != NULL);
    Scanner sc = {
        .source = source,
        .pos = 0,
        .error = error,
    };
    bool whitespace = false;  // Whitespace since the last byte written.
    bool comment = false;     // A comment since the last byte written.
    unsigned char last = 0;   // The last byte written by this call; 0 before the first.
    size_t depth = 0;
    Piece p;
    for (;;) {
        if (!next_piece(&sc, &p)) {
            return false;
        }
        if (p.kind == PIECE_END) {
            break;
        }
        if (p.kind == PIECE_SPACE) {
            whitespace = true;
            continue;
        }
        if (p.kind == PIECE_COMMENT) {
            comment = true;
            continue;
        }
        if (p.kind == PIECE_CHAR && is_leading_zero(source, p.offset)) {
            continue;  // 0.5 is written .5; the spacing is decided at the ".".
        }

        unsigned char const first = (unsigned char)p.text.data[0];
        if ((whitespace || comment) && last != 0 && needs_space(last, first, whitespace)) {
            str_builder_append_char(out, ' ');
        }
        whitespace = false;
        comment = false;

        if (p.kind == PIECE_CHAR && first == '{') {
            depth++;
        } else if (p.kind == PIECE_CHAR && first == '}') {
            if (depth == 0) {
                return fail(error, source, p.offset, STR("unbalanced }"));
            }
            depth--;
            if (last == ';' && !out->overflow) {
                out->len--;  // The last declaration needs no ";".
            }
        }

        if (p.kind == PIECE_URL) {
            str_builder_append(out, STR("url("));
            str_builder_append(out, trim_space(str_slice(p.text, 4, p.text.len - 1)));
            str_builder_append_char(out, ')');
        } else {
            str_builder_append(out, p.text);
        }
        last = (unsigned char)p.text.data[p.text.len - 1];

        bool const property_colon = p.kind == PIECE_CHAR && first == ':' && !out->overflow;
        if (property_colon && ends_custom_property_name(out)) {
            if (!copy_custom_property_value(&sc, out, &last)) {
                return false;
            }
        }
    }
    if (depth > 0) {
        return fail(error, source, source.len, STR("unclosed {"));
    }
    if (out->overflow) {
        error->line = 0;
        error->message = STR("output buffer full");
        return false;
    }
    return true;
}

bool css_check(Str source, Minify_Error *error) {
    assert(error != NULL);
    Scanner sc = {
        .source = source,
        .pos = 0,
        .error = error,
    };
    size_t depth = 0;
    Piece p;
    for (;;) {
        if (!next_piece(&sc, &p)) {
            return false;
        }
        if (p.kind == PIECE_END) {
            break;
        }
        if (p.kind != PIECE_CHAR) {
            continue;
        }
        unsigned char const c = (unsigned char)p.text.data[0];
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (depth == 0) {
                return fail(error, source, p.offset, STR("unbalanced }"));
            }
            depth--;
        } else if (c == '@') {
            size_t const name = p.offset + 1;
            size_t end = name;
            while (end < source.len && is_name_byte(byte_at(source, end))) {
                end++;
            }
            Str const word = str_slice(source, name, end);
            if (str_eq_nocase(word, STR("import"))) {
                return fail(error, source, p.offset,
                            STR("@import cannot be bundled; list the file in the manifest "
                                "instead"));
            }
            if (str_eq_nocase(word, STR("charset"))) {
                return fail(error, source, p.offset,
                            STR("@charset is only valid at the start of a style sheet; "
                                "bundles are UTF-8 without it"));
            }
        }
    }
    if (depth > 0) {
        return fail(error, source, source.len, STR("unclosed {"));
    }
    return true;
}
