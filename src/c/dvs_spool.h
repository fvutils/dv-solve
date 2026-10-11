#ifndef DVS_SPOOL_H
#define DVS_SPOOL_H

#include <stddef.h>
#include <stdint.h>
#include "dvs_alloc.h"
#include "dvs_pool.h"   /* EXPR_NULL */

/**
 * dvs_spool_t — segmented, growable bump allocator with 32-bit offsets.
 *
 * The solver context's pool (docs/dynamic_pool_design.md §4.1). An offset
 * names slot `off >> DVS_SEG_SHIFT` and the byte `off & DVS_SEG_MASK` in
 * it; seg[] maps each slot to memory:
 *
 *     ptr = seg[off >> DVS_SEG_SHIFT] + (off & DVS_SEG_MASK)
 *
 * The memory behind the slots comes in "backings", each contiguous:
 *   - the fixed region: a caller-supplied buffer, mapped onto slots
 *     0..n-1 (embedded mode, and the start of every context);
 *   - one slot from the allocator, for objects up to a slot in size;
 *   - one allocation spanning consecutive slots, for a larger object.
 * An allocation never straddles two backings, so it is contiguous in
 * memory however large it is, and an offset stays valid until it is
 * released: growth adds backings and never moves one.
 *
 * Offsets increase in allocation order, so dvs_spool_mark() and
 * dvs_spool_release() free whatever was allocated after a mark, and an
 * offset >= dvs_spool_mark_offset(mark) was allocated after it.
 *
 * Without an allocator (fixed mode) the pool holds only the caller's
 * buffer: an allocation that does not fit fails with EXPR_NULL and sets
 * the sticky overflow flag, as dvs_pool_t does. With one, the pool grows
 * until the allocator fails or the 32-bit offset space is used up.
 *
 * Offsets 0..15 are never returned: 0 means "none" in several tables.
 */

#ifndef DVS_SEG_SHIFT
#define DVS_SEG_SHIFT 16u
#endif
#define DVS_SEG_SIZE  ((uint32_t)1u << DVS_SEG_SHIFT)
#define DVS_SEG_MASK  (DVS_SEG_SIZE - 1u)

/** Freed one-slot backings kept for reuse (push/pop open and close slots). */
#define DVS_SPOOL_SPARES 4u

typedef struct {
    uint32_t start;     /* offset of its first byte (a slot boundary)   */
    uint32_t size;      /* bytes                                         */
    uint8_t *mem;
} dvs_spool_backing_t;

typedef struct dvs_spool_s {
    uint8_t            **seg;       /* slot -> base address              */
    uint32_t             top;       /* next free offset                  */
    uint32_t             end;       /* end of the backing `top` is in    */
    uint32_t             overflow;  /* sticky: an allocation failed      */
    uint32_t             seg_cap;   /* entries in seg[]                  */
    uint8_t             *fixed;     /* the caller's buffer (fixed region) */
    uint32_t             fixed_end; /* offset end of the fixed region     */
    uint8_t              seg_owned; /* seg[] is from alloc (else carved)  */
    dvs_alloc_t         *alloc;     /* growth; NULL = fixed mode         */
    dvs_spool_backing_t *back;      /* allocator backings, in order      */
    uint32_t             n_back;
    uint32_t             back_cap;
    uint8_t             *spare[DVS_SPOOL_SPARES];
    uint32_t             n_spare;
} dvs_spool_t;

typedef struct {
    uint32_t top;
    uint32_t end;
    uint32_t overflow;
    uint32_t n_back;
} dvs_spool_mark_t;

/**
 * Initialise a pool over `buf` (may be NULL with size 0) that grows through
 * `alloc` (NULL: fixed mode). In fixed mode the slot table is carved from
 * the front of `buf`.
 *
 * @return 0, or -1 if the buffer cannot hold the slot table or the table
 *         cannot be allocated.
 */
int dvs_spool_init(dvs_spool_t *p, void *buf, size_t size, dvs_alloc_t *alloc);

/** Release every backing the pool allocated. The pool is unusable after. */
void dvs_spool_destroy(dvs_spool_t *p);

/**
 * Bump-allocate `bytes` bytes aligned to `align` (a power of two, at most
 * 16; 0 or 1 means 1). Returns the offset, or EXPR_NULL (and sets the
 * overflow flag) when the pool is full.
 */
uint32_t dvs_spool_alloc(dvs_spool_t *p, uint32_t bytes, uint32_t align);

/** The address of offset `off`, or NULL for EXPR_NULL. */
static inline void *dvs_spool_ptr(const dvs_spool_t *p, uint32_t off) {
    if (off == EXPR_NULL)
        return NULL;
#ifdef DVS_POOL_SINGLE_SEGMENT
    return p->fixed + off;
#else
    return p->seg[off >> DVS_SEG_SHIFT] + (off & DVS_SEG_MASK);
#endif
}

static inline dvs_spool_mark_t dvs_spool_mark(const dvs_spool_t *p) {
    dvs_spool_mark_t m = { p->top, p->end, p->overflow, p->n_back };
    return m;
}

/** The lowest offset dvs_spool_alloc() can return after `mark` was taken. */
static inline uint32_t dvs_spool_mark_offset(dvs_spool_mark_t mark) {
    return mark.top;
}

/**
 * Free everything allocated since `mark` and restore the overflow flag to
 * its value then. Offsets >= dvs_spool_mark_offset(mark) become invalid.
 * Built with DVS_DEBUG_ALWAYS_MOVE the freed bytes are filled with 0xA5;
 * under AddressSanitizer they are poisoned until allocated again.
 */
void dvs_spool_release(dvs_spool_t *p, dvs_spool_mark_t mark);

/** The pool's high-water offset: bytes of offset space in use. */
static inline uint32_t dvs_spool_used(const dvs_spool_t *p) {
    return p->top;
}

/** Bytes the pool can still hand out without growing (fixed region). */
uint32_t dvs_spool_fixed_free(const dvs_spool_t *p);

#endif /* DVS_SPOOL_H */
