#include "jsmin.h"

#include <stdint.h>

/**
 * Deepest nesting of braces and template substitutions the lexer follows.
 */
#define MAX_BRACE_DEPTH 256

/**
 * Deepest nesting of parentheses the minifier follows.
 */
#define MAX_PAREN_DEPTH 256

typedef enum {
    TOKEN_END,
    TOKEN_WORD,             /**< Identifier, keyword or #private name. */
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_REGEX,
    TOKEN_TEMPLATE,         /**< A template without substitutions: `...`. */
    TOKEN_TEMPLATE_HEAD,    /**< The start of a template up to its first substitution: `...${ */
    TOKEN_TEMPLATE_MIDDLE,  /**< Between two substitutions: }...${ */
    TOKEN_TEMPLATE_TAIL,    /**< After the last substitution: }...` */
    TOKEN_PUNCT,
} Token_Kind;

typedef struct {
    Token_Kind kind;
    Str        text;
    size_t     offset;          /**< Where the token starts in the source. */
    bool       newline_before;  /**< A line break, or a comment with one, came before it. */
} Token;

typedef enum {
    BRACE_BLOCK,     /**< An ordinary {. */
    BRACE_TEMPLATE,  /**< The ${ of a template substitution: its } continues the template. */
} Brace_Kind;

typedef struct {
    Str        source;
    size_t     pos;
    Token      prev;  /**< The last token returned, to tell a regular expression from a division. */
    uint8_t    braces[MAX_BRACE_DEPTH];
    size_t     brace_depth;
    Minify_Error  *error;
} Lexer;

typedef struct {
    Str_Builder *out;
    Token        prev;               /**< The last token written; TOKEN_END before the first. */
    bool         prev_closes_head;   /**< prev is the ")" ending an if, for, while or with head. */
    bool         paren_is_head[MAX_PAREN_DEPTH];
    size_t       paren_depth;
    Token        pending;            /**< A ";" or "," held back until the next token shows whether it is needed. */
    bool         has_pending;
    Minify_Error    *error;
} Emitter;

/**
 * Punctuators, longest first so the first match is the longest.
 */
static Str const PUNCTUATORS[] = {
    STR_INIT(">>>="),
    STR_INIT("..."),
    STR_INIT("==="),
    STR_INIT("!=="),
    STR_INIT("**="),
    STR_INIT("<<="),
    STR_INIT(">>="),
    STR_INIT(">>>"),
    STR_INIT("&&="),
    STR_INIT("||="),
    STR_INIT("?\?="),
    STR_INIT("=>"),
    STR_INIT("=="),
    STR_INIT("!="),
    STR_INIT("<="),
    STR_INIT(">="),
    STR_INIT("&&"),
    STR_INIT("||"),
    STR_INIT("?\?"),
    STR_INIT("?."),
    STR_INIT("++"),
    STR_INIT("--"),
    STR_INIT("+="),
    STR_INIT("-="),
    STR_INIT("*="),
    STR_INIT("/="),
    STR_INIT("%="),
    STR_INIT("&="),
    STR_INIT("|="),
    STR_INIT("^="),
    STR_INIT("<<"),
    STR_INIT(">>"),
    STR_INIT("**"),
};

/**
 * Keywords after which a "/" starts a regular expression, not a division.
 */
static Str const REGEX_KEYWORDS[] = {
    STR_INIT("return"),
    STR_INIT("typeof"),
    STR_INIT("instanceof"),
    STR_INIT("in"),
    STR_INIT("of"),
    STR_INIT("new"),
    STR_INIT("delete"),
    STR_INIT("void"),
    STR_INIT("throw"),
    STR_INIT("case"),
    STR_INIT("do"),
    STR_INIT("else"),
    STR_INIT("yield"),
    STR_INIT("await"),
};

/**
 * Keywords that start a statement head in parentheses; a ";" right after the
 * ")" is then the statement's empty body and must stay.
 */
static Str const HEAD_KEYWORDS[] = {
    STR_INIT("if"),
    STR_INIT("for"),
    STR_INIT("while"),
    STR_INIT("with"),
};

