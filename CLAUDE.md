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
- Never use __ or _ prefix for variables/functions/macros/types.

## Scopes

- Place opening curly bracket always at the same line as function signature.
- Place opening curly bracket always at the same line as keyword (for, while, do, switch, ...).
- Use curly brackets for if and loop statements, do not use single statement bodies without explicit curly brackets.

## Constants

- Place `const` always after the type name, e.g. `int const x = 0;`.

## Structures

- No single line struct initializer lists, split each field on its own line.
- Always add trailing comma in the last element of structure (or its children) initialization.