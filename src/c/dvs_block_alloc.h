#ifndef DVS_BLOCK_ALLOC_H
#define DVS_BLOCK_ALLOC_H

#include <stddef.h>
#include "dvs_alloc.h"

/**
 * dvs_block_alloc_t — block-level allocator with a free-list cache.
 *
 * Manages a pool of equal-sized blocks obtained from a backing dvs_alloc_t.
 * Released blocks are kept in an internal free list so that subsequent
 * dvs_block_alloc_get() calls can reuse them without hitting the system
 * allocator.
 *
 * Both dvs_pool_t and dvs_stack_t accept a dvs_block_alloc_t * so that
 * neither calls malloc directly.
 */
typedef struct dvs_block_alloc_s dvs_block_alloc_t;

/**
 * Create a new block allocator.
 *
 * @param alloc       Backing allocator (must outlive the block allocator).
 *                    Pass NULL to use dvs_malloc_alloc.
 * @param block_size  Size of every block vended by this allocator.
 * @return  New block allocator, or NULL on allocation failure.
 */
dvs_block_alloc_t *dvs_block_alloc_create(dvs_alloc_t *alloc, size_t block_size);

/**
 * Obtain a block of `block_size` bytes.
 *
 * Returns a cached block if one is available, otherwise allocates a fresh
 * one from the backing allocator.
 *
 * @return  Pointer to a block, or NULL on allocation failure.
 */
void *dvs_block_alloc_get(dvs_block_alloc_t *ba);

/**
 * Return a block to the free-list cache.
 *
 * The caller must not use the block after this call.
 */
void dvs_block_alloc_put(dvs_block_alloc_t *ba, void *block);

/**
 * Destroy the block allocator and release all cached blocks plus the
 * allocator itself back to the backing allocator.
 */
void dvs_block_alloc_destroy(dvs_block_alloc_t *ba);

/**
 * Return the block size this allocator was created with.
 */
size_t dvs_block_alloc_block_size(const dvs_block_alloc_t *ba);

/**
 * Bound the size of the internal free-list cache.
 *
 * When more than `max_cached` blocks would be retained, dvs_block_alloc_put
 * releases the surplus immediately to the backing allocator instead of
 * caching them. The default (0) means unlimited — the existing behavior
 * prior to this knob being added.
 *
 * Setting a lower limit than the current cache size does not trim
 * immediately; call dvs_block_alloc_trim() if you want to force the
 * cache down right now.
 */
void dvs_block_alloc_set_max_cached(dvs_block_alloc_t *ba, size_t max_cached);

/**
 * Trim the internal free-list cache to at most `target` blocks, releasing
 * the rest back to the backing allocator. No-op if the cache is already
 * at or below `target`.
 */
void dvs_block_alloc_trim(dvs_block_alloc_t *ba, size_t target);

/**
 * Return the current size of the internal free-list cache.
 */
size_t dvs_block_alloc_cached_count(const dvs_block_alloc_t *ba);

#endif /* DVS_BLOCK_ALLOC_H */
