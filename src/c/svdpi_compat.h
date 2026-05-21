#ifndef SVDPI_COMPAT_H
#define SVDPI_COMPAT_H

/*
 * Minimal svdpi type definitions for standalone build and testing.
 * Simulators provide the real svdpi.h at link time; this header is
 * only used when compiling without a simulator SDK.
 *
 * For testing, svOpenArrayHandle is treated as a simple struct pointer
 * wrapping a data pointer and element count.
 */

#ifndef INCLUDED_SVDPI
/* Only define if the real svdpi.h hasn't been included */

#include <stdint.h>
#include <stddef.h>

/*
 * In real simulators, svOpenArrayHandle is an opaque pointer.
 * For standalone testing, we define a concrete struct that our
 * stub implementation can work with.
 */
typedef struct {
    void    *data;      /* pointer to contiguous element data */
    int32_t  length;    /* number of elements */
    int32_t  lo;        /* lower index bound (typically 0) */
} svOpenArrayCompat;

typedef svOpenArrayCompat *svOpenArrayHandle;

/* svGetArrayPtr: return pointer to contiguous storage */
static inline void *svGetArrayPtr(svOpenArrayHandle h) {
    if (!h) return NULL;
    return h->data;
}

/* svSize: return the number of elements */
static inline int svSize(svOpenArrayHandle h, int dim) {
    (void)dim;
    if (!h) return 0;
    return h->length;
}

/* svLeft/svRight: index bounds */
static inline int svLeft(svOpenArrayHandle h, int dim) {
    (void)dim;
    if (!h) return 0;
    return h->lo + h->length - 1;
}

static inline int svRight(svOpenArrayHandle h, int dim) {
    (void)dim;
    if (!h) return 0;
    return h->lo;
}

#endif /* INCLUDED_SVDPI */

#endif /* SVDPI_COMPAT_H */