#define COUNT_OF(array) (sizeof (array) / sizeof (array)[0])

static unsigned char byte_at(Str s, size_t i) {
    return i < s.len ? (unsigned char)s.data[i] : 0;
}

static bool is_one_of(Str s, Str const *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (str_eq(s, list[i])) {
            return true;
        }
    }
    return false;
}

static bool token_is(Token const *t, Str punct) {
    return t->kind == TOKEN_PUNCT && str_eq(t->text, punct);
}

static bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static bool is_ascii_letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/**
 * Whether a byte can be part of an identifier, number or keyword. Bytes of
 * multi-byte UTF-8 characters count, so non-ASCII names stay whole.
 */
static bool is_word_byte(unsigned char c) {
    return is_ascii_letter(c) || is_digit(c) || c == '_' || c == '$' || c == '\\' || c >= 0x80;
}

/**
 * Length of the whitespace or line terminator at pos, or 0 when there is none.
 *
 * @param line_break Set when it is a line terminator.
 */
static size_t space_at(Str s, size_t pos, bool *line_break) {
    unsigned char const c = byte_at(s, pos);
    *line_break = false;
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
        return 1;
    }
    if (c == '\n' || c == '\r') {
        *line_break = true;
        return 1;
    }
    if (c == 0xC2 && byte_at(s, pos + 1) == 0xA0) {
        return 2;  // No-break space.
    }
    if (c == 0xEF && byte_at(s, pos + 1) == 0xBB && byte_at(s, pos + 2) == 0xBF) {
        return 3;  // Byte order mark.
    }
    if (c == 0xE2 && byte_at(s, pos + 1) == 0x80 && (byte_at(s, pos + 2) == 0xA8 || byte_at(s, pos + 2) == 0xA9)) {
        *line_break = true;  // Line and paragraph separators.
        return 3;
    }
    return 0;
}

static bool line_break_at(Str s, size_t pos) {
    bool line_break;
    return space_at(s, pos, &line_break) > 0 && line_break;
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
    error->line = line_of(source, offset);
    error->message = message;
    return false;
}

// Lexer.

static bool regex_allowed(Token const *prev) {
    switch (prev->kind) {
        case TOKEN_END:
        case TOKEN_TEMPLATE_HEAD:
        case TOKEN_TEMPLATE_MIDDLE:
            return true;
        case TOKEN_WORD:
            return is_one_of(prev->text, REGEX_KEYWORDS, COUNT_OF(REGEX_KEYWORDS));
        case TOKEN_PUNCT:
            return !(token_is(prev, STR(")")) || token_is(prev, STR("]")) ||
                     token_is(prev, STR("++")) || token_is(prev, STR("--")));
        default:
            return false;
    }
}

static bool push_brace(Lexer *lx, Brace_Kind kind, size_t offset) {
    if (lx->brace_depth == MAX_BRACE_DEPTH) {
        return fail(lx->error, lx->source, offset, STR("braces nested too deeply"));
    }
    lx->braces[lx->brace_depth++] = (uint8_t)kind;
    return true;
}

/**
 * Skips whitespace and comments.
 *
 * @param newline Set when a line terminator was skipped, also inside a comment.
 */
static bool skip_space(Lexer *lx, bool *newline) {
    Str const s = lx->source;
    *newline = false;
    while (lx->pos < s.len) {
        bool line_break;
        size_t const n = space_at(s, lx->pos, &line_break);
        if (n > 0) {
            *newline = *newline || line_break;
            lx->pos += n;
        } else if (byte_at(s, lx->pos) == '/' && byte_at(s, lx->pos + 1) == '/') {
            lx->pos += 2;
            while (lx->pos < s.len && !line_break_at(s, lx->pos)) {
                lx->pos++;
            }
        } else if (byte_at(s, lx->pos) == '/' && byte_at(s, lx->pos + 1) == '*') {
            size_t const start = lx->pos;
            size_t const end = str_find(str_slice(s, start + 2, s.len), STR("*/"));
            if (end == STR_NPOS) {
                return fail(lx->error, s, start, STR("unterminated comment"));
            }
            for (size_t i = start + 2; i < start + 2 + end; i++) {
                if (line_break_at(s, i)) {
                    *newline = true;
                    break;
                }
            }
            lx->pos = start + 2 + end + 2;
        } else {
            break;
        }
    }
    return true;
}

