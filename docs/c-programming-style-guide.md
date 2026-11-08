# C Programming Style Guide

## Purpose

This document describes how to use the C programming language and associated tooling. This document is useful for not only developers, but for AI agents as well to keep code more consistent.

Do not run any formatter that works against these rules.

## C Standard

- Pick one C standard for the whole code base and state it in the build. Build with the matching `-std=` flag, GNU extensions off, and `-Wpedantic -Werror`, so that any use of a newer or non-standard feature fails the build.
- `static_assert` requires C11; under an older standard, provide a portable fallback macro.

## Indentation, lines and spacing

- Use 4 spaces for indentation, never tabs.
- Keep lines to at most 100 columns, with no exceptions for long strings other than URLs.
- Always put a space after keywords (`if (`, `for (`, `while (`), but not after function names (`assert(x)`).
- Add spaces to separate parts of function forward declarations when there is more than one signature declared in a row. Ensure function names and parameter lists (beginning with an opening parenthesis) are aligned in their own columns.
    - This way forward declarations read like a table, which is better for readability.

## File layout

- Use the following order:
    - includes
        - standard library
        - third-party
        - project specific
    - macros and constants
    - types
    - function declarations
    - function definitions

Put `main` at the top of the file when it is the entry point, and the most important functions first.

## Comments and documentation

- Do not use single line comments above function definitions. Instead, use `/**` to start a documentation comment. Example:

```c
/**
 * This is an example documentation comment.
 * It is split on multiple lines.
 */
```

- Use `//` for line comments. Comments are complete sentences, with a capital letter, a space after the slashes, and a full stop. Write inline comments (after code) as phrases and omit the full stop.
- Always say why. The code shows how; the comment explains the reason, the trade-off, or the thing that is not obvious.
- Explain the "why" for every non-obvious choice, including the choice of a bound, a magic number, or an unusual order of operations.
- Keep documentation, comments and code in sync, and update comments when the code changes.

## Naming

- Use `snake_case` for functions, variables, parameters and file names.
- Use Ada case for types (`Header_Flags`, `Task_Completion`: each word capitalized, words joined by underscores; a one-word type is just capitalized, like `Event`).
- Use `SCREAMING_SNAKE_CASE` for macros and constants.
    - The rule is for types we define, not for third-party or libc types.
- Do not abbreviate names (except for loop indexes `i`, `j`, and widely understood terms such as `x`, `y`).
- Use proper capitalization for acronyms (`HTTP_Response`, not `Http_Response`) where they appear in type names.
- Add units or qualifiers to variable names, and put them last, so that related names sort together and read in order of significance: `latency_ms_max` rather than `max_latency_ms`; `width_pixels`, `cell_height_pixels`.
- Name things so that related names have the same length where it helps alignment, for example `source` and `target` rather than `src` and `dest`.
- Use nouns for types and variables, and verbs for functions. Name a function for what it does; callbacks and predicates read as questions (`is_leap_year`).
- Distinguish between an index (0-based), a count (1-based, the number of things) and a size (count times unit): `index`, `count`, `size_bytes`. Use `index < count` and `index <= count_max`.
- Prefix helper functions with the name of the function that calls them, when they are used in only one place: `draw_grid` and `draw_grid_cell`. Callbacks go last in the parameter list.
- Struct and type names are in the order of the most significant part first.
- Do not overload names with different meanings. Do not use the name `data`, `info`, `value` or `tmp` when a precise name exists.

## Global and local variables

- Initialize every variable at declaration.
- Declare variables at the smallest possible scope, and as close as possible to where they are used, to reduce the chance of use before initialization.
- Do not use global mutable state. Pass state explicitly through a context struct.

## Constants

- Use `static const` or `enum` for constants rather than `#define` where possible. Use `#define` only for macros that must be macros. Give every constant a unit or comment.
- Place `const` after type name (e.g. `int const`).

## Strings

- Prefer length-based strings (a pointer plus a length) over C strings (`char *`, `const char *`): pass text as a length-based string, return it as one, and slice, trim, split, compare and parse it with functions that cannot run past the end because they never look for a terminator. A length-based string does not own its bytes; it is a view into a literal, a buffer or a message.
- Use a C string only where something outside the code requires one: third-party libraries, the C library and the operating system (paths for `fopen`, host names for `getaddrinfo`, text that is formatted with `snprintf`). Convert at that boundary, once, on the way in and on the way out, and do not let the C string travel further into the program than the call that needs it.
- A fixed `char` array that a library writes into or reads from stays a `char` array, and is turned into a length-based string as soon as the program reads it.
- Build text in a string builder over a buffer the caller owns, rather than with `strcat` or by hand with `strlen` arithmetic.

## Scopes

- Function statements should have their beginning curly brace on the same line as the function signature.
- If statements should have their beginning curly brace on the same line as the condition.
- Loops (`for`, `while`, `do`) and `switch` follow the same curly brace placement.

## Data types

