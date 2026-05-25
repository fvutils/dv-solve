#ifndef ZSP_ARENA_H
#define ZSP_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include "zsp_alloc.h"

/**
 * zsp_arena — resizable, offset-keyed bump allocator.
 *
 * Pattern: kissat's `stack`/arena combined with zsp_pool's 32-bit
 * offset addressing. Unlike zsp_pool (which is fixed-size), this arena
 * grows by power-of-two doubling on demand, with stable *offsets* but
 * **unstable pointers**.
 *
 *   uint32_t ref = zsp_arena_alloc(a, bytes, align);   // offset
 *   void    *p   = zsp_arena_ptr(a, ref);              // pointer (may move)
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
 * Memory: allocations route through zsp_alloc_t (or libc malloc if NULL).
 * Capacity is bounded by ZSP_AREF_MAX (2^31 - 1) to keep refs signed-safe.
 */

#define ZSP_AREF_NULL ((uint32_t)0xFFFFFFFFu)
#define ZSP_AREF_MAX  ((uint32_t)0x7FFFFFFFu)

typedef uint32_t zsp_aref_t;

typedef struct zsp_arena_s zsp_arena_t;

typedef struct {
    uint32_t used;
} zsp_arena_mark_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Create a fresh arena. `initial_cap` may be 0 (defaults to 4096). */
zsp_arena_t *zsp_arena_create(zsp_alloc_t *alloc, uint32_t initial_cap);

/** Destroy and free all memory. */
void zsp_arena_destroy(zsp_arena_t *a);

/**
 * Bump-allocate `bytes` bytes with `align`-byte alignment (power of two,
 * 0 or 1 → 1). Grows the backing buffer if needed. Returns ZSP_AREF_NULL
 * on overflow (request larger than ZSP_AREF_MAX) or allocation failure.
 * Note: any zsp_arena_ptr() result from before this call may now be
 * invalid (the buffer may have moved).
 */
zsp_aref_t zsp_arena_alloc(zsp_arena_t *a, uint32_t bytes, uint32_t align);

/**
 * Translate `ref` to a pointer inside the arena. The pointer is valid
 * only until the next alloc / release call.
 */
void *zsp_arena_ptr(const zsp_arena_t *a, zsp_aref_t ref);

/** Current allocation high-water (= valid range of refs is [0, used)). */
uint32_t zsp_arena_used(const zsp_arena_t *a);

/** Current capacity (bytes of backing buffer). */
uint32_t zsp_arena_capacity(const zsp_arena_t *a);

/**
 * Reset the arena: equivalent to zsp_arena_release(arena, {0}). All
 * outstanding refs become invalid. Capacity is preserved.
 */
void zsp_arena_reset(zsp_arena_t *a);

/**
 * Save the current allocation high-water for a future release().
 * Cheap and constant-size — analogous to zsp_stack_push.
 */
zsp_arena_mark_t zsp_arena_mark(const zsp_arena_t *a);

/**
 * Roll the arena back to a previously-saved mark. Any allocations made
 * after `mark` are discarded; their refs become invalid. Capacity is
 * preserved (no realloc).
 */
void zsp_arena_release(zsp_arena_t *a, zsp_arena_mark_t mark);

/**
 * Shrink the backing buffer if its capacity is wastefully larger than
 * `used`. The new capacity is at most `max(used, min_cap)` rounded up
 * to a power of two. No-op if the buffer is already at or below that
 * target. Like kissat's SHRINK_STACK, but works on the heap buffer.
 *
 * After this call, previously-issued pointers (from zsp_arena_ptr) may
 * be invalid; refs remain valid.
 */
void zsp_arena_shrink_to_fit(zsp_arena_t *a, uint32_t min_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_ARENA_H */