static bool scan_string(Lexer *lx, Token *token) {
    Str const s = lx->source;
    unsigned char const quote = byte_at(s, lx->pos);
    size_t pos = lx->pos + 1;
    while (pos < s.len) {
        unsigned char const c = byte_at(s, pos);
        if (c == quote) {
            token->kind = TOKEN_STRING;
            lx->pos = pos + 1;
            return true;
        }
        if (c == '\\') {
            // An escaped "\r\n" is one line continuation.
            pos += byte_at(s, pos + 1) == '\r' && byte_at(s, pos + 2) == '\n' ? 3 : 2;
        } else if (c == '\n' || c == '\r') {
            break;
        } else {
            pos++;
        }
    }
    return fail(lx->error, s, token->offset, STR("unterminated string"));
}

/**
 * Scans template text from just after its opening "`" (head) or the "}" of a
 * substitution (not head) to the closing "`" or the next "${".
 */
static bool scan_template(Lexer *lx, Token *token, bool head) {
    Str const s = lx->source;
    size_t pos = lx->pos + 1;
    while (pos < s.len) {
        unsigned char const c = byte_at(s, pos);
        if (c == '\\') {
            pos += 2;
        } else if (c == '`') {
            token->kind = head ? TOKEN_TEMPLATE : TOKEN_TEMPLATE_TAIL;
            lx->pos = pos + 1;
            return true;
        } else if (c == '$' && byte_at(s, pos + 1) == '{') {
            token->kind = head ? TOKEN_TEMPLATE_HEAD : TOKEN_TEMPLATE_MIDDLE;
            lx->pos = pos + 2;
            return push_brace(lx, BRACE_TEMPLATE, pos);
        } else {
            pos++;
        }
    }
    return fail(lx->error, s, token->offset, STR("unterminated template literal"));
}

static bool scan_regex(Lexer *lx, Token *token) {
    Str const s = lx->source;
    size_t pos = lx->pos + 1;
    bool in_class = false;
    while (pos < s.len) {
        unsigned char const c = byte_at(s, pos);
        if (c == '\\') {
            pos += 2;
            continue;
        }
        if (c == '\n' || c == '\r') {
            break;
        }
        if (in_class) {
            in_class = c != ']';
        } else if (c == '[') {
            in_class = true;
        } else if (c == '/') {
            pos++;
            while (pos < s.len && is_ascii_letter(byte_at(s, pos))) {
                pos++;  // Flags.
            }
            token->kind = TOKEN_REGEX;
            lx->pos = pos;
            return true;
        }
        pos++;
    }
    return fail(lx->error, s, token->offset, STR("unterminated regular expression"));
}

static void scan_number(Lexer *lx, Token *token) {
    Str const s = lx->source;
    size_t pos = lx->pos;
    unsigned char const second = byte_at(s, pos + 1);
    if (byte_at(s, pos) == '0' && (second == 'x' || second == 'X' || second == 'b' || second == 'B' ||
                                   second == 'o' || second == 'O')) {
        pos += 2;
        while (pos < s.len && (is_ascii_letter(byte_at(s, pos)) || is_digit(byte_at(s, pos)) ||
                               byte_at(s, pos) == '_')) {
            pos++;
        }
    } else {
        bool dot = false;
        bool exponent = false;
        while (pos < s.len) {
            unsigned char const c = byte_at(s, pos);
            if (is_digit(c) || c == '_') {
                pos++;
            } else if (c == '.' && !dot && !exponent) {
                dot = true;
                pos++;
            } else if ((c == 'e' || c == 'E') && !exponent) {
                exponent = true;
                pos++;
                if (byte_at(s, pos) == '+' || byte_at(s, pos) == '-') {
                    pos++;
                }
            } else if (c == 'n') {
                pos++;  // BigInt.
                break;
            } else {
                break;
            }
        }
    }
    token->kind = TOKEN_NUMBER;
    lx->pos = pos;
}

