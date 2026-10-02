#include <stdlib.h>
#include <stddef.h>
#include "dvs_block_alloc.h"

/* ------------------------------------------------------------------ */
/* Default malloc-backed dvs_alloc_t                                   */
/* ------------------------------------------------------------------ */

static void *_malloc_alloc(dvs_alloc_t *self, size_t size) {
    (void)self;
    return malloc(size);
}

static void _malloc_release(dvs_alloc_t *self, void *ptr, size_t size) {
    (void)self;
    (void)size;
    free(ptr);
}

dvs_alloc_t dvs_malloc_alloc = {
    .alloc   = _malloc_alloc,
    .release = _malloc_release,
};

/* ------------------------------------------------------------------ */
/* Free-list node (stored inside the block while it is cached)         */
/* ------------------------------------------------------------------ */

typedef struct _free_node {
    struct _free_node *next;
} _free_node_t;

/* Out-of-band record of every block ever allocated, so destroy() can free
 * blocks that are still in use (not on the free list). Stored separately from
 * the block memory itself so it never clobbers the block's usable bytes. */
typedef struct _blk_rec {
    struct _blk_rec *next;
    void            *block;
} _blk_rec_t;

/* ------------------------------------------------------------------ */
/* dvs_block_alloc_t internals                                         */
/* ------------------------------------------------------------------ */

struct dvs_block_alloc_s {
    dvs_alloc_t  *alloc;        /* backing allocator                  */
    size_t        block_size;   /* size of every vended block         */
    _free_node_t *free_list;    /* singly-linked list of cached blocks */
    size_t        free_count;   /* number of blocks currently cached  */
    size_t        max_cached;   /* 0 = unlimited                       */
    _blk_rec_t   *all_list;     /* every live block (cached or in-use) */
};

/* Drop the record for `block` from all_list (used when a block is released
 * back to the system rather than cached). O(n) in live blocks; only the
 * capacity-release / trim paths hit it, which are rare. */
static void _all_list_remove(dvs_block_alloc_t *ba, void *block) {
    _blk_rec_t **pp = &ba->all_list;
    while (*pp) {
        _blk_rec_t *rec = *pp;
        if (rec->block == block) {
            *pp = rec->next;
            DVS_RELEASE(ba->alloc, rec, sizeof(_blk_rec_t));
            return;
        }
        pp = &rec->next;
    }
}

dvs_block_alloc_t *dvs_block_alloc_create(dvs_alloc_t *alloc, size_t block_size) {
    if (!alloc) alloc = &dvs_malloc_alloc;

    /* Use a reasonable default block size if 0 is given.
     * Block must be large enough for the header + useful data. */
    if (block_size == 0)
        block_size = 4096;
    if (block_size < sizeof(_free_node_t))
        block_size = sizeof(_free_node_t);

    dvs_block_alloc_t *ba = DVS_ALLOC(alloc, sizeof(dvs_block_alloc_t));
    if (!ba) return NULL;

    ba->alloc      = alloc;
    ba->block_size = block_size;
    ba->free_list  = NULL;
    ba->free_count = 0;
    ba->max_cached = 0;
    ba->all_list   = NULL;
    return ba;
}

void *dvs_block_alloc_get(dvs_block_alloc_t *ba) {
    if (ba->free_list) {
        /* Pop from free list — no system allocation needed */
        _free_node_t *node = ba->free_list;
        ba->free_list = node->next;
        ba->free_count--;
        return (void *)node;
    }
    /* Fresh allocation from backing allocator. Track it so destroy() can free
     * it even if it is never returned to the free list. */
    void *block = DVS_ALLOC(ba->alloc, ba->block_size);
    if (!block) return NULL;
    _blk_rec_t *rec = DVS_ALLOC(ba->alloc, sizeof(_blk_rec_t));
    if (!rec) {
        DVS_RELEASE(ba->alloc, block, ba->block_size);
        return NULL;
    }
    rec->block    = block;
    rec->next     = ba->all_list;
    ba->all_list  = rec;
    return block;
}

void dvs_block_alloc_put(dvs_block_alloc_t *ba, void *block) {
    if (ba->max_cached && ba->free_count >= ba->max_cached) {
        /* At capacity — release directly instead of caching. */
        _all_list_remove(ba, block);
        DVS_RELEASE(ba->alloc, block, ba->block_size);
        return;
    }
    _free_node_t *node = (_free_node_t *)block;
    node->next    = ba->free_list;
    ba->free_list = node;
    ba->free_count++;
}

void dvs_block_alloc_set_max_cached(dvs_block_alloc_t *ba, size_t max_cached) {
    ba->max_cached = max_cached;
}

void dvs_block_alloc_trim(dvs_block_alloc_t *ba, size_t target) {
    while (ba->free_count > target && ba->free_list) {
        _free_node_t *node = ba->free_list;
        ba->free_list = node->next;
        ba->free_count--;
        _all_list_remove(ba, (void *)node);
        DVS_RELEASE(ba->alloc, node, ba->block_size);
    }
}

size_t dvs_block_alloc_cached_count(const dvs_block_alloc_t *ba) {
    return ba->free_count;
}

void dvs_block_alloc_destroy(dvs_block_alloc_t *ba) {
    /* Release every block ever allocated — whether cached on the free list or
     * still in use by the caller — plus its tracking record. This frees memory
     * the old free-list-only teardown leaked (in-use blocks were never freed).
     */
    _blk_rec_t *rec = ba->all_list;
    while (rec) {
        _blk_rec_t *next = rec->next;
        DVS_RELEASE(ba->alloc, rec->block, ba->block_size);
        DVS_RELEASE(ba->alloc, rec, sizeof(_blk_rec_t));
        rec = next;
    }
    ba->all_list   = NULL;
    ba->free_list  = NULL;
    ba->free_count = 0;

    /* Release the allocator struct itself */
    DVS_RELEASE(ba->alloc, ba, sizeof(dvs_block_alloc_t));
}

size_t dvs_block_alloc_block_size(const dvs_block_alloc_t *ba) {
    return ba->block_size;
}
