#include <assert.h>
#include <stdio.h>

#include "template.h"

/** One variable of a page. The strings point into the templates and are not copied. */
typedef struct {
    Str name;
    Str value;
} Variable;

/** One template being processed: the page itself or a template it includes. */
typedef struct {
    Str    name;     /**< For errors. */
    Str    text;
    size_t pos;      /**< Where processing goes on. */
    size_t if_base;  /**< How many {{if}}s were open when the template started. */
} Frame;

/** An open {{if}}. */
typedef struct {
    bool parent_emitting;  /**< The text around the {{if}} is part of the page. */
    bool condition;        /**< What the {{if}} asked. */
    bool in_else;          /**< After the {{else}}. */
} Branch;

/** A word or a quoted string of a tag. */
typedef struct {
    Str  text;    /**< Without the quotes. */
    bool quoted;
} Word;

typedef struct {
    Variable        variables[TEMPLATE_MAX_VARIABLES];
    size_t          variable_count;
    Frame           frames[TEMPLATE_MAX_DEPTH];
    size_t          frame_count;
    Branch          branches[TEMPLATE_MAX_IFS];
    size_t          branch_count;
    Template_Loader load;
    void           *context;
    Str_Builder    *out;
    Template_Error *error;
} Renderer;

/** The words that start a statement. A variable cannot be named like one. */
static Str const KEYWORDS[] = {
    STR_INIT("set"),
    STR_INIT("default"),
    STR_INIT("include"),
    STR_INIT("if"),
    STR_INIT("else"),
    STR_INIT("end"),
};

static bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static bool is_keyword(Str word) {
    for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
        if (str_eq(word, KEYWORDS[i])) {
            return true;
        }
    }
    return false;
}

/**
 * Whether a word is a possible name for a variable: a-z, 0-9 and _, and not a keyword.
 */
static bool is_variable_name(Str word) {
    if (word.len == 0 || is_keyword(word)) {
        return false;
    }
    for (size_t i = 0; i < word.len; i++) {
        if (!is_name_char(word.data[i])) {
            return false;
        }
    }
    return true;
}

/**
 * Whether a name is safe to look up in the directory of includes: no absolute path,
 * no backslash and no "." or ".." segment that could lead elsewhere.
 */
static bool is_include_name(Str name) {
    if (name.len == 0 || name.data[0] == '/' || str_contains(name, STR("\\"))) {
        return false;
    }
    Str segment;
    Str rest = name;
    while (str_split_once(rest, STR("/"), &segment, &rest)) {
        if (segment.len == 0 || str_eq(segment, STR(".")) || str_eq(segment, STR(".."))) {
            return false;
        }
    }
    return rest.len > 0 && !str_eq(rest, STR(".")) && !str_eq(rest, STR(".."));
}

/**
 * The line of an offset in a template, counting from 1.
 */
static size_t line_of(Str text, size_t offset) {
    size_t line = 1;
    for (size_t i = 0; i < offset && i < text.len; i++) {
        if (text.data[i] == '\n') {
            line++;
        }
    }
    return line;
}

/**
 * Records an error. Always returns false, so that a caller can return the call.
 */
static bool fail(Renderer *r, Frame const *frame, size_t offset, Str message, Str detail) {
    assert(r != NULL);
    assert(frame != NULL);
    *r->error = (Template_Error) {
        .file = frame->name,
        .line = line_of(frame->text, offset),
        .message = message,
        .detail = detail,
    };
    return false;
}

static bool is_emitting(Renderer const *r) {
    if (r->branch_count == 0) {
        return true;
    }
    Branch const *branch = &r->branches[r->branch_count - 1];
    return branch->parent_emitting && (branch->in_else ? !branch->condition : branch->condition);
}

static Variable *find_variable(Renderer *r, Str name) {
    for (size_t i = 0; i < r->variable_count; i++) {
        if (str_eq(r->variables[i].name, name)) {
            return &r->variables[i];
        }
    }
    return NULL;
}

/**
 * Splits a tag into words and quoted strings.
 *
 * @param words Receives at most TEMPLATE_MAX_WORDS words.
 * @param count Receives how many.
 * @param problem Receives what is wrong when this returns false.
 */