static void scan_word(Lexer *lx, Token *token) {
    Str const s = lx->source;
    size_t pos = lx->pos + 1;  // The first byte may be "#".
    if (byte_at(s, lx->pos) == '\\') {
        pos = lx->pos;
    }
    while (pos < s.len) {
        bool line_break;
        unsigned char const c = byte_at(s, pos);
        if (!is_word_byte(c) || space_at(s, pos, &line_break) > 0) {
            break;
        }
        if (c == '\\' && byte_at(s, pos + 1) == 'u' && byte_at(s, pos + 2) == '{') {
            // Escape in a name: \u{...}.
            size_t const close = str_find_char(str_slice(s, pos, s.len), '}');
            pos = close == STR_NPOS ? s.len : pos + close + 1;
        } else {
            pos += c == '\\' ? 2 : 1;
        }
    }
    token->kind = TOKEN_WORD;
    lx->pos = pos;
}

static bool scan_punct(Lexer *lx, Token *token) {
    Str const s = lx->source;
    Str const rest = str_slice(s, lx->pos, s.len);
    size_t len = 1;
    for (size_t i = 0; i < COUNT_OF(PUNCTUATORS); i++) {
        if (str_starts_with(rest, PUNCTUATORS[i])) {
            len = PUNCTUATORS[i].len;
            break;
        }
    }
    // "?." followed by a digit is "?" and a number: a ? .5 : 1.
    if (len == 2 && rest.data[0] == '?' && rest.data[1] == '.' && is_digit(byte_at(rest, 2))) {
        len = 1;
    }
    unsigned char const c = byte_at(s, lx->pos);
    if (c == '{') {
        if (!push_brace(lx, BRACE_BLOCK, lx->pos)) {
            return false;
        }
    } else if (c == '}') {
        if (lx->brace_depth == 0) {
            return fail(lx->error, s, lx->pos, STR("unbalanced }"));
        }
        lx->brace_depth--;
    }
    token->kind = TOKEN_PUNCT;
    lx->pos += len;
    return true;
}

/**
 * Reads the next token. At the end of the source the token is TOKEN_END.
 */
static bool lex_next(Lexer *lx, Token *token) {
    Str const s = lx->source;
    bool newline;
    if (!skip_space(lx, &newline)) {
        return false;
    }
    size_t const start = lx->pos;
    token->offset = start;
    token->newline_before = newline;
    bool ok = true;
    if (start >= s.len) {
        if (lx->brace_depth > 0) {
            return fail(lx->error, s, start, STR("unclosed { or template substitution"));
        }
        token->kind = TOKEN_END;
    } else {
        unsigned char const c = byte_at(s, start);
        if (c == '"' || c == '\'') {
            ok = scan_string(lx, token);
        } else if (c == '`') {
            ok = scan_template(lx, token, true);
        } else if (c == '}' && lx->brace_depth > 0 && lx->braces[lx->brace_depth - 1] == BRACE_TEMPLATE) {
            lx->brace_depth--;
            ok = scan_template(lx, token, false);
        } else if (is_digit(c) || (c == '.' && is_digit(byte_at(s, start + 1)))) {
            scan_number(lx, token);
        } else if (is_word_byte(c) || c == '#') {
            scan_word(lx, token);
        } else if (c == '/' && regex_allowed(&lx->prev)) {
            ok = scan_regex(lx, token);
        } else {
            ok = scan_punct(lx, token);
        }
    }
    if (!ok) {
        return false;
    }
    token->text = str_slice(s, start, lx->pos);
    lx->prev = *token;
    return true;
}

static void lexer_init(Lexer *lx, Str source, Minify_Error *error) {
    lx->source = source;
    lx->pos = 0;
    lx->prev = (Token) {
        .kind = TOKEN_END,
    };
    lx->brace_depth = 0;
    lx->error = error;
}

// Minifier.

/**
 * Whether a statement could end with this token, so that a line break after
 * it may be where a semicolon is inserted.
 */
