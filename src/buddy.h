/**
 * @file buddy.h
 * @brief Thread-safe buddy allocator over a fixed, caller-provided memory block.
 *
 * Used as the general-purpose heap for code that insists on malloc-style
 * allocation (SQLite). Blocks are powers of two; freeing merges a block with
 * its buddy whenever both halves are free, so the heap does not fragment
 * permanently. The heap never grows beyond the block it was given.
 */

#ifndef __KASIN_BUDDY_H__
#define __KASIN_BUDDY_H__

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    /** Smallest block, including its header, in bytes. */
    BUDDY_MIN_BLOCK = 64,
    /** Upper bound on block orders: heaps up to BUDDY_MIN_BLOCK << 31 bytes. */
    BUDDY_MAX_ORDERS = 32,
};

typedef struct Buddy_Header Buddy_Header;

typedef struct {
    uint8_t         *base;
    size_t           size;
    uint32_t         max_order;
    Buddy_Header    *free_lists[BUDDY_MAX_ORDERS];
    size_t           bytes_in_use;  /**< Sum of allocated block sizes. */
    pthread_mutex_t  lock;
} Buddy_Heap;

/**
 * Turns a memory block into an empty heap.
 *
 * @param heap Heap to initialize.
 * @param base Start of the block, aligned to at least 16 bytes.
 * @param size Size of the block; must be BUDDY_MIN_BLOCK times a power of two.
 * @return False when size is not usable.
 */
bool buddy_init(Buddy_Heap *heap, void *base, size_t size);

/**
 * Allocates at least size bytes, aligned to 16 bytes.
 *
 * @return The allocation, or NULL when no large enough block is free.
 */
void *buddy_alloc(Buddy_Heap *heap, size_t size);

/**
 * Returns an allocation to the heap. NULL is ignored.
 */
void buddy_free(Buddy_Heap *heap, void *ptr);

/**
 * Resizes an allocation, moving it only when its block is too small.
 *
 * @return The resized allocation, or NULL (leaving ptr intact) when out of memory.
 */
void *buddy_realloc(Buddy_Heap *heap, void *ptr, size_t size);

/**
 * Returns how many bytes an allocation can actually hold.
 */
size_t buddy_usable_size(void const *ptr);

/**
 * Returns how many usable bytes an allocation of size bytes would get.
 *
 * @return The rounded size, or 0 when size can never be allocated.
 */
size_t buddy_round_up(Buddy_Heap const *heap, size_t size);

#endif  // __KASIN_BUDDY_H__
