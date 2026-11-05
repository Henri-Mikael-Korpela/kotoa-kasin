/**
 * @file template_test.c
 * @brief Tests for the template engine, with the included templates kept in memory.
 *
 * Run with ctest, or directly: template-test. Exits with EXIT_FAILURE when a test fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/template.h"

static int FAILURES = 0;

typedef struct {
    char const *name;
    char const *contents;
} Fragment;

/** The templates {{include}} can find. */
static Fragment const FRAGMENTS[] = {
    {
        .name = "footer.html",
        .contents = "<footer>{{owner}}</footer>\n",
    },
    {
        .name = "defaults.html",
        .contents = "{{default owner \"nobody\"}}\n",
    },
    {
        .name = "nested/inner.html",
        .contents = "[{{include \"footer.html\"}}]",
    },
    {
        .name = "loop.html",
        .contents = "{{include \"loop.html\"}}",
    },
    {
        .name = "open.html",
        .contents = "{{if owner}}\nunclosed\n",
    },
    {
        .name = "end.html",
        .contents = "{{end}}\n",
    },
};

static bool find_fragment(void *context, Str name, Str *contents) {
    (void)context;
    for (size_t i = 0; i < sizeof FRAGMENTS / sizeof FRAGMENTS[0]; i++) {
        if (str_eq(name, str_from_cstr(FRAGMENTS[i].name))) {
            *contents = str_from_cstr(FRAGMENTS[i].contents);
            return true;
        }
    }
    return false;
}

/**
 * Renders a template into a buffer of the given size.
 */
static bool render(char const *source, char *buf, size_t capacity, Str_Builder *out,
                   Template_Error *error) {
    str_builder_init(out, buf, capacity);
    return template_render(STR("page.html"), str_from_cstr(source), find_fragment, NULL, out,
                           error);
}

static void expect_page(char const *source, char const *expected) {
    char buf[1024];
    Str_Builder out;
    Template_Error error;
    if (!render(source, buf, sizeof buf, &out, &error)) {
        printf("FAIL %s\n  error at line %zu: %.*s%.*s\n", source, error.line,
               STR_ARG(error.message), STR_ARG(error.detail));
        FAILURES++;
        return;
    }
    Str const got = str_builder_str(&out);
    if (!str_eq(got, str_from_cstr(expected))) {
        printf("FAIL %s\n  expected: %s\n  got:      %.*s\n", source, expected, STR_ARG(got));
        FAILURES++;
    }
}

static void expect_error(char const *source, size_t line, char const *message_part) {
    char buf[1024];
    Str_Builder out;
    Template_Error error;
    if (render(source, buf, sizeof buf, &out, &error)) {
        printf("FAIL %s\n  expected an error\n", source);
        FAILURES++;
        return;
    }
    if (error.line != line || !str_contains(error.message, str_from_cstr(message_part))) {
        printf("FAIL %s\n  expected line %zu with \"%s\"\n  got line %zu: %.*s%.*s\n", source,
               line, message_part, error.line, STR_ARG(error.message), STR_ARG(error.detail));
        FAILURES++;
    }
}