static bool can_end_statement(Token const *t) {
    switch (t->kind) {
        case TOKEN_WORD:
        case TOKEN_NUMBER:
        case TOKEN_STRING:
        case TOKEN_REGEX:
        case TOKEN_TEMPLATE:
        case TOKEN_TEMPLATE_TAIL:
            return true;
        case TOKEN_PUNCT:
            return token_is(t, STR(")")) || token_is(t, STR("]")) || token_is(t, STR("}")) ||
                   token_is(t, STR("++")) || token_is(t, STR("--"));
        default:
            return false;
    }
}

/**
 * Whether a statement could start with this token.
 */
static bool can_start_statement(Token const *t) {
    switch (t->kind) {
        case TOKEN_WORD:
        case TOKEN_NUMBER:
        case TOKEN_STRING:
        case TOKEN_REGEX:
        case TOKEN_TEMPLATE:
        case TOKEN_TEMPLATE_HEAD:
            return true;
        case TOKEN_PUNCT:
            return token_is(t, STR("(")) || token_is(t, STR("[")) || token_is(t, STR("{")) ||
                   token_is(t, STR("+")) || token_is(t, STR("-")) || token_is(t, STR("++")) ||
                   token_is(t, STR("--")) || token_is(t, STR("!")) || token_is(t, STR("~")) ||
                   token_is(t, STR("...")) || token_is(t, STR("<"));
        default:
            return false;
    }
}

/**
 * Whether writing b right after a would read differently, so a space must
 * separate them.
 */
static bool needs_space(Token const *a, Token const *b) {
    unsigned char const last = (unsigned char)a->text.data[a->text.len - 1];
    unsigned char const first = (unsigned char)b->text.data[0];
    if (is_word_byte(last) && (is_word_byte(first) || first == '#')) {
        return true;  // Two names, a keyword and a name, or a regex's flags and a name.
    }
    if (a->kind == TOKEN_NUMBER && first == '.') {
        return true;  // 1 .toString() is not 1. toString().
    }
    if ((last == '+' && first == '+') || (last == '-' && first == '-')) {
        return true;  // a + ++b is not a++ + b.
    }
    if (last == '/' && (first == '/' || first == '*')) {
        return true;  // Would start a comment.
    }
    if ((last == '<' && first == '!') || (last == '-' && first == '>')) {
        return true;  // Would read as an HTML comment, "<!--" or "-->".
    }
    return false;
}

static bool write_token(Emitter *e, Token const *t) {
    bool ok = true;
    if (e->prev.kind != TOKEN_END) {
        if (t->newline_before && can_end_statement(&e->prev) && can_start_statement(t)) {
            ok = str_builder_append_char(e->out, '\n');
        } else if (needs_space(&e->prev, t)) {
            ok = str_builder_append_char(e->out, ' ');
        }
    }
    ok = ok && str_builder_append(e->out, t->text);
    if (!ok) {
        e->error->line = 0;
        e->error->message = STR("output buffer full");
        return false;
    }

    bool closes_head = false;
    if (token_is(t, STR("("))) {
        if (e->paren_depth < MAX_PAREN_DEPTH) {
            e->paren_is_head[e->paren_depth] = e->prev.kind == TOKEN_WORD &&
                                               is_one_of(e->prev.text, HEAD_KEYWORDS, COUNT_OF(HEAD_KEYWORDS));
        }
        e->paren_depth++;
    } else if (token_is(t, STR(")")) && e->paren_depth > 0) {
        e->paren_depth--;
        closes_head = e->paren_depth < MAX_PAREN_DEPTH && e->paren_is_head[e->paren_depth];
    }
    e->prev = *t;
    e->prev_closes_head = closes_head;
    return true;
}

/**
 * Whether the held-back ";" or "," can be left out before t.
 */
