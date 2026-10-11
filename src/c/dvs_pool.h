#ifndef DVS_POOL_H
#define DVS_POOL_H

#include <stddef.h>
#include <stdint.h>

/**
 * EXPR_NULL — sentinel value returned by dvs_pool_alloc on overflow.
 *
 * Chosen as the maximum uint32_t value so it can never be a valid
 * byte offset into any realistically-sized pool.
 */
#define EXPR_NULL  ((uint32_t)0xFFFFFFFFu)

/**
 * dvs_pool_t — in-place bump allocator backed by a caller-supplied buffer.
 *
 * The struct and all allocations live in the same contiguous buffer:
 *
 *   [ dvs_pool_t header | ... free bytes ... ]
 *   ^buf                ^buf + sizeof(dvs_pool_t)
 *
 * All allocations are represented as 32-bit byte offsets from the start
 * of the buffer.  This makes them compact and relocatable (the buffer can
 * be copied as a whole), and EXPR_NULL (0xFFFFFFFF) is a safe "null"
 * sentinel because no pool of that size will ever be created.
 *
 * Capacity is fixed at creation time; there is no reallocation.  On
 * overflow dvs_pool_alloc() returns EXPR_NULL and sets a sticky error
 * flag — all subsequent allocations also return EXPR_NULL until the pool
 * is reset.
 */
typedef struct {
    uint32_t capacity;   /* total usable bytes (buf_size - header)  */
    uint32_t used;       /* bytes allocated so far                  */
    uint32_t overflow;   /* non-zero if any allocation has overflowed */
    uint32_t _pad;       /* keep header 16-byte aligned             */
    /* Allocation data follows immediately in the same buffer.      */
} dvs_pool_t;

/**
 * Initialise a pool in-place inside a caller-supplied buffer.
 *
 * @param buf       Buffer of at least `buf_size` bytes.  Must be
 *                  suitably aligned for any type (e.g. from malloc).
 * @param buf_size  Total size of `buf` in bytes.
 * @return  Pointer to the initialised pool (== buf), or NULL if
 *          `buf_size` is too small to hold even the pool header.
 */
dvs_pool_t *dvs_pool_init(void *buf, size_t buf_size);

/**
 * Bump-allocate `bytes` bytes with `align`-byte alignment.
 *
 * @param pool   Pool created with dvs_pool_init().
 * @param bytes  Number of bytes requested.
 * @param align  Required alignment (must be a power of two; 0 or 1 → 1).
 * @return  32-bit offset from the start of the backing buffer, or
 *          EXPR_NULL on overflow.  The sticky overflow flag is set.
 */
uint32_t dvs_pool_alloc(dvs_pool_t *pool, uint32_t bytes, uint32_t align);

/**
 * Convert an offset returned by dvs_pool_alloc() to a real pointer.
 *
 * Inline: it sits on every hot path (each Variable and propagator lookup).
 * The out-of-line `(dvs_pool_ptr)` stays exported for FFI callers.
 *
 * @param pool    The pool.
 * @param offset  Offset returned by dvs_pool_alloc().
 * @return  Pointer to the allocated region, or NULL if offset == EXPR_NULL.
 */
static inline void *dvs_pool_ptr_inline(const dvs_pool_t *pool, uint32_t offset) {
    if (offset == EXPR_NULL)
        return NULL;
    return (void *)((char *)pool + offset);
}
void *(dvs_pool_ptr)(const dvs_pool_t *pool, uint32_t offset);
#define dvs_pool_ptr(pool, offset)  dvs_pool_ptr_inline((pool), (offset))

/**
 * A point in the pool's allocation order, for dvs_pool_release(). Offsets
 * allocated after the mark compare >= dvs_pool_mark_offset(mark).
 */
typedef struct {
    uint32_t used;
    uint32_t overflow;
} dvs_pool_mark_t;

static inline dvs_pool_mark_t dvs_pool_mark(const dvs_pool_t *pool) {
    dvs_pool_mark_t m = { pool->used, pool->overflow };
    return m;
}

/** The first offset dvs_pool_alloc() can return after `mark` was taken. */
static inline uint32_t dvs_pool_mark_offset(dvs_pool_mark_t mark) {
    return (uint32_t)sizeof(dvs_pool_t) + mark.used;
}

/**
 * Free everything allocated since `mark` and restore the overflow flag to
 * its value then. Offsets at or above dvs_pool_mark_offset(mark) become
 * invalid; the caller must drop every reference to them first. A mark
 * at or above the current position is a no-op.
 *
 * Built with DVS_DEBUG_ALWAYS_MOVE the freed bytes are filled with 0xA5;
 * under AddressSanitizer they are poisoned until allocated again.
 */
void dvs_pool_release(dvs_pool_t *pool, dvs_pool_mark_t mark);

/**
 * Reset the pool: clear all allocations and the overflow flag.
 * The backing buffer is not zeroed; all previous offsets become invalid.
 */
void dvs_pool_reset(dvs_pool_t *pool);

/**
 * Return the number of bytes currently allocated (excluding the header).
 */
uint32_t dvs_pool_used(const dvs_pool_t *pool);

#endif /* DVS_POOL_H */
