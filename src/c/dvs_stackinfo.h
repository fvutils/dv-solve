#ifndef DVS_STACKINFO_H
#define DVS_STACKINFO_H

/*
 * dvs_stackinfo -- how much stack the calling thread has (dvs_thread.c).
 *
 * Kept apart from dvs_thread.h so a recursive pass can ask without pulling in
 * <windows.h>.
 */

#include <stddef.h>
#include <stdint.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The calling thread's whole stack size in bytes, or 0 if unknown. */
size_t dvs_stack_size(void);

/* An address in the caller's frame on the thread's real stack. Not the
 * address of a local: under AddressSanitizer's detect_stack_use_after_return
 * (and MSVC's /fsanitize=address equivalent) locals live in a heap-allocated
 * "fake stack", so a local's address says nothing about stack depth (the guard
 * would find no stack and stop guarding). The frame address stays on the real
 * stack. */
static inline uintptr_t dvs_stack_here(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (uintptr_t)__builtin_frame_address(0);
#elif defined(_MSC_VER)
    return (uintptr_t)_AddressOfReturnAddress();
#else
    char here;
    return (uintptr_t)&here;
#endif
}

/* Stack a guarded pass leaves free below its deepest checked frame, for the
 * non-recursive work it does there (gate building, propagation, malloc). */
#define DVS_STACK_MARGIN ((size_t)128 * 1024)

/* A recursive pass's stack check. Embed one, zero-initialised, in the pass's
 * state and call dvs_stack_low() once per level: it is a subtract and a
 * compare while the pass stays on one thread's stack, and looks the stack up
 * again (cheaply, per thread) when it does not. On a stack the platform cannot
 * describe, it never refuses -- the pass then has only its own limits. */
typedef struct dvs_stack_guard {
    uintptr_t floor;   /* lowest address a guarded frame may reach */
    uintptr_t span;    /* stack top - floor; 0 = not looked up yet */
} dvs_stack_guard_t;

int dvs_stack_guard_slow(dvs_stack_guard_t *g, uintptr_t at);

/* Nonzero when fewer than DVS_STACK_MARGIN bytes of stack are left: the pass
 * must give up (answer "unknown", decline the constraint) rather than recurse. */
static inline int dvs_stack_low(dvs_stack_guard_t *g) {
    uintptr_t at = dvs_stack_here();
    if (at - g->floor < g->span) return 0;
    return dvs_stack_guard_slow(g, at);
}

/* dvs_stack_low(), plus a fixed `limit` on `depth` that applies only where the
 * stack cannot be measured. */
static inline int dvs_stack_deep(dvs_stack_guard_t *g, long depth, long limit) {
    return dvs_stack_low(g) || (depth > limit && g->span == UINTPTR_MAX);
}

#ifdef __cplusplus
}
#endif

#endif /* DVS_STACKINFO_H */