static bool pending_is_optional(Emitter const *e, Token const *t) {
    Token const *p = &e->pending;
    if (token_is(p, STR(";"))) {
        /*
         * A ";" before "}" is supplied by automatic semicolon insertion, unless
         * it is an empty statement that something needs: "if (a);", "else;",
         * "do;" or "label:;".
         */
        return token_is(t, STR("}")) && !e->prev_closes_head && !token_is(&e->prev, STR(":")) &&
               !(e->prev.kind == TOKEN_WORD && (str_eq(e->prev.text, STR("else")) || str_eq(e->prev.text, STR("do"))));
    }
    if (token_is(t, STR("}")) || token_is(t, STR(")"))) {
        return true;  // A trailing comma in an object, a call or parameters.
    }
    // In an array, a trailing comma is optional, but not after a hole: [1,,] has two elements.
    return token_is(t, STR("]")) && !token_is(&e->prev, STR(",")) && !token_is(&e->prev, STR("["));
}

static bool emit(Emitter *e, Token const *t) {
    if (e->has_pending) {
        e->has_pending = false;
        if (!pending_is_optional(e, t) && !write_token(e, &e->pending)) {
            return false;
        }
    }
    if (token_is(t, STR(";")) || token_is(t, STR(","))) {
        e->pending = *t;
        e->has_pending = true;
        return true;
    }
    return t->kind == TOKEN_END || write_token(e, t);
}

bool js_minify(Str source, Str_Builder *out, Minify_Error *error) {
    Lexer lx;
    lexer_init(&lx, source, error);
    Emitter e = {
        .out = out,
        .prev = {
            .kind = TOKEN_END,
        },
        .prev_closes_head = false,
        .paren_depth = 0,
        .has_pending = false,
        .error = error,
    };
    Token token;
    do {
        if (!lex_next(&lx, &token) || !emit(&e, &token)) {
            return false;
        }
    } while (token.kind != TOKEN_END);
    return true;
}

// Top-level declarations.

static bool add_name(Js_Name *names, size_t capacity, size_t *count, Str source, Token const *t, Minify_Error *error) {
    if (*count == capacity) {
        return fail(error, source, t->offset, STR("too many top-level names"));
    }
    names[*count] = (Js_Name) {
        .name = t->text,
        .line = line_of(source, t->offset),
    };
    (*count)++;
    return true;
}

bool js_top_level_names(Str source, Js_Name *names, size_t capacity, size_t *count, Minify_Error *error) {
    Lexer lx;
    lexer_init(&lx, source, error);
    *count = 0;
    size_t depth = 0;
    bool statement_start = true;
    bool in_declaration = false;  // Inside a top-level const, let or var, until its ";".
    bool expect_name = false;
    Token t;
    for (;;) {
        if (!lex_next(&lx, &t)) {
            return false;
        }
        if (t.kind == TOKEN_END) {
            return true;
        }
        bool next_statement_start = false;
        if (depth == 0) {
            if (expect_name && token_is(&t, STR("*"))) {
                continue;  // function* name.
            }
            if (expect_name && t.kind == TOKEN_WORD) {
                if (!add_name(names, capacity, count, source, &t, error)) {
                    return false;
                }
            }
            expect_name = false;
            if (statement_start && t.kind == TOKEN_WORD) {
                if (str_eq(t.text, STR("const")) || str_eq(t.text, STR("let")) || str_eq(t.text, STR("var"))) {
                    in_declaration = true;
                    expect_name = true;
                } else if (str_eq(t.text, STR("function")) || str_eq(t.text, STR("class"))) {
                    expect_name = true;
                } else if (str_eq(t.text, STR("async"))) {
                    next_statement_start = true;  // async function name.
                }
            } else if (in_declaration && token_is(&t, STR(","))) {
                expect_name = true;
            } else if (token_is(&t, STR(";"))) {
                in_declaration = false;
            }
        }

        if (token_is(&t, STR("(")) || token_is(&t, STR("[")) || token_is(&t, STR("{")) ||
            t.kind == TOKEN_TEMPLATE_HEAD) {
            depth++;
        } else if ((token_is(&t, STR(")")) || token_is(&t, STR("]")) || token_is(&t, STR("}")) ||
                    t.kind == TOKEN_TEMPLATE_TAIL) && depth > 0) {
            depth--;
        }
        statement_start = next_statement_start ||
                          (depth == 0 && (token_is(&t, STR(";")) || token_is(&t, STR("}"))));
    }
}
