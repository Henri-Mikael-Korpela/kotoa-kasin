# C Programming Style

## Spacing

- Use 4 spaces for indentation, do not use tabs.
- Use 1 space between a keyword and its opening parenthesis.
- Do not use space between function name and opening bracket

## Comments

- For documentation comments, use Doxygen style.
- Use double-asterisk only for Doxygen documentation.
- For multi-line comments use space+asterisk for every line.
- Use `//` for single line comment.
- Always use dot to end comments when comment forms one or more sentences.

Example documentation comment:

```c
/**
 * This is multi-line comments,
 * written in 2 lines (ok).
 */
```

Example single-line comment:

```c
// This is a single-line comment.
```

## Naming conventions

- Use SCREAMING_SNAKE_CASE for constants and global variables.
- Use snake_case for functions and local variables.
- Use Ada case for type names (e.g. `HTTP_Request`).
- Never use __ or _ prefix for variables/functions/macros/types. The only exception is header guards (see Header files).

## Header files

- Every header file of own code has a header guard named `__KASIN_<NAME>_H__`, where `<NAME>` is the file name in SCREAMING_SNAKE_CASE without the extension, e.g. `__KASIN_STR_H__` for `str.h` and `__KASIN_HTTP_REQUEST_H__` for `http_request.h`. The `KASIN_` prefix marks the guard as belonging to this project.
- Use `#ifndef` and `#define` with that name at the top of the file, and close the guard with `#endif` followed by a `//` comment that contains the name of the header guard.

Example header guard:

```c
#ifndef __KASIN_STR_H__
#define __KASIN_STR_H__

// Declarations.

#endif  // __KASIN_STR_H__
```

## Scopes

- Place opening curly bracket always at the same line as function signature.
- Place opening curly bracket always at the same line as keyword (for, while, do, switch, ...).
- Use curly brackets for if and loop statements, do not use single statement bodies without explicit curly brackets.

## Constants

- Place `const` always after the type name, e.g. `int const x = 0;`.

## Strings

- Use Str type for strings whenever possible, do not use `char*` or `char[]` for strings. Only when you need to use C string functions, use `char*` or `char[]` for strings.

## Structures

- No single line struct initializer lists, split each field on its own line.
- Always add trailing comma in the last element of structure (or its children) initialization.

# JavaScript Programming Style

## Spacing

- Use 4 spaces for indentation, do not use tabs.

## Variables

- Use `const` for variables that are not reassigned, and `let` for variables that are reassigned. Do not use `var`.

## Functions

- Mark all functions using `function` keyword, do not use arrow functions. Use arrow functions only for callbacks.

## Paradigms

- Prefer pure functions over side effects. Avoid mutating objects and arrays, instead create new ones with the changes unless performance is a concern.
- Use imperative style whenever possible, avoid functional style unless it makes the code more readable. Avoid object-oriented style.

## Object-oriented programming

- Do not use inheritance.