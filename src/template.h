/**
 * @file template.h
 * @brief The HTML template engine behind kasin-htmlgen: variables, includes and conditions.
 *
 * A template is text in which {{ ... }} tags are replaced:
 *
 *     {{name}}                  The value of a variable.
 *     {{set name "value"}}      Sets a variable.
 *     {{default name "value"}}  Sets a variable unless it already has a value.
 *     {{include "file.html"}}   Inserts another template, which is processed in turn.
 *     {{if name}}               The text up to {{else}} or {{end}} is used when the variable
 *                               has a non-empty value.
 *     {{if name == "value"}}    The same when the value equals; != when it does not.
 *     {{else}}
 *     {{end}}
 *
 * Variables are global to one page: whatever a page or an included template sets, all
 * the others see. A variable that was never set is empty in {{if}} and an error in
 * {{name}}. Values are inserted as they are, without escaping, because they are written
 * by the site's author and are HTML themselves.
 *
 * A tag that is alone on its line (set, default, include, if, else and end) takes the
 * whole line, with its line break, along with it. A template can therefore be laid out
 * for reading without leaving blank lines in the page.
 *
 * Nothing is recursive or unbounded: includes nest at most TEMPLATE_MAX_DEPTH deep, there
 * are at most TEMPLATE_MAX_VARIABLES variables and {{if}}s nest at most TEMPLATE_MAX_IFS.
 * See docs/html-templating.md.
 */

#ifndef __KASIN_TEMPLATE_H__
#define __KASIN_TEMPLATE_H__

#include <stdbool.h>
#include <stddef.h>

#include "str.h"

enum {
    /** Most templates being processed at once: a page and the includes it is inside. */
    TEMPLATE_MAX_DEPTH = 8,
    /** Most variables one page can have. */
    TEMPLATE_MAX_VARIABLES = 32,
    /** Most {{if}}s open at once, over a page and all its includes. */
    TEMPLATE_MAX_IFS = 16,
    /** Most words and quoted strings in one tag. */
    TEMPLATE_MAX_WORDS = 4,
};

/**
 * What went wrong, for printing as "<file>:<line>: <message><detail>".
 */
typedef struct {
    Str    file;     /**< The template the error is in. */
    size_t line;     /**< Its line, counting from 1. */
    Str    message;  /**< What is wrong; a string literal. */
    Str    detail;   /**< The name or text it is about; may be empty. */
} Template_Error;

/**
 * Finds an included template.
 *
 * @param context Passed on from template_render().
 * @param name The name in the {{include}} tag, for example "footer.html".
 * @param contents Receives the text of the template, which must stay valid until
 *                 template_render() returns.
 * @return False when there is no such template or it cannot be read. The page then fails
 *         with "cannot include <name>"; the loader may print the reason itself.
 */
typedef bool (*Template_Loader)(void *context, Str name, Str *contents);

/**
 * Processes a template into a page.
 *
 * @param name The template's name, for errors.
 * @param source The text of the template.
 * @param load Finds the templates {{include}} asks for.
 * @param context Given to load.
 * @param out Receives the page. An overflow is reported as an error.
 * @param error Receives what went wrong when this returns false.
 * @return True when the whole page was made.
 */
bool template_render(Str name, Str source, Template_Loader load, void *context,
                     Str_Builder *out, Template_Error *error);

/**
 * Prints an error as "<file>:<line>: <message><detail>" to stderr.
 */
void template_print_error(Template_Error const *error);

#endif  // __KASIN_TEMPLATE_H__
