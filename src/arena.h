/**
 * @file arena.h
 * @brief Linear (bump) arena allocator over a caller-provided memory block.
 *
 * The arena never allocates or frees memory itself. Allocations are carved
 * out of the block in order and are only released all at once by discarding
 * the arena.
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *base;
    size_t   capacity;
    size_t   used;
} Arena;

/**
 * Rounds a value up to the next multiple of a power-of-two alignment.
 *
 * @param value Value to round.
 * @param align Alignment, must be a power of two.
 * @return Smallest multiple of align that is >= value.
 */
size_t arena_align_up(size_t value, size_t align);

/**
 * Initializes an arena over an existing memory block.
 *
 * @param arena Arena to initialize.
 * @param base Start of the memory block.
 * @param capacity Size of the memory block in bytes.
 */
void arena_init(Arena *arena, void *base, size_t capacity);

/**
 * Carves an aligned allocation out of the arena.
 *
 * @param arena Arena to allocate from.
 * @param size Number of bytes to allocate.
 * @param align Alignment, must be a power of two.
 * @return Pointer to the allocation, or NULL if the arena is exhausted.
 */
void *arena_alloc(Arena *arena, size_t size, size_t align);

#endif
