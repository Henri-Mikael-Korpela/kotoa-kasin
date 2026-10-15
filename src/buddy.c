#include "buddy.h"

#include <assert.h>
#include <string.h>

/*
 * Every block, free or allocated, starts with this header, so the buddy of
 * any block can always be inspected. The links are only used while free.
 */
struct Buddy_Header {
    uint32_t      order;
    bool          is_free;
    Buddy_Header *next;
    Buddy_Header *prev;
    uint64_t      padding;  // Keeps payloads 16-byte aligned.
};

static_assert(sizeof (Buddy_Header) == 32, "header must keep payload aligned");
static_assert(BUDDY_MIN_BLOCK > sizeof (Buddy_Header), "minimum block too small");

static size_t block_size(uint32_t order) {
    return (size_t)BUDDY_MIN_BLOCK << order;
}

static bool order_for(Buddy_Heap const *heap, size_t size, uint32_t *order) {
    assert(order != NULL);
    if (size > heap->size - sizeof (Buddy_Header)) {
        return false;
    }
    size_t need = size + sizeof (Buddy_Header);
    uint32_t k = 0;
    while (block_size(k) < need) {
        k++;
    }
    *order = k;
    return true;
}

static void list_push(Buddy_Heap *heap, Buddy_Header *block) {
    Buddy_Header *head = heap->free_lists[block->order];
    block->is_free = true;
    block->prev = NULL;
    block->next = head;
    if (head) {
        head->prev = block;
    }
    heap->free_lists[block->order] = block;
}

static void list_remove(Buddy_Heap *heap, Buddy_Header *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        heap->free_lists[block->order] = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
    block->is_free = false;
}

bool buddy_init(Buddy_Heap *heap, void *base, size_t size) {
    assert(heap != NULL);
    assert(base != NULL);
    uint32_t max_order = 0;
    while (max_order + 1 < BUDDY_MAX_ORDERS && block_size(max_order) < size) {
        max_order++;
    }
    if (block_size(max_order) != size || ((uintptr_t)base & 15) != 0) {
        return false;
    }
    heap->base = base;
    heap->size = size;
    heap->max_order = max_order;
    heap->bytes_in_use = 0;
    for (uint32_t i = 0; i < BUDDY_MAX_ORDERS; i++) {
        heap->free_lists[i] = NULL;
    }
    if (pthread_mutex_init(&heap->lock, NULL) != 0) {
        return false;
    }
    Buddy_Header *root = base;
    root->order = max_order;
    list_push(heap, root);
    return true;
}

void *buddy_alloc(Buddy_Heap *heap, size_t size) {
    uint32_t order;
    if (!order_for(heap, size, &order)) {
        return NULL;
    }

    pthread_mutex_lock(&heap->lock);
    uint32_t k = order;
    while (k <= heap->max_order && !heap->free_lists[k]) {
        k++;
    }
    if (k > heap->max_order) {
        pthread_mutex_unlock(&heap->lock);
        return NULL;
    }

    Buddy_Header *block = heap->free_lists[k];
    list_remove(heap, block);
    while (k > order) {  // Split, keeping the lower half and freeing the upper.
        k--;
        Buddy_Header *upper = (Buddy_Header *)((uint8_t *)block + block_size(k));
        upper->order = k;
        list_push(heap, upper);
    }
    block->order = order;
    heap->bytes_in_use += block_size(order);
    pthread_mutex_unlock(&heap->lock);
    return block + 1;
}

void buddy_free(Buddy_Heap *heap, void *ptr) {
    if (!ptr) {
        return;
    }
    Buddy_Header *block = (Buddy_Header *)ptr - 1;

    pthread_mutex_lock(&heap->lock);
    heap->bytes_in_use -= block_size(block->order);
    uint32_t order = block->order;
    while (order < heap->max_order) {
        size_t offset = (size_t)((uint8_t *)block - heap->base);
        Buddy_Header *buddy = (Buddy_Header *)(heap->base + (offset ^ block_size(order)));
        if (!buddy->is_free || buddy->order != order) {
            break;
        }
        list_remove(heap, buddy);
        if (buddy < block) {
            block = buddy;
        }
        order++;
    }
    block->order = order;
    list_push(heap, block);
    pthread_mutex_unlock(&heap->lock);
}

void *buddy_realloc(Buddy_Heap *heap, void *ptr, size_t size) {
    if (!ptr) {
        return buddy_alloc(heap, size);
    }
    size_t usable = buddy_usable_size(ptr);
    if (size <= usable) {
        return ptr;
    }
    void *moved = buddy_alloc(heap, size);
    if (!moved) {
        return NULL;
    }
    memcpy(moved, ptr, usable);
    buddy_free(heap, ptr);
    return moved;
}

size_t buddy_usable_size(void const *ptr) {
    Buddy_Header const *block = (Buddy_Header const *)ptr - 1;
    return block_size(block->order) - sizeof (Buddy_Header);
}

size_t buddy_round_up(Buddy_Heap const *heap, size_t size) {
    uint32_t order;
    if (!order_for(heap, size, &order)) {
        return 0;
    }
    return block_size(order) - sizeof (Buddy_Header);
}