static bool split_words(Str tag, Word *words, size_t *count, Str *problem) {
    assert(words != NULL);
    assert(count != NULL);
    assert(problem != NULL);
    *count = 0;
    size_t i = 0;
    while (i < tag.len) {
        if (is_blank(tag.data[i])) {
            i++;
            continue;
        }
        if (*count == TEMPLATE_MAX_WORDS) {
            *problem = STR("too many words in the tag");
            return false;
        }
        if (tag.data[i] == '"') {
            size_t const start = i + 1;
            size_t const length = str_find_char(str_slice(tag, start, tag.len), '"');
            if (length == STR_NPOS) {
                *problem = STR("a quote is not closed");
                return false;
            }
            words[*count] = (Word) {
                .text = str_slice(tag, start, start + length),
                .quoted = true,
            };
            i = start + length + 1;
            if (i < tag.len && !is_blank(tag.data[i])) {
                *problem = STR("a quoted string must be followed by a space");
                return false;
            }
        } else {
            size_t const start = i;
            while (i < tag.len && !is_blank(tag.data[i]) && tag.data[i] != '"') {
                i++;
            }
            if (i < tag.len && tag.data[i] == '"') {
                *problem = STR("a quoted string must be preceded by a space");
                return false;
            }
            words[*count] = (Word) {
                .text = str_slice(tag, start, i),
                .quoted = false,
            };
        }
        (*count)++;
    }
    return true;
}

/**
 * Handles {{set name "value"}} and {{default name "value"}}.
 */
static bool run_set(Renderer *r, Frame const *frame, size_t offset, Word const *words,
                    size_t count, bool only_if_unset) {
    if (count != 3 || words[1].quoted || !words[2].quoted) {
        return fail(r, frame, offset,
                    only_if_unset ? STR("expected {{default name \"value\"}}")
                                  : STR("expected {{set name \"value\"}}"),
                    STR(""));
    }
    Str const name = words[1].text;
    if (!is_variable_name(name)) {
        return fail(r, frame, offset,
                    STR("a variable name uses a-z, 0-9 and _ and is not a keyword: "), name);
    }
    if (!is_emitting(r)) {
        return true;
    }
    Variable *variable = find_variable(r, name);
    if (variable == NULL) {
        if (r->variable_count == TEMPLATE_MAX_VARIABLES) {
            return fail(r, frame, offset, STR("too many variables"), STR(""));
        }
        variable = &r->variables[r->variable_count++];
        variable->name = name;
        variable->value = words[2].text;
    } else if (!only_if_unset || variable->value.len == 0) {
        variable->value = words[2].text;
    }
    return true;
}

/**
 * Handles {{include "name"}}: processing goes on in the included template and returns
 * to the tag's end when it is done.
 */
static bool run_include(Renderer *r, Frame const *frame, size_t offset, Word const *words,
                        size_t count) {
    if (count != 2 || !words[1].quoted) {
        return fail(r, frame, offset, STR("expected {{include \"file.html\"}}"), STR(""));
    }
    Str const name = words[1].text;
    if (!is_include_name(name)) {
        return fail(r, frame, offset, STR("not a template name to include: "), name);
    }
    if (!is_emitting(r)) {
        return true;
    }
    if (r->frame_count == TEMPLATE_MAX_DEPTH) {
        return fail(r, frame, offset,
                    STR("includes nested too deeply (is one including itself?): "), name);
    }
    Str contents;
    if (!r->load(r->context, name, &contents)) {
        return fail(r, frame, offset, STR("cannot include "), name);
    }
    r->frames[r->frame_count++] = (Frame) {
        .name = name,
        .text = contents,
        .pos = 0,
        .if_base = r->branch_count,
    };
    return true;
}

/**
 * Handles {{if name}}, {{if name == "value"}} and {{if name != "value"}}.
 */
