/**
 * @file minify.h
 * @brief What the minifiers (jsmin.h, cssmin.h) report when they cannot read a source.
 */

#ifndef __KASIN_MINIFY_H__
#define __KASIN_MINIFY_H__

#include <stddef.h>

#include "str.h"

typedef struct {
    size_t line;     /**< 1-based line in the source where the problem was found, or 0 for none. */
    Str    message;  /**< What went wrong; points to static text. */
} Minify_Error;

#endif  // __KASIN_MINIFY_H__
