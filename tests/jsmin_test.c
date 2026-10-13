/**
 * @file jsmin_test.c
 * @brief Tests for the JavaScript minifier and top-level name scanner.
 *
 * Run with ctest, or directly: jsmin-test. Exits with EXIT_FAILURE when a test fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/jsmin.h"

static int FAILURES = 0;

static void expect_minified(char const *input, char const *expected) {
    char buf[4096];
    Str_Builder out;
    str_builder_init(&out, buf, sizeof buf);
    Js_Error error;
    if (!js_minify(str_from_cstr(input), &out, &error)) {
        printf("FAIL %s\n  error on line %zu: %.*s\n", input, error.line, STR_ARG(error.message));
        FAILURES++;
        return;
    }
    if (!str_eq(str_builder_str(&out), str_from_cstr(expected))) {
        printf("FAIL %s\n  got  %.*s\n  want %s\n", input, STR_ARG(str_builder_str(&out)), expected);
        FAILURES++;
    }
}

static void expect_error(char const *input, size_t line, char const *message) {
    char buf[4096];
    Str_Builder out;
    str_builder_init(&out, buf, sizeof buf);
    Js_Error error;
    if (js_minify(str_from_cstr(input), &out, &error)) {
        printf("FAIL %s\n  minified without the expected error\n", input);
        FAILURES++;
        return;
    }
    if (error.line != line || !str_eq(error.message, str_from_cstr(message))) {
        printf("FAIL %s\n  got  line %zu: %.*s\n  want line %zu: %s\n", input, error.line, STR_ARG(error.message), line, message);
        FAILURES++;
    }
}

static void expect_names(char const *input, char const *expected) {
    Js_Name names[32];
    size_t count;
    Js_Error error;
    if (!js_top_level_names(str_from_cstr(input), names, 32, &count, &error)) {
        printf("FAIL %s\n  error on line %zu: %.*s\n", input, error.line, STR_ARG(error.message));
        FAILURES++;
        return;
    }
    char buf[512];
    Str_Builder got;
    str_builder_init(&got, buf, sizeof buf);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            str_builder_append_char(&got, ' ');
        }
        str_builder_append(&got, names[i].name);
        str_builder_append_char(&got, '@');
        str_builder_append_u64(&got, names[i].line);
    }
    if (!str_eq(str_builder_str(&got), str_from_cstr(expected))) {
        printf("FAIL names of %s\n  got  %.*s\n  want %s\n", input, STR_ARG(str_builder_str(&got)), expected);
        FAILURES++;
    }
}

int main(void) {
    // Whitespace and comments.
    expect_minified("const a = 1;\nconst b = 2;\n", "const a=1;const b=2;");
    expect_minified("a = b; // c\n/* d\n e */ f();", "a=b;f();");
    expect_minified("\xEF\xBB\xBF" "a\xC2\xA0=\t1;", "a=1;");

    // Line breaks stay where a semicolon could be inserted.
    expect_minified("a = b\nc()", "a=b\nc()");
    expect_minified("a = b\n(c)", "a=b\n(c)");
    expect_minified("function f() {\n    return\n    1;\n}", "function f(){return\n1}");
    expect_minified("a\n++b", "a\n++b");
    expect_minified("a = b /* x\n */ c", "a=b\nc");
    expect_minified("a = b\n.c", "a=b.c");

    // Spaces stay where tokens would merge.
    expect_minified("a = b + +c; d = e - -f; g = h + ++i;", "a=b+ +c;d=e- -f;g=h+ ++i;");
    expect_minified("a = 1 .toString(); b = x in y; c = typeof d;", "a=1 .toString();b=x in y;c=typeof d;");
    expect_minified("x = a < !b; y = c-- > d;", "x=a< !b;y=c-- >d;");
    expect_minified("function f() { return 'x'; }", "function f(){return'x'}");
    expect_minified("a = .5 + 1.5e-3 + 0x1F + 10n;", "a=.5+1.5e-3+0x1F+10n;");

    // Strings, regular expressions and templates are kept as they are.
    expect_minified("s = 'a  b' + \"c // d\" + 'e\\'  f';", "s='a  b'+\"c // d\"+'e\\'  f';");
    expect_minified("x = a / b / c; y = /[/]\\//g;", "x=a/b/c;y=/[/]\\//g;");
    expect_minified("x = /a/g in y;", "x=/a/g in y;");
    expect_minified("if (/ a /.test(s)) { return / b /i; }", "if(/ a /.test(s)){return/ b /i}");
    expect_minified("x = s.replace(/<!--/g, '<\\\\!--');", "x=s.replace(/<!--/g,'<\\\\!--');");
    expect_minified("t = `a ${ b + `c ${ d }` }  e`;", "t=`a ${b+`c ${d}`}  e`;");
    expect_minified("t = `a\n  b`;\nu = `${ {x: 1}.x }`;", "t=`a\n  b`;u=`${{x:1}.x}`;");
    expect_minified("a = b ? .5 : c?.d;", "a=b?.5:c?.d;");

    // Optional semicolons and trailing commas.
    expect_minified("o = { a: 1, b: [1, 2,], };", "o={a:1,b:[1,2]};");
    expect_minified("h = [, ]; i = [1, , ];", "h=[,];i=[1,,];");
    expect_minified("f(a, b,);", "f(a,b);");
    expect_minified("function f() { g(); }", "function f(){g()}");
    expect_minified("function f() { if (x) ; }", "function f(){if(x);}");
    expect_minified("function f() { while (g(x)) ; }", "function f(){while(g(x));}");
    expect_minified("function f() { if (x) {} else ; }", "function f(){if(x){}else;}");
    expect_minified("function f() { a: ; }", "function f(){a:;}");
    expect_minified("for (;;) { break; }", "for(;;){break}");

    // Errors.
    expect_error("a = 1;\nb = 'x;\n", 2, "unterminated string");
    expect_error("a = 1;\n/* x", 2, "unterminated comment");
    expect_error("a = `x", 1, "unterminated template literal");
    expect_error("a = 1;\nb = /x\n/;", 2, "unterminated regular expression");
    expect_error("}", 1, "unbalanced }");
    expect_error("function f() {\n", 2, "unclosed { or template substitution");

    // Top-level declarations.
    expect_names("const A = 1, B = f(x, y);\nfunction g() { const C = 1; }\nclass D {}\n"
                 "const E = (function () { function F() {} return { G: 1, H }; })();\n"
                 "let I = { J: 1, K: 2 }, L;\nasync function M() {}\nfunction* N() {}\nvar O = `${P}`;",
                 "A@1 B@1 g@2 D@3 E@4 I@5 L@5 M@6 N@7 O@8");
    expect_names("x = function y() {};\nif (a) { var b = 1; }", "");

    if (FAILURES > 0) {
        printf("%d failure%s\n", FAILURES, FAILURES == 1 ? "" : "s");
        return EXIT_FAILURE;
    }
    printf("all jsmin tests passed\n");
    return EXIT_SUCCESS;
}