static bool run_if(Renderer *r, Frame const *frame, size_t offset, Word const *words,
                   size_t count) {
    bool const plain = count == 2 && !words[1].quoted;
    bool const compare = count == 4 && !words[1].quoted && !words[2].quoted && words[3].quoted
                         && (str_eq(words[2].text, STR("==")) || str_eq(words[2].text, STR("!=")));
    if (!plain && !compare) {
        return fail(r, frame, offset,
                    STR("expected {{if name}} or {{if name == \"value\"}} (or !=)"), STR(""));
    }
    Str const name = words[1].text;
    if (!is_variable_name(name)) {
        return fail(r, frame, offset, STR("not a variable name: "), name);
    }
    if (r->branch_count == TEMPLATE_MAX_IFS) {
        return fail(r, frame, offset, STR("too many {{if}}s inside each other"), STR(""));
    }
    Variable const *variable = find_variable(r, name);
    Str const value = variable == NULL ? STR("") : variable->value;
    bool condition = value.len > 0;
    if (compare) {
        condition = str_eq(value, words[3].text) == str_eq(words[2].text, STR("=="));
    }
    bool const parent_emitting = is_emitting(r);
    r->branches[r->branch_count++] = (Branch) {
        .parent_emitting = parent_emitting,
        .condition = condition,
        .in_else = false,
    };
    return true;
}

static bool run_else(Renderer *r, Frame const *frame, size_t offset, size_t count) {
    if (count != 1) {
        return fail(r, frame, offset, STR("expected {{else}}"), STR(""));
    }
    if (r->branch_count == frame->if_base) {
        return fail(r, frame, offset, STR("{{else}} without {{if}}"), STR(""));
    }
    Branch *branch = &r->branches[r->branch_count - 1];
    if (branch->in_else) {
        return fail(r, frame, offset, STR("a second {{else}} in one {{if}}"), STR(""));
    }
    branch->in_else = true;
    return true;
}

static bool run_end(Renderer *r, Frame const *frame, size_t offset, size_t count) {
    if (count != 1) {
        return fail(r, frame, offset, STR("expected {{end}}"), STR(""));
    }
    if (r->branch_count == frame->if_base) {
        return fail(r, frame, offset, STR("{{end}} without {{if}}"), STR(""));
    }
    r->branch_count--;
    return true;
}

/**
 * Handles {{name}}: appends the variable's value.
 */
static bool run_variable(Renderer *r, Frame const *frame, size_t offset, Str tag) {
    if (!is_variable_name(tag)) {
        return fail(r, frame, offset, STR("unknown tag: "), tag);
    }
    if (!is_emitting(r)) {
        return true;
    }
    Variable const *variable = find_variable(r, tag);
    if (variable == NULL) {
        return fail(r, frame, offset, STR("variable has no value: "), tag);
    }
    str_builder_append(r->out, variable->value);
    return true;
}

/**
 * Runs one tag, whatever it is.
 *
 * @param offset Where the tag starts in the template, for errors.
 * @param tag The text between the braces, trimmed.
 */
static bool run_tag(Renderer *r, Frame const *frame, size_t offset, Str tag) {
    Word words[TEMPLATE_MAX_WORDS];
    size_t count;
    Str problem;
    if (!split_words(tag, words, &count, &problem)) {
        return fail(r, frame, offset, problem, STR(""));
    }
    if (count == 0) {
        return fail(r, frame, offset, STR("empty tag"), STR(""));
    }
    Str const first = words[0].text;
    if (words[0].quoted) {
        return fail(r, frame, offset, STR("a tag cannot start with a quote"), STR(""));
    }
    if (str_eq(first, STR("set"))) {
        return run_set(r, frame, offset, words, count, false);
    }
    if (str_eq(first, STR("default"))) {
        return run_set(r, frame, offset, words, count, true);
    }
    if (str_eq(first, STR("include"))) {
        return run_include(r, frame, offset, words, count);
    }
    if (str_eq(first, STR("if"))) {
        return run_if(r, frame, offset, words, count);
    }
    if (str_eq(first, STR("else"))) {
        return run_else(r, frame, offset, count);
    }
    if (str_eq(first, STR("end"))) {
        return run_end(r, frame, offset, count);
    }
    return run_variable(r, frame, offset, tag);
}

/**
 * Whether a tag is a statement, which takes its line along when it is alone on it,
 * rather than a variable, which is part of the line.
 */
static bool is_statement(Str tag) {
    size_t const space = str_find_any(tag, STR(" \t\r\n"));
    Str const first = space == STR_NPOS ? tag : str_slice(tag, 0, space);
    return is_keyword(first);
}

