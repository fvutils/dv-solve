#include <stdlib.h>
#include <stddef.h>
#include "zsp_block_alloc.h"

/* ------------------------------------------------------------------ */
/* Default malloc-backed zsp_alloc_t                                   */
/* ------------------------------------------------------------------ */

static void *_malloc_alloc(zsp_alloc_t *self, size_t size) {
    (void)self;
    return malloc(size);
}

static void _malloc_release(zsp_alloc_t *self, void *ptr, size_t size) {
    (void)self;
    (void)size;
    free(ptr);
}

zsp_alloc_t zsp_malloc_alloc = {
    .alloc   = _malloc_alloc,
    .release = _malloc_release,
};

/* ------------------------------------------------------------------ */
/* Free-list node (stored inside the block while it is cached)         */
/* ------------------------------------------------------------------ */

typedef struct _free_node {
    struct _free_node *next;
} _free_node_t;

/* ------------------------------------------------------------------ */
/* zsp_block_alloc_t internals                                         */
/* ------------------------------------------------------------------ */

struct zsp_block_alloc_s {
    zsp_alloc_t  *alloc;        /* backing allocator                  */
    size_t        block_size;   /* size of every vended block         */
    _free_node_t *free_list;    /* singly-linked list of cached blocks */
    size_t        free_count;   /* number of blocks currently cached  */
};

zsp_block_alloc_t *zsp_block_alloc_create(zsp_alloc_t *alloc, size_t block_size) {
    if (!alloc) alloc = &zsp_malloc_alloc;

    /* Use a reasonable default block size if 0 is given.
     * Block must be large enough for the header + useful data. */
    if (block_size == 0)
        block_size = 4096;
    if (block_size < sizeof(_free_node_t))
        block_size = sizeof(_free_node_t);

    zsp_block_alloc_t *ba = ZSP_ALLOC(alloc, sizeof(zsp_block_alloc_t));
    if (!ba) return NULL;

    ba->alloc      = alloc;
    ba->block_size = block_size;
    ba->free_list  = NULL;
    ba->free_count = 0;
    return ba;
}

void *zsp_block_alloc_get(zsp_block_alloc_t *ba) {
    if (ba->free_list) {
        /* Pop from free list — no system allocation needed */
        _free_node_t *node = ba->free_list;
        ba->free_list = node->next;
        ba->free_count--;
        return (void *)node;
    }
    /* Fresh allocation from backing allocator */
    return ZSP_ALLOC(ba->alloc, ba->block_size);
}

void zsp_block_alloc_put(zsp_block_alloc_t *ba, void *block) {
    _free_node_t *node = (_free_node_t *)block;
    node->next    = ba->free_list;
    ba->free_list = node;
    ba->free_count++;
}

void zsp_block_alloc_destroy(zsp_block_alloc_t *ba) {
    /* Release every cached block */
    _free_node_t *node = ba->free_list;
    while (node) {
        _free_node_t *next = node->next;
        ZSP_RELEASE(ba->alloc, node, ba->block_size);
        node = next;
    }
    ba->free_list  = NULL;
    ba->free_count = 0;

    /* Release the allocator struct itself */
    ZSP_RELEASE(ba->alloc, ba, sizeof(zsp_block_alloc_t));
}

size_t zsp_block_alloc_block_size(const zsp_block_alloc_t *ba) {
    return ba->block_size;
}
