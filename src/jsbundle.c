/**
 * @file jsbundle.c
 * @brief kasin-jsbundle: builds the site's JavaScript bundles from js/.
 *
 * Usage: kasin-jsbundle [-n] [-w] -o <out_dir> <manifest>
 *
 * See bundler.h for the manifest, the rules that keep the bundles apart and
 * watch mode. On top of those, every bundle runs in its own function scope in
 * strict mode, so nothing it declares becomes a global that another bundle
 * could see or overwrite. All files of a bundle share that scope, so the tool
 * refuses to build a bundle in which two files declare the same top-level name.
 *
 * The sources are minified by jsmin.h.
 */

#include <stdio.h>

#include "bundler.h"
#include "jsmin.h"
#include "str.h"

#define MAX_NAMES 4096

/**
 * Checks that no two sources of a bundle declare the same top-level name.
 */
static bool check_names(Arena *arena, Bundle const *bundle, Str const *contents) {
    Js_Name *names = arena_alloc(arena, MAX_NAMES * sizeof *names, _Alignof(Js_Name));
    size_t *owner = arena_alloc(arena, MAX_NAMES * sizeof *owner, _Alignof(size_t));
    if (names == NULL || owner == NULL) {
        fprintf(stderr, "out of memory\n");
        return false;
    }
    size_t total = 0;
    bool ok = true;
    for (size_t i = 0; i < bundle->source_count; i++) {
        Minify_Error error;
        size_t count;
        if (!js_top_level_names(contents[i], names + total, MAX_NAMES - total, &count, &error)) {
            bundler_print_error(bundle->sources[i], &error);
            return false;
        }
        for (size_t n = total; n < total + count; n++) {
            owner[n] = i;
            for (size_t m = 0; m < n; m++) {
                if (owner[m] != i && str_eq(names[m].name, names[n].name)) {
                    fprintf(stderr, "bundle \"%.*s\": %.*s:%zu and %.*s:%zu both declare \"%.*s\" at the top level\n",
                            STR_ARG(bundle->name),
                            STR_ARG(bundle->sources[owner[m]]), names[m].line,
                            STR_ARG(bundle->sources[i]), names[n].line,
                            STR_ARG(names[n].name));
                    ok = false;
                }
            }
        }
        total += count;
    }
    return ok;
}

/**
 * Files are separated by a line break. A file that starts with ( [ ` + - or /
 * could continue the previous file's last statement across it when that did
 * not end with ";": then the separator becomes ";".
 */
static void join_source(Str_Builder *out, size_t separator, size_t index) {
    if (index > 0 && out->len > separator + 1 &&
        str_find_char(STR("([`+-/"), out->data[separator + 1]) != STR_NPOS &&
        out->data[separator - 1] != ';') {
        out->data[separator] = ';';
    }
}

/**
 * A last statement without ";" would otherwise run into the wrapper's end.
 */
static void end_sources(Str_Builder *out) {
    char const last = out->data[out->len - 1];
    if (last != ';' && last != '}') {
        str_builder_append_char(out, ';');
    }
}

static Bundle_Kind const JS_BUNDLE = {
    .program = "kasin-jsbundle",
    .source_root = STR_INIT("js"),
    .extension = STR_INIT(".js"),
    .comment_start = STR_INIT("// "),
    .comment_end = STR_INIT("\n"),
    .prologue = STR_INIT("(function(){'use strict';\n"),
    .prologue_plain = STR_INIT("(function () {\n'use strict';\n"),
    .epilogue = STR_INIT("\n})();\n"),
    .epilogue_plain = STR_INIT("\n})();\n"),
    .separator = STR_INIT("\n"),
    .plain_source_end = STR_INIT("\n;"),
    .minify = js_minify,
    .check = check_names,
    .after_source = join_source,
    .after_sources = end_sources,
};

int main(int argc, char **argv) {
    return bundler_main(argc, argv, &JS_BUNDLE);
}
