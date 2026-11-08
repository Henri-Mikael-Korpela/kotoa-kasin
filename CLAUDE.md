# C Programming Style

See [C Programming Style Guide](./docs/c-programming-style-guide.md) for how to use the C programming language in this project.

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