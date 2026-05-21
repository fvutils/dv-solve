#include <stdlib.h>
#include <string.h>
#include "zsp_stack.h"

/* ------------------------------------------------------------------ */
/* Block header stored at the very start of each block                 */
/* ------------------------------------------------------------------ */

typedef struct _block_hdr {
    struct _block_hdr *prev;   /* previous block in the chain (older) */
} _block_hdr_t;

#define BLOCK_HDR_SZ  ((size_t)sizeof(_block_hdr_t))

/* ------------------------------------------------------------------ */
/* zsp_stack_t internals                                               */
/* ------------------------------------------------------------------ */

struct zsp_stack_s {
    zsp_block_alloc_t *block_alloc; /* block source                  */
    _block_hdr_t      *cur_block;   /* current (newest) block, or NULL */
    size_t             intra;       /* bytes used in cur_block        */
    size_t             block_count; /* total blocks currently held    */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static size_t _block_capacity(const zsp_stack_t *s) {
    return zsp_block_alloc_block_size(s->block_alloc) - BLOCK_HDR_SZ;
}

static _block_hdr_t *_acquire_block(zsp_stack_t *s) {
    _block_hdr_t *blk = (_block_hdr_t *)zsp_block_alloc_get(s->block_alloc);
    if (!blk) return NULL;
    blk->prev = s->cur_block;
    s->cur_block = blk;
    s->intra = 0;
    s->block_count++;
    return blk;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

zsp_stack_t *zsp_stack_create(zsp_block_alloc_t *block_alloc) {
    /* Allocate the stack struct via the block allocator's backing alloc.
       We ask for exactly one block and use only the header portion for
       the struct; alternatively, allocate with system malloc.
       Simpler: just use malloc directly for the tiny stack control struct. */
    zsp_stack_t *s = (zsp_stack_t *)malloc(sizeof(zsp_stack_t));
    if (!s) return NULL;
    s->block_alloc = block_alloc;
    s->cur_block   = NULL;
    s->intra       = 0;
    s->block_count = 0;
    return s;
}

void zsp_stack_destroy(zsp_stack_t *stack) {
    /* Return all held blocks */
    _block_hdr_t *blk = stack->cur_block;
    while (blk) {
        _block_hdr_t *prev = blk->prev;
        zsp_block_alloc_put(stack->block_alloc, blk);
        blk = prev;
    }
    free(stack);
}

void *zsp_stack_alloc(zsp_stack_t *stack, size_t bytes, size_t align) {
    if (align < 1) align = 1;

    /* Try to satisfy from the current block */
    if (stack->cur_block) {
        size_t cap   = _block_capacity(stack);
        size_t base  = stack->intra;

        /* Align up */
        if (align > 1) {
            size_t mask = align - 1;
            base = (base + mask) & ~mask;
        }

        if (base + bytes <= cap) {
            stack->intra = base + bytes;
            return (char *)stack->cur_block + BLOCK_HDR_SZ + base;
        }
    }

    /* Need a new block — must fit within one block */
    size_t needed = bytes + (align - 1);  /* worst-case padding */
    if (needed + BLOCK_HDR_SZ > zsp_block_alloc_block_size(stack->block_alloc))
        return NULL;  /* request too large for a single block */

    _block_hdr_t *blk = _acquire_block(stack);
    if (!blk) return NULL;

    /* Re-try with fresh block */
    size_t base = 0;
    if (align > 1) {
        size_t addr = (size_t)((char *)blk + BLOCK_HDR_SZ);
        size_t mask = align - 1;
        size_t off  = (align - (addr & mask)) & mask;
        base = off;
    }
    stack->intra = base + bytes;
    return (char *)blk + BLOCK_HDR_SZ + base;
}

zsp_stack_mark_t zsp_stack_push(zsp_stack_t *stack) {
    zsp_stack_mark_t mark;
    mark.block = stack->cur_block;
    mark.intra = (uint32_t)stack->intra;
    return mark;
}

void zsp_stack_pop(zsp_stack_t *stack, zsp_stack_mark_t mark) {
    /* Return all blocks that were allocated after the push */
    while (stack->cur_block && stack->cur_block != mark.block) {
        _block_hdr_t *blk = stack->cur_block;
        stack->cur_block = blk->prev;
        stack->block_count--;
        zsp_block_alloc_put(stack->block_alloc, blk);
    }
    /* Restore the intra-block pointer */
    stack->intra = (size_t)mark.intra;
}

size_t zsp_stack_block_count(const zsp_stack_t *stack) {
    return stack->block_count;
}
