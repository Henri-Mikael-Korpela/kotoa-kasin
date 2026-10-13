/**
 * @file cssbundle.c
 * @brief kasin-cssbundle: builds the site's style sheet bundles from css/.
 *
 * Usage: kasin-cssbundle [-n] [-w] -o <out_dir> <manifest>
 *
 * See bundler.h for the manifest, the rules that keep the bundles apart and
 * watch mode. The sources are joined in the manifest's order, which is also the
 * cascade order, and minified by cssmin.h. A source may not use @import or
 * @charset, which only work at the start of a style sheet.
 *
 * url() values are not rewritten: write them relative to the bundles' directory
 * (/css/), where the browser resolves them.
 */

#include "bundler.h"
#include "cssmin.h"
#include "str.h"

/**
 * Checks that every source can be joined with the others.
 */
static bool check_sources(Arena *arena, Bundle const *bundle, Str const *contents) {
    (void)arena;
    bool ok = true;
    for (size_t i = 0; i < bundle->source_count; i++) {
        Minify_Error error;
        if (!css_check(contents[i], &error)) {
            bundler_print_error(bundle->sources[i], &error);
            ok = false;
        }
    }
    return ok;
}

static Bundle_Kind const CSS_BUNDLE = {
    .program = "kasin-cssbundle",
    .source_root = STR_INIT("css"),
    .extension = STR_INIT(".css"),
    .comment_start = STR_INIT("/* "),
    .comment_end = STR_INIT(" */\n"),
    .prologue = STR_INIT(""),
    .prologue_plain = STR_INIT(""),
    .epilogue = STR_INIT("\n"),
    .epilogue_plain = STR_INIT(""),
    .separator = STR_INIT("\n"),
    .plain_source_end = STR_INIT("\n"),
    .minify = css_minify,
    .check = check_sources,
    .after_source = NULL,
    .after_sources = NULL,
};

int main(int argc, char **argv) {
    return bundler_main(argc, argv, &CSS_BUNDLE);
}
