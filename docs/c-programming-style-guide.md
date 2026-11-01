# C Programming Style Guide

## Purpose

This document describes how to use the C programming language and associated tooling in this project. This document is useful for not only developers, but for AI agents as well to keep code more consistent.

Do not run any formatter that works against these rules.

## Indentation, lines and spacing

- Use 4 spaces for indentation, never tabs.
- Keep lines to at most 100 columns, with no exceptions for long strings other than URLs.
- Always put a space after keywords (`if (`, `for (`, `while (`), but not after function names (`assert(x)`).

## File layout

- Use the following order:
    - includes
        - standard library
        - third-party
        - project specific
    - macros and constants
    - types
    - function definitions.

## Comments and documentation

- Use `/**` to start a documentation comment. Example:

```c
/**
 * This is an example documentation comment.
 * It is split on multiple lines.
 */
```

- Use `//` for line comments. Comments are complete sentences, with a capital letter, a space after the slashes, and a full stop.
- Always say why. The code shows how; the comment explains the reason, the trade-off, or the thing that is not obvious.
- Explain the "why" for every non-obvious choice, including the choice of a bound, a magic number, or an unusual order of operations.
- Keep documentation, comments and code in sync, and update comments when the code changes.

## Naming

- Use `snake_case` for functions, variables, parameters and file names
- Use Ada case for types (`Header_Flags`, `Task_Completion`: each word capitalized, words joined by underscores; a one-word type is just capitalized, like `Event`).
- Use `SCREAMING_SNAKE_CASE` for macros and constants. The rule is for types we define, not for SQLite or libc types.
- Use proper capitalization for acronyms (`HTTP_Response`, not `Http_Response`) where they appear in type names.
- Add units to variable names, and put them last: `width_pixels`, `cell_height_pixels`.
- Use nouns for types and variables, and verbs for functions. Name a function for what it does; callbacks and predicates read as questions (`is_leap_year`).
- Distinguish between an index (0-based), a count (1-based, the number of things) and a size (count times unit): `index`, `count`, `size_bytes`. Use `index < count` and `index <= count_max`.
- Struct and type names are in the order of the most significant part first.
- Do not overload names with different meanings. Do not use the name `data`, `info`, `value` or `tmp` when a precise name exists.

## Strings

- Prefer length-based strings, `Str` from `str.h`, over C strings (`char *`, `const char *`): pass text as a `Str`, return it as a `Str`, and slice, trim, split, compare and parse it with the `str_` functions, which cannot run past the end because they never look for a terminator. A `Str` does not own its bytes; it is a view into a literal (`STR("text")`), a buffer or a message.
- Use a C string only where something outside the project requires one: SQLite, the C library and the operating system (paths for `open`, text that is formatted with `snprintf`). Convert at that boundary, once, with `str_from_cstr` on the way in and `str_to_cstr` or `str_copy_truncated` on the way out, and do not let the C string travel further into the program than the call that needs it.
- Build text in a `Str_Builder` over a buffer the caller owns, rather than with `strcat` or by hand with `strlen` arithmetic.

## Variables

- Declare variables at the smallest possible scope, and as close as possible to where they are used, to reduce the chance of use before initialization.

## Constants

- Use `static const` or `enum` for constants rather than `#define` where possible. Use `#define` only for macros that must be macros. Give every constant a unit or comment.
- Place `const` after type name (e.g. `int const`).

## Scopes

- Function statements should have their beginning curly brace on the same line as the function signature.
- If statements should have their beginning curly brace on the same line as the condition.
- Loops (`for`, `while`, `do`) and `switch` follow the same curly brace placement.

## Data types

- Use `bool` for booleans, never `int`.

## Conditional statements

- If statements must have their bodies in curly brackets. No single line if statement body without curly brackets.
- State invariants positively. For `else` branches, make sure the case is handled or deliberately asserted unreachable.
- When matching against an enumeration in a `switch` statement, sort enumeration keys alphabetically unless performance matters. If it's likely one key should come before another key, comment this and explain performance-related reason for this.

## Control flow

- Use only simple, explicit control flow. Do not use `goto` (other than a single cleanup label where unavoidable), `setjmp`/`longjmp`, or recursion. All executions that should be bounded must be bounded.
- Put a fixed upper bound on every loop and every queue or buffer, so that a runaway loop cannot occur. A loop that must run forever, such as the main loop, should be the only exception and should say so.

## Functions

- Keep functions to at most 70 lines. If a function grows longer, split it; centralize control flow in the parent function and move pure logic into helpers.
- Good function shape is a few parameters and a simple return type. Prefer passing structs by `const` pointer when large, and by value when small.
- Mark everything that can be `static` as `static`. Mark pointer parameters `const` when the function does not modify the data.
- Keep control flow (`if`, `switch`, loops) in the parent function and pure computation (no I/O, no side effects) in helpers. Keep leaf functions pure.
- Do not duplicate or alias variables or state, since duplication creates opportunities for them to drift out of sync.
- Avoid out parameters where a return value will do; if you must use them, assert on them.

## Structs

- Put a declaration of each struct field on its own line.

## Assertions

- Use `assert` (from `<assert.h>`) to check function arguments, return values, pre- and post-conditions, and invariants.
- Assert the positive space (what you expect) and the negative space (what you do not expect). The boundary between valid and invalid data is where bugs live.
- Assertions detect programmer errors and must crash. Never use them for expected, recoverable conditions such as I/O failure; handle those by other means.
- Split compound assertions: `assert(a); assert(b);` rather than `assert(a && b);`.
- Use `static_assert` (C11 `_Static_assert`) to check compile-time constants, sizes and relationships between them.
- Assertions must not have side effects, because they may be compiled out with `NDEBUG`.

## Error handling

- All errors must be handled. Check every return value that can fail; do not ignore results. Do not leave `(void)` casts on fallible calls without a comment explaining why.
- Compile with all warnings on (`-Wall -Wextra`) and treat warnings as errors in our own code. Fix the cause rather than suppressing the warning.

## Type casting

- Do not use implicit conversions that lose information. Be explicit with casts, and keep them rare.

## Preprocessor macros

- Avoid complex macros. If a macro has side effects or evaluates its argument more than once, it must be named in capitals and commented.

## Undefined behavior

- Do not rely on undefined behavior. Be careful with signed overflow, shifts, strict aliasing, uninitialized reads and pointer arithmetic. Use unsigned types for wraparound arithmetic.

# Other project management

## Version control

- Write descriptive commit messages that explain the reason for the change, not only what changed.

## Dependencies and tooling

- Avoid global dependencies, keep them to a minimum.
- Keep the build in CMake, and keep tooling small and consistent. Prefer a shell script or CMake target over adding a new language or tool.
