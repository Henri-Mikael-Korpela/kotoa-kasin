/**
 * @file cssmin.h
 * @brief CSS minifier and bundling check.
 *
 * The minifier removes comments and whitespace, keeping a space only where it
 * can change the meaning: between two words ("a .b", "solid red"), before a
 * pseudo-class ("a :hover" is not "a:hover"), before "(" ("and (" is not the
 * function "and(") and around "+", "-", "*", "/", "=" and "[" (calc() needs
 * spaces around + and -). It also drops the ";" before "}" and the 0 of 0.5.
 * Strings and unquoted url() values are copied as they are. Nothing else is
 * changed, so the output means exactly what the input does.
 *
 * Neither function allocates: output goes to a caller-provided builder.
 */

#ifndef __KASIN_CSSMIN_H__
#define __KASIN_CSSMIN_H__

#include <stdbool.h>

#include "minify.h"
#include "str.h"

/**
 * Minifies a style sheet.
 *
 * @param source The style sheet.
 * @param out Builder that receives the minified CSS; it is appended to.
 * @param error Receives the line and reason when the style sheet cannot be
 *              read (an unterminated comment, string or url(), unbalanced
 *              braces) or the output does not fit.
 * @return True on success.
 */
bool css_minify(Str source, Str_Builder *out, Minify_Error *error);

/**
 * Checks that a style sheet can be bundled: its braces balance, and it has no
 * "@import" or "@charset", which are only valid at the start of a style sheet
 * and so would break or be ignored once files are joined.
 *
 * @param error Receives the line and reason when it cannot.
 * @return True when the style sheet can be bundled.
 */
bool css_check(Str source, Minify_Error *error);

#endif  // __KASIN_CSSMIN_H__
