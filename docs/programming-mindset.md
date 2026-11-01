# Programming mindset

- Think before coding. Design for the simplest solution that solves the real problem: the "super idea" that fixes safety, performance and readability at once.
- Do not add features, options or abstractions that were not asked for. Do it right the first time; avoid technical debt.
- Zero dependencies beyond the C standard library and SQLite. Do not add others without a strong reason.
- Always motivate, always say why. Never leave a decision without a reason in a comment.

## Performance

- Think about performance from the start, in the design phase, because it is hardest to fix later. Do back-of-the-envelope sketches for the four resources (network, disk, memory, CPU) and their two characteristics (bandwidth, latency).
- Optimize for the slowest resource first, in the order network, disk, memory, CPU, adjusted for frequency of use.
- Be explicit. Do not depend on the compiler to optimize something that matters; extract hot loops into standalone functions with primitive arguments and no `self` pointer, so that the compiler (and a reader) can see that nothing is aliased.
- Batch work. Amortize costs by processing many items at once rather than one at a time.
- Avoid dynamic allocation after initialization. Allocate memory statically or once at startup, with fixed upper bounds, and never call `malloc` or `free` in the main loop.
  - This eliminates use-after-free and avoids fragmentation.
- Prefer custom memory allocators instead of general-purpose allocation.
- Instead of opening and closing files, prefer memory mapping whenever it is more memory efficient and faster to execute.