- Use explicitly sized types (`int32_t`, `uint32_t`, `uint64_t`, `size_t` only for sizes and indexes of memory). Avoid `int`, `long` and `unsigned` where the range matters.
    - Where an external API forces `int` or `float`, convert at the boundary.
- Use `bool` for booleans, never `int`.
- Define and use a custom type `byte` using `typedef` to represent bytes instead of `char`, `unsigned char` or `uint8_t`.

## Control flow

- If statements must have their bodies in curly brackets. No single line if statement body without curly brackets.
- Split compound conditions into nested `if` statements where that makes each branch clear. Prefer positive conditions (`if (index < length)`) over negations.
- State invariants positively. For `else` branches, make sure the case is handled or deliberately asserted unreachable.
- Handle every case in a `switch` explicitly. Do not add a `default:` that hides a missing case; assert unreachable instead.
- When matching against an enumeration in a `switch` statement, sort enumeration keys alphabetically unless performance matters. If it's likely one key should come before another key, comment this and explain the performance-related reason for this.
- Do not use the ternary operator unless mandatory.
- Use only simple, explicit control flow. Do not use `goto` (other than a single cleanup label where unavoidable), `setjmp`/`longjmp`, or recursion. All executions that should be bounded must be bounded.
- Put a fixed upper bound on every loop and every queue or buffer, so that a runaway loop cannot occur. A loop that must run forever, such as the main loop, should be the only exception and should say so.
- Use `while (true)` for infinite loops, not `for (;;)` or `while (1)`.

## Functions

- Good function shape is a few parameters and a simple return type. Prefer passing structs by `const` pointer when large, and by value when small.
- Mark everything that can be `static` as `static`. Mark pointer parameters `const` when the function does not modify the data.
- Keep control flow (`if`, `switch`, loops) in the parent function and pure computation (no I/O, no side effects) in helpers. Keep leaf functions pure.
- Do not duplicate or alias variables or state, since duplication creates opportunities for them to drift out of sync.
- Avoid out parameters where a return value will do; if you must use them, assert on them.
- Sort function forward declarations alphabetically.

## Structs

- Put a declaration of each struct field on its own line, and order fields from largest to smallest alignment so that padding is minimal. Assert the struct size with `static_assert` when layout matters.
- Put each field in struct object initialization on its own line. Use a trailing comma for each line, because this helps when one uses a line-based change review tool like Git.

## Assertions

- Use `assert` (from `<assert.h>`) liberally to check function arguments, return values, pre- and post-conditions, and invariants. Aim for at least two assertions per function on average.
- Assert the positive space (what you expect) and the negative space (what you do not expect). The boundary between valid and invalid data is where bugs live.
- Assertions detect programmer errors and must crash. Never use them for expected, recoverable conditions such as I/O failure; handle those by other means.
- Pair assertions: check important properties at two different code paths where possible, for example when writing and when reading.
- Split compound assertions: `assert(a); assert(b);` rather than `assert(a && b);`.
- Use `static_assert` (C11 `_Static_assert`) to check compile-time constants, sizes and relationships between them.
- Assertions must not have side effects, because they may be compiled out with `NDEBUG`.
- Ensure there's an empty line after function precondition asserts and static_asserts.

## Error handling

- All errors must be handled. Check every return value that can fail; do not ignore results. Do not leave `(void)` casts on fallible calls without a comment explaining why.
- Compile with all warnings on (`-Wall -Wextra`, `/W4` on MSVC) and treat warnings as errors in our own code. Fix the cause rather than suppressing the warning.

## Type casting

- Do not use implicit conversions that lose information. Be explicit with casts, and keep them rare.

## Preprocessor macros

- Avoid complex macros. If a macro has side effects or evaluates its argument more than once, it must be named in capitals and commented.

## Undefined behavior

- Do not rely on undefined behavior. Be careful with signed overflow, shifts, strict aliasing, uninitialized reads and pointer arithmetic. Use unsigned types for wraparound arithmetic.

# Project-specific guidelines

## Header files

- Every header file has a header guard named `__KASIN_<NAME>_H__`, where `<NAME>` is the file name in SCREAMING_SNAKE_CASE without the extension, e.g. `__KASIN_STR_H__` for `str.h`. The `KASIN_` prefix marks the guard as belonging to this project. Close the guard with `#endif` followed by a `//` comment that contains the name of the guard.

## Strings

- The length-based string is `Str` from `str.h`, built from a literal with `STR("text")`. Slice, trim, split, compare and parse it with the `str_` functions.
- Convert at a C string boundary with `str_from_cstr` on the way in and `str_to_cstr` or `str_copy_truncated` on the way out.
- The string builder is `Str_Builder`.
- C strings are acceptable where SQLite, the C library or the operating system require them (paths for `open`, text formatted with `snprintf`).

## Types

- `byte` is defined in `byte.h`. SQLite and libc types are exempt from the naming rules.

## Build and tooling

- The build is CMake, in C11. The server needs `_GNU_SOURCE` for `memmem` and `accept4`; this is defined as a feature test macro, and GNU compiler extensions stay off.
- Keep tooling small and consistent. Prefer a shell script or CMake target over adding a new language or tool.