static bool only_blanks(Str s) {
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] != ' ' && s.data[i] != '\t' && s.data[i] != '\r') {
            return false;
        }
    }
    return true;
}

/**
 * Finds the whole line of a tag when nothing but blanks is around the tag on it.
 *
 * @param open Where the tag starts.
 * @param close Where the tag ends, just after its braces.
 * @param line_start Receives where the line starts.
 * @param line_end Receives where the next line starts, or the end of the text.
 * @return False when the tag shares its line with other text.
 */
static bool find_own_line(Str text, size_t open, size_t close, size_t *line_start,
                          size_t *line_end) {
    assert(line_start != NULL);
    assert(line_end != NULL);
    size_t const previous = str_rfind_char(str_slice(text, 0, open), '\n');
    size_t const start = previous == STR_NPOS ? 0 : previous + 1;
    Str const after = str_slice(text, close, text.len);
    size_t const newline = str_find_char(after, '\n');
    Str const tail = newline == STR_NPOS ? after : str_slice(after, 0, newline);
    if (!only_blanks(str_slice(text, start, open)) || !only_blanks(tail)) {
        return false;
    }
    *line_start = start;
    *line_end = newline == STR_NPOS ? text.len : close + newline + 1;
    return true;
}

static void emit(Renderer *r, Str text) {
    if (is_emitting(r)) {
        str_builder_append(r->out, text);
    }
}

void template_print_error(Template_Error const *error) {
    assert(error != NULL);
    if (error->line > 0) {
        fprintf(stderr, "%.*s:%zu: %.*s%.*s\n", STR_ARG(error->file), error->line,
                STR_ARG(error->message), STR_ARG(error->detail));
    } else {
        fprintf(stderr, "%.*s: %.*s%.*s\n", STR_ARG(error->file), STR_ARG(error->message),
                STR_ARG(error->detail));
    }
}

bool template_render(Str name, Str source, Template_Loader load, void *context,
                     Str_Builder *out, Template_Error *error) {
    assert(load != NULL);
    assert(out != NULL);
    assert(error != NULL);
    Renderer r = {
        .variable_count = 0,
        .frame_count = 1,
        .branch_count = 0,
        .load = load,
        .context = context,
        .out = out,
        .error = error,
    };
    r.frames[0] = (Frame) {
        .name = name,
        .text = source,
        .pos = 0,
        .if_base = 0,
    };

    // Every step moves a template forward by at least one byte or ends one, and a
    // template can only start (an include) when there is room for it on the stack of at
    // most TEMPLATE_MAX_DEPTH, so the loop ends.
    while (r.frame_count > 0) {
        Frame *frame = &r.frames[r.frame_count - 1];
        Str const text = frame->text;
        if (frame->pos >= text.len) {
            if (r.branch_count != frame->if_base) {
                return fail(&r, frame, text.len, STR("{{if}} without {{end}}"), STR(""));
            }
            r.frame_count--;
            continue;
        }

        Str const rest = str_slice(text, frame->pos, text.len);
        size_t const found = str_find(rest, STR("{{"));
        if (found == STR_NPOS) {
            emit(&r, rest);
            frame->pos = text.len;
            continue;
        }
        size_t const open = frame->pos + found;
        size_t const length = str_find(str_slice(text, open + 2, text.len), STR("}}"));
        if (length == STR_NPOS) {
            return fail(&r, frame, open, STR("{{ is not closed by }}"), STR(""));
        }
        size_t const close = open + 2 + length + 2;
        Str const tag = str_trim(str_slice(text, open + 2, open + 2 + length));

        size_t text_end = open;
        size_t next = close;
        size_t line_start;
        size_t line_end;
        if (is_statement(tag) && find_own_line(text, open, close, &line_start, &line_end)) {
            assert(line_start >= frame->pos);
            text_end = line_start;
            next = line_end;
        }
        emit(&r, str_slice(text, frame->pos, text_end));
        frame->pos = next;
        if (!run_tag(&r, frame, open, tag)) {
            return false;
        }
    }

    if (out->overflow) {
        *error = (Template_Error) {
            .file = name,
            .line = 0,
            .message = STR("the page is larger than the output buffer"),
            .detail = STR(""),
        };
        return false;
    }
    return true;
}
