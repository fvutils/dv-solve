#ifndef DVS_ARENA_H
#define DVS_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include "dvs_alloc.h"

/**
 * dvs_arena — resizable, offset-keyed bump allocator.
 *
 * Pattern: kissat's `stack`/arena combined with dvs_pool's 32-bit
 * offset addressing. Unlike dvs_pool (which is fixed-size), this arena
 * grows by power-of-two doubling on demand, with stable *offsets* but
 * **unstable pointers**.
 *
 *   uint32_t ref = dvs_arena_alloc(a, bytes, align);   // offset
 *   void    *p   = dvs_arena_ptr(a, ref);              // pointer (may move)
 *
 * Use offsets when storing references that must survive a grow event
 * (e.g. inside the arena itself, or in long-lived data structures).
 * Use pointers only for short-lived access between alloc events.
 *
 * Phase-B-1 motivation: this is the substrate for a kissat-style clause
 * arena where each clause is identified by a `cref_t` (32-bit offset),
 * watch lists hold the same `cref_t`, and the arena can compact /
 * slide-collapse with O(N) reference rewrites instead of an O(N²)
 * pointer fixup.
 *
 * Memory: allocations route through dvs_alloc_t (or libc malloc if NULL).
 * Capacity is bounded by DVS_AREF_MAX (2^31 - 1) to keep refs signed-safe.
 */

#define DVS_AREF_NULL ((uint32_t)0xFFFFFFFFu)
#define DVS_AREF_MAX  ((uint32_t)0x7FFFFFFFu)

typedef uint32_t dvs_aref_t;

typedef struct dvs_arena_s dvs_arena_t;

typedef struct {
    uint32_t used;
} dvs_arena_mark_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Create a fresh arena. `initial_cap` may be 0 (defaults to 4096). */
dvs_arena_t *dvs_arena_create(dvs_alloc_t *alloc, uint32_t initial_cap);

/** Destroy and free all memory. */
void dvs_arena_destroy(dvs_arena_t *a);

/**
 * Bump-allocate `bytes` bytes with `align`-byte alignment (power of two,
 * 0 or 1 → 1). Grows the backing buffer if needed. Returns DVS_AREF_NULL
 * on overflow (request larger than DVS_AREF_MAX) or allocation failure.
 * Note: any dvs_arena_ptr() result from before this call may now be
 * invalid (the buffer may have moved).
 */
dvs_aref_t dvs_arena_alloc(dvs_arena_t *a, uint32_t bytes, uint32_t align);

/**
 * Translate `ref` to a pointer inside the arena. The pointer is valid
 * only until the next alloc / release call.
 */
void *dvs_arena_ptr(const dvs_arena_t *a, dvs_aref_t ref);

/** Current allocation high-water (= valid range of refs is [0, used)). */
uint32_t dvs_arena_used(const dvs_arena_t *a);

/** Current capacity (bytes of backing buffer). */
uint32_t dvs_arena_capacity(const dvs_arena_t *a);

/**
 * Reset the arena: equivalent to dvs_arena_release(arena, {0}). All
 * outstanding refs become invalid. Capacity is preserved.
 */
void dvs_arena_reset(dvs_arena_t *a);

/**
 * Save the current allocation high-water for a future release().
 * Cheap and constant-size — analogous to dvs_stack_push.
 */
dvs_arena_mark_t dvs_arena_mark(const dvs_arena_t *a);

/**
 * Roll the arena back to a previously-saved mark. Any allocations made
 * after `mark` are discarded; their refs become invalid. Capacity is
 * preserved (no realloc).
 */
void dvs_arena_release(dvs_arena_t *a, dvs_arena_mark_t mark);

/**
 * Shrink the backing buffer if its capacity is wastefully larger than
 * `used`. The new capacity is at most `max(used, min_cap)` rounded up
 * to a power of two. No-op if the buffer is already at or below that
 * target. Like kissat's SHRINK_STACK, but works on the heap buffer.
 *
 * After this call, previously-issued pointers (from dvs_arena_ptr) may
 * be invalid; refs remain valid.
 */
void dvs_arena_shrink_to_fit(dvs_arena_t *a, uint32_t min_cap);

#ifdef __cplusplus
}
#endif

#endif /* DVS_ARENA_H */
