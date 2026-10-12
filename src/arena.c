#include "arena.h"

size_t arena_align_up(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

void arena_init(Arena *arena, void *base, size_t capacity) {
    arena->base = base;
    arena->capacity = capacity;
    arena->used = 0;
}

void *arena_alloc(Arena *arena, size_t size, size_t align) {
    size_t p = arena_align_up(arena->used, align);
    if (p > arena->capacity || size > arena->capacity - p) {
        return NULL;
    }
    arena->used = p + size;
    return arena->base + p;
}
