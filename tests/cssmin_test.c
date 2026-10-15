/**
 * @file cssmin_test.c
 * @brief Tests for the CSS minifier and bundling check.
 *
 * Run with ctest, or directly: cssmin-test. Exits with EXIT_FAILURE when a test fails.
 */

#include <stdio.h>
#include <stdlib.h>

#include "../src/cssmin.h"

static int FAILURES = 0;

static void expect_minified(char const *input, char const *expected) {
    char buf[4096];
    Str_Builder out;
    str_builder_init(&out, buf, sizeof buf);
    Minify_Error error;
    if (!css_minify(str_from_cstr(input), &out, &error)) {
        printf("FAIL %s\n  error on line %zu: %.*s\n", input, error.line, STR_ARG(error.message));
        FAILURES++;
        return;
    }
    if (!str_eq(str_builder_str(&out), str_from_cstr(expected))) {
        printf("FAIL %s\n  got  %.*s\n  want %s\n",
               input,
               STR_ARG(str_builder_str(&out)),
               expected);
        FAILURES++;
    }
}

static void expect_minify_error(char const *input, size_t line, char const *message) {
    char buf[4096];
    Str_Builder out;
    str_builder_init(&out, buf, sizeof buf);
    Minify_Error error;
    if (css_minify(str_from_cstr(input), &out, &error)) {
        printf("FAIL %s\n  minified without the expected error\n", input);
        FAILURES++;
        return;
    }
    if (error.line != line || !str_eq(error.message, str_from_cstr(message))) {
        printf("FAIL %s\n  got  line %zu: %.*s\n  want line %zu: %s\n",
               input,
               error.line,
               STR_ARG(error.message),
               line,
               message);
        FAILURES++;
    }
}

static void expect_check(char const *input, size_t line, char const *message) {
    Minify_Error error;
    bool const ok = css_check(str_from_cstr(input), &error);
    if (message == NULL) {
        if (!ok) {
            printf("FAIL check %s\n  unexpected error on line %zu: %.*s\n",
                   input,
                   error.line,
                   STR_ARG(error.message));
            FAILURES++;
        }
        return;
    }
    if (ok || error.line != line || !str_eq(error.message, str_from_cstr(message))) {
        printf("FAIL check %s\n  got  %s line %zu: %.*s\n  want line %zu: %s\n", input,
               ok ? "success," : "error", ok ? 0 : error.line,
               STR_ARG(ok ? STR("") : error.message), line, message);
        FAILURES++;
    }
}

int main(void) {
    // Comments and whitespace.
    expect_minified("/* Header. */\n.a {\n  color: red;\n  margin: 0 auto;\n}\n",
                    ".a{color:red;margin:0 auto}");
    expect_minified("a , b > c ~ d { x : y ; }", "a,b>c~d{x :y}");
    expect_minified(".a .b, .c:hover::after { }", ".a .b,.c:hover::after{}");
    expect_minified("p {}\n\n\n/* gap */\n\n.q {}", "p{}.q{}");

    // Spaces that change the meaning stay.
    expect_minified("a :hover { }", "a :hover{}");
    expect_minified("a + b { }", "a + b{}");
    expect_minified("a [href] { }", "a [href]{}");
    expect_minified("x { width: calc(100% - 2 * var(--gap)); }",
                    "x{width:calc(100% - 2 * var(--gap))}");
    expect_minified("@media screen and (max-width: 600px) { a { b: c } }",
                    "@media screen and (max-width:600px){a{b:c}}");
    expect_minified("x { transform: translate(1px, 2px) rotate(3deg); }",
                    "x{transform:translate(1px,2px) rotate(3deg)}");
    expect_minified("x { font: 12px / 1.5 serif; }", "x{font:12px / 1.5 serif}");
    expect_minified("x { color: red !important; }", "x{color:red!important}");
    expect_minified("x { margin: 1px/**/2px; }", "x{margin:1px 2px}");
    expect_minified(".a/**/.b { }", ".a.b{}");

    // Leading zeros.
    expect_minified("x { opacity: 0.5; margin: 0 0.25em -0.5em; color: rgba(0, 0, 0, 0.08); }",
                    "x{opacity:.5;margin:0 .25em -.5em;color:rgba(0,0,0,.08)}");
    expect_minified("x { width: 10.5px; flex: 0 0 0; }", "x{width:10.5px;flex:0 0 0}");
    expect_minified(".h0.5 { }", ".h0.5{}");

    // Strings, escapes and url() are kept as they are.
    expect_minified("x { content: ' a ; } /* b */ '; }", "x{content:' a ; } /* b */ '}");
    expect_minified("x { content: \"\\\"\"; }", "x{content:\"\\\"\"}");
    expect_minified("x { content: '\\1F33F'; }", "x{content:'\\1F33F'}");
    expect_minified(".\\31 0 { }", ".\\31 0{}");
    expect_minified("x { background: url( /a/b c.png ) no-repeat; }",
                    "x{background:url(/a/b c.png) no-repeat}");
    expect_minified("x { background: url( \"/a b.png\" ); }", "x{background:url(\"/a b.png\")}");
    expect_minified("x { background: url(http://example.com/a/*b*/.png); }",
                    "x{background:url(http://example.com/a/*b*/.png)}");

    // Custom property values are kept as written, only trimmed.
    expect_minified(":root {\n  --font: 'A', b,  c ;\n  --x:calc( 1px + 2px );\n"
                    "  --y: { a: b; };\n  --z: 0.5;\n}\n",
                    ":root{--font:'A', b,  c;--x:calc( 1px + 2px );--y:{ a: b; };--z:0.5}");
    expect_minified("a { --x: 1px }", "a{--x:1px}");
    expect_minified("a { color: var(--c, 0.5) ; }", "a{color:var(--c,.5)}");
    expect_minified("a--b:hover { }", "a--b:hover{}");

    // Errors.
    expect_minify_error("a { b: c; }\n/* open", 2, "unterminated comment");
    expect_minify_error("a {\n  content: 'x;\n}", 2, "unterminated string");
    expect_minify_error("a { b: c; }\n}", 2, "unbalanced }");
    expect_minify_error("a {\n  b: c;\n", 3, "unclosed {");
    expect_minify_error("a { background: url(x.png; }", 1, "unterminated url()");

    // The bundling check.
    expect_check("@media (min-width: 1px) { a { b: c } }\n@keyframes k { from { x: y } }", 0, NULL);
    expect_check("a { b: c }\n@import url(x.css);",
                 2,
                 "@import cannot be bundled; list the file in the manifest instead");
    expect_check("@CHARSET \"utf-8\";",
                 1,
                 "@charset is only valid at the start of a style sheet; "
                 "bundles are UTF-8 without it");
    expect_check("a { content: '@import' }", 0, NULL);
    expect_check("a { b: c", 1, "unclosed {");

    if (FAILURES > 0) {
        printf("%d failure%s\n", FAILURES, FAILURES == 1 ? "" : "s");
        return EXIT_FAILURE;
    }
    printf("all cssmin tests passed\n");
    return EXIT_SUCCESS;
}
