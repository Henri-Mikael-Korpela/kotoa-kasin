/**
 * @file jsmin.h
 * @brief JavaScript minifier and top-level declaration scanner.
 *
 * The minifier works on tokens, not on a syntax tree: it drops comments and
 * whitespace, and keeps a space only where two tokens would otherwise run
 * together and a line break only where automatic semicolon insertion could
 * depend on it. It also drops semicolons right before "}" and trailing commas
 * where the language makes them optional. Names are never renamed, so the
 * output behaves exactly like the input.
 *
 * Neither function allocates: output goes to a caller-provided builder or array.
 */

#ifndef __KASIN_JSMIN_H__
#define __KASIN_JSMIN_H__

#include <stdbool.h>
#include <stddef.h>

#include "minify.h"
#include "str.h"

typedef struct {
    Str    name;  /**< The declared name, a slice of the source. */
    size_t line;  /**< 1-based line of the declaration. */
} Js_Name;

/**
 * Minifies JavaScript source code.
 *
 * @param source The code.
 * @param out Builder that receives the minified code; it is appended to.
 * @param error Receives the line and reason when the code cannot be read
 *              (an unterminated string, comment, template or regular
 *              expression, unbalanced braces) or the output does not fit.
 * @return True on success.
 */
bool js_minify(Str source, Str_Builder *out, Minify_Error *error);

/**
 * Lists the names a script declares at its top level with const, let, var,
 * function or class. In a bundle all files share one scope, so two files
 * declaring the same name would clash.
 *
 * @param source The code.
 * @param names Receives the names, in order of appearance.
 * @param capacity Number of entries names has room for.
 * @param count Receives the number of names found.
 * @param error Receives the line and reason when the code cannot be read or
 *              there are more names than fit.
 * @return True on success.
 */
bool js_top_level_names(Str source, Js_Name *names, size_t capacity, size_t *count, Minify_Error *error);

#endif  // __KASIN_JSMIN_H__
