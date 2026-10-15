/**
 * @file bundler.h
 * @brief The part of the bundling tools (kasin-jsbundle, kasin-cssbundle) that
 *        does not depend on the language: the manifest, files and watch mode.
 *
 * Usage of a tool: <program> [-n] [-w] -o <out_dir> <manifest>
 *
 * The manifest lists the bundles and, under each, its source files in load
 * order, relative to the manifest's directory:
 *
 *     # Comment.
 *     site:
 *         shared/blocks.js
 *         site/main.js
 *
 * Each bundle is written to <out_dir>/<name><extension>. To keep the bundles
 * apart, a bundle may only use sources from shared/ and from the directory
 * named after it: the "site" bundle can never pick up admin code, nor the
 * other way round.
 *
 * The sources are minified unless -n is given, which keeps them as they are
 * for debugging.
 *
 * With -w the tool keeps running and rebuilds the bundles whenever the manifest
 * or one of its sources changes. Errors are reported and the tool waits for the
 * next change. It polls file times instead of using inotify, which does not see
 * changes made through the Windows bind mounts of Docker Desktop.
 *
 * Like the server, the tools allocate nothing after startup: one mmap()
 * reserves all their memory, carved by an arena.
 */

#ifndef __KASIN_BUNDLER_H__
#define __KASIN_BUNDLER_H__

#include <stdbool.h>
#include <stddef.h>

#include "arena.h"
#include "minify.h"
#include "str.h"

enum {
    /** Most source files in one bundle. */
    BUNDLER_MAX_SOURCES = 64,
};

typedef struct {
    Str    name;
    size_t line;  /**< Manifest line of the bundle's heading. */
    Str    sources[BUNDLER_MAX_SOURCES];
    size_t source_count;
} Bundle;

/**
 * What one tool does differently: its language.
 */
typedef struct {
    char const *program;          /**< The tool's name, for usage and the header of each bundle. */
    Str         source_root;      /**< Directory of the sources, for the header: "js" or "css". */
    Str         extension;        /**< Of sources and bundles: ".js" or ".css". */
    /** Starts a one-line comment in the language: two slashes, or slash-star. */
    Str         comment_start;
    /** Ends it, including the line break: just the line break, or star-slash. */
    Str         comment_end;
    Str         prologue;         /**< Written after the header when minifying. */
    Str         prologue_plain;   /**< The same with -n. */
    Str         epilogue;         /**< Written after the sources when minifying. */
    Str         epilogue_plain;   /**< The same with -n. */
    Str         separator;        /**< Written between minified sources. */
    Str         plain_source_end; /**< Written after each source with -n. */

    /**
     * Minifies one source, appending to out.
     */
    bool (*minify)(Str source, Str_Builder *out, Minify_Error *error);

    /**
     * Checks a bundle's sources before they are written, with or without -n,
     * and prints what is wrong. May be NULL.
     *
     * @param arena Scratch memory, given back after the bundle is built.
     * @param contents The contents of the bundle's sources, in order.
     */
    bool (*check)(Arena *arena, Bundle const *bundle, Str const *contents);

    /**
     * Called after each minified source has been appended, to adjust how it
     * joins the previous one. May be NULL.
     *
     * @param separator Where the separator before this source starts in out.
     * @param index The source's position in the bundle.
     */
    void (*after_source)(Str_Builder *out, size_t separator, size_t index);

    /**
     * Called after the last minified source, before the epilogue. May be NULL.
     */
    void (*after_sources)(Str_Builder *out);
} Bundle_Kind;

/**
 * Prints a minifier's error for a source as "<source>:<line>: <message>".
 */
void bundler_print_error(Str source, Minify_Error const *error);

/**
 * Runs a bundling tool: parses the command line and builds, or watches.
 *
 * @return The process exit status: EXIT_SUCCESS or a sysexits.h code.
 */
int bundler_main(int argc, char **argv, Bundle_Kind const *kind);

#endif  // __KASIN_BUNDLER_H__
