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

/* Nonzero when the caller's stack has reached `floor` (0 never has). */
static inline int dvs_stack_exhausted(uintptr_t floor) {
    char here;
    return floor && (uintptr_t)&here < floor;
}

#ifdef __cplusplus
}
#endif

#endif /* DVS_STACKINFO_H */
