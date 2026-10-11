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

/* The lowest address the calling thread's stack may grow down to while still
 * leaving `margin` bytes free, or 0 if the platform cannot tell. Recursive
 * passes compare the address of a local against it (dvs_stack_exhausted) and
 * refuse the input rather than overflow: a fixed depth limit cannot be right
 * for every stack -- 1 MB on a Windows thread, 512 KB on a macOS secondary
 * thread, whatever the caller gave its own threads. */
uintptr_t dvs_stack_floor(size_t margin);

/* The calling thread's whole stack size in bytes, or 0 if unknown. */
size_t dvs_stack_size(void);

/* An address in the caller's frame on the thread's real stack. Not the
 * address of a local: under AddressSanitizer's detect_stack_use_after_return
 * (and MSVC's /fsanitize=address equivalent) locals live in a heap-allocated
 * "fake stack", so a local's address says nothing about stack depth and every
 * call would look exhausted. The frame address stays on the real stack. */
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

/* Nonzero when the caller's stack has reached `floor` (0 never has). */
static inline int dvs_stack_exhausted(uintptr_t floor) {
    return floor && dvs_stack_here() < floor;
}

#ifdef __cplusplus
}
#endif

#endif /* DVS_STACKINFO_H */