int main(void) {
    // Text and variables.
    expect_page("", "");
    expect_page("plain <b>text</b>\n", "plain <b>text</b>\n");
    expect_page("{{set a \"x &amp; y\"}}[{{a}}][{{ a }}]", "[x &amp; y][x &amp; y]");
    expect_page("{{set a \"1\"}}{{set a \"2\"}}{{a}}", "2");
    expect_page("{{set a \"\"}}[{{a}}]", "[]");

    // Statements alone on a line take the line with them; variables do not.
    expect_page("a\n{{set x \"1\"}}\nb\n", "a\nb\n");
    expect_page("a\n    {{set x \"1\"}}  \nb\n", "a\nb\n");
    expect_page("a\n{{set x \"1\"}}", "a\n");
    expect_page("a {{set x \"1\"}} b", "a  b");
    expect_page("{{set x \"1\"}}{{set y \"2\"}}\nb", "\nb");
    expect_page("a\r\n{{set x \"1\"}}\r\nb", "a\r\nb");

    // Default only sets what has no value.
    expect_page("{{default a \"1\"}}{{a}}", "1");
    expect_page("{{set a \"2\"}}{{default a \"1\"}}{{a}}", "2");
    expect_page("{{set a \"\"}}{{default a \"1\"}}{{a}}", "1");

    // Conditions.
    expect_page("{{if a}}yes{{else}}no{{end}}", "no");
    expect_page("{{set a \"x\"}}{{if a}}yes{{else}}no{{end}}", "yes");
    expect_page("{{set a \"\"}}{{if a}}yes{{else}}no{{end}}", "no");
    expect_page("{{set a \"x\"}}{{if a}}yes{{end}}!", "yes!");
    expect_page("{{if a}}yes{{end}}!", "!");
    expect_page("{{set a \"x\"}}{{if a == \"x\"}}eq{{else}}ne{{end}}", "eq");
    expect_page("{{set a \"x\"}}{{if a == \"y\"}}eq{{else}}ne{{end}}", "ne");
    expect_page("{{set a \"x\"}}{{if a != \"y\"}}ne{{else}}eq{{end}}", "ne");
    expect_page("{{if a == \"\"}}empty{{end}}", "empty");
    expect_page("{{set a \"x\"}}<a{{if a == \"x\"}} class=\"on\"{{end}}>", "<a class=\"on\">");
    expect_page("a\n{{if b}}\nb\n{{else}}\nc\n{{end}}\nd\n", "a\nc\nd\n");
    expect_page("{{set a \"1\"}}{{if a}}{{if b}}x{{else}}y{{end}}z{{else}}w{{end}}", "yz");
    expect_page("{{if a}}{{set b \"1\"}}{{end}}[{{if b}}set{{end}}]", "[]");
    expect_page("{{if a}}{{missing}}{{include \"nowhere.html\"}}{{end}}ok", "ok");

    // Includes.
    expect_page("{{set owner \"Hanna\"}}{{include \"footer.html\"}}", "<footer>Hanna</footer>\n");
    expect_page("{{set owner \"Hanna\"}}a\n{{include \"footer.html\"}}\nb\n",
                "a\n<footer>Hanna</footer>\nb\n");
    expect_page("{{set owner \"Hanna\"}}{{include \"nested/inner.html\"}}",
                "[<footer>Hanna</footer>\n]");
    expect_page("{{include \"defaults.html\"}}{{owner}}", "nobody");
    expect_page("{{set owner \"Hanna\"}}{{include \"defaults.html\"}}{{owner}}", "Hanna");

    // Mistakes are reported with their line.
    expect_error("a\nb\n{{missing}}\n", 3, "no value");
    expect_error("a\n{{unknown tag}}\n", 2, "unknown tag");
    expect_error("{{Upper}}", 1, "unknown tag");
    expect_error("a\n\n{{", 3, "not closed");
    expect_error("{{}}", 1, "empty");
    expect_error("{{set a}}", 1, "expected {{set");
    expect_error("{{set a b}}", 1, "expected {{set");
    expect_error("{{set if \"x\"}}", 1, "variable name");
    expect_error("{{set A \"x\"}}", 1, "variable name");
    expect_error("{{set a \"x}}", 1, "quote");
    expect_error("{{set a\"x\"}}", 1, "preceded by a space");
    expect_error("{{set a \"x\"y}}", 1, "followed by a space");
    expect_error("{{include footer.html}}", 1, "expected {{include");
    expect_error("{{include \"nowhere.html\"}}", 1, "cannot include");
    expect_error("{{include \"../secret.html\"}}", 1, "not a template name");
    expect_error("{{include \"/etc/passwd\"}}", 1, "not a template name");
    expect_error("{{include \"a//b.html\"}}", 1, "not a template name");
    expect_error("{{include \"a\\\\b.html\"}}", 1, "not a template name");
    expect_error("{{include \"loop.html\"}}", 1, "nested too deeply");
    expect_error("{{if}}", 1, "expected {{if");
    expect_error("{{if a b}}", 1, "expected {{if");
    expect_error("{{if a < \"b\"}}", 1, "expected {{if");
    expect_error("{{if a}}\n", 2, "without {{end}}");
    expect_error("{{else}}", 1, "without {{if}}");
    expect_error("{{end}}", 1, "without {{if}}");
    expect_error("{{if a}}{{else}}{{else}}{{end}}", 1, "second {{else}}");
    expect_error("{{set a \"1\"}}{{if a}}\n{{include \"end.html\"}}\n{{end}}", 1, "without {{if}}");
    expect_error("{{set owner \"x\"}}{{include \"open.html\"}}", 3, "without {{end}}");
    expect_error("{{set a \"1\"}}{{a b}}", 1, "unknown tag");

    // Limits.
    {
        char source[2048];
        size_t len = 0;
        for (int i = 0; i < TEMPLATE_MAX_VARIABLES + 1; i++) {
            len += (size_t)snprintf(source + len, sizeof source - len, "{{set v%d \"x\"}}", i);
        }
        expect_error(source, 1, "too many variables");

        len = 0;
        for (int i = 0; i < TEMPLATE_MAX_IFS + 1; i++) {
            len += (size_t)snprintf(source + len, sizeof source - len, "{{if a}}");
        }
        expect_error(source, 1, "too many {{if}}");
    }
    {
        // A page that does not fit in the output buffer.
        char buf[8];
        Str_Builder out;
        Template_Error error;
        if (render("0123456789", buf, sizeof buf, &out, &error)) {
            printf("FAIL output overflow was not reported\n");
            FAILURES++;
        }
    }

    if (FAILURES > 0) {
        printf("%d test(s) failed\n", FAILURES);
        return EXIT_FAILURE;
    }
    printf("template tests passed\n");
    return EXIT_SUCCESS;
}
