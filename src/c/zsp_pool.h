#ifndef ZSP_POOL_H
#define ZSP_POOL_H

#include <stddef.h>
#include <stdint.h>

/**
 * EXPR_NULL — sentinel value returned by zsp_pool_alloc on overflow.
 *
 * Chosen as the maximum uint32_t value so it can never be a valid
 * byte offset into any realistically-sized pool.
 */
#define EXPR_NULL  ((uint32_t)0xFFFFFFFFu)

/**
 * zsp_pool_t — in-place bump allocator backed by a caller-supplied buffer.
 *
 * The struct and all allocations live in the same contiguous buffer:
 *
 *   [ zsp_pool_t header | ... free bytes ... ]
 *   ^buf                ^buf + sizeof(zsp_pool_t)
 *
 * All allocations are represented as 32-bit byte offsets from the start
 * of the buffer.  This makes them compact and relocatable (the buffer can
 * be copied as a whole), and EXPR_NULL (0xFFFFFFFF) is a safe "null"
 * sentinel because no pool of that size will ever be created.
 *
 * Capacity is fixed at creation time; there is no reallocation.  On
 * overflow zsp_pool_alloc() returns EXPR_NULL and sets a sticky error
 * flag — all subsequent allocations also return EXPR_NULL until the pool
 * is reset.
 */
typedef struct {
    uint32_t capacity;   /* total usable bytes (buf_size - header)  */
    uint32_t used;       /* bytes allocated so far                  */
    uint32_t overflow;   /* non-zero if any allocation has overflowed */
    uint32_t _pad;       /* keep header 16-byte aligned             */
    /* Allocation data follows immediately in the same buffer.      */
} zsp_pool_t;

/**
 * Initialise a pool in-place inside a caller-supplied buffer.
 *
 * @param buf       Buffer of at least `buf_size` bytes.  Must be
 *                  suitably aligned for any type (e.g. from malloc).
 * @param buf_size  Total size of `buf` in bytes.
 * @return  Pointer to the initialised pool (== buf), or NULL if
 *          `buf_size` is too small to hold even the pool header.
 */
zsp_pool_t *zsp_pool_init(void *buf, size_t buf_size);

/**
 * Bump-allocate `bytes` bytes with `align`-byte alignment.
 *
 * @param pool   Pool created with zsp_pool_init().
 * @param bytes  Number of bytes requested.
 * @param align  Required alignment (must be a power of two; 0 or 1 → 1).
 * @return  32-bit offset from the start of the backing buffer, or
 *          EXPR_NULL on overflow.  The sticky overflow flag is set.
 */
uint32_t zsp_pool_alloc(zsp_pool_t *pool, uint32_t bytes, uint32_t align);

/**
 * Convert an offset returned by zsp_pool_alloc() to a real pointer.
 *
 * @param pool    The pool.
 * @param offset  Offset returned by zsp_pool_alloc().
 * @return  Pointer to the allocated region, or NULL if offset == EXPR_NULL.
 */
void *zsp_pool_ptr(const zsp_pool_t *pool, uint32_t offset);

/**
 * Reset the pool: clear all allocations and the overflow flag.
 * The backing buffer is not zeroed; all previous offsets become invalid.
 */
void zsp_pool_reset(zsp_pool_t *pool);

/**
 * Return the number of bytes currently allocated (excluding the header).
 */
uint32_t zsp_pool_used(const zsp_pool_t *pool);

#endif /* ZSP_POOL_H */
