#ifndef DVS_ALLOC_H
#define DVS_ALLOC_H

#include <stddef.h>

/**
 * dvs_alloc_t — system-level allocator interface.
 *
 * All components that need raw memory call through this interface rather
 * than malloc/free directly.  A default implementation backed by malloc is
 * provided in dvs_block_alloc.c.
 */
typedef struct dvs_alloc_s {
    /** Allocate at least `size` bytes; return NULL on failure. */
    void *(*alloc)(struct dvs_alloc_s *self, size_t size);
    /** Release a block previously returned by `alloc`. */
    void  (*release)(struct dvs_alloc_s *self, void *ptr, size_t size);
} dvs_alloc_t;

/** Convenience wrappers */
#define DVS_ALLOC(a, sz)       ((a)->alloc((a), (sz)))
#define DVS_RELEASE(a, p, sz)  ((a)->release((a), (p), (sz)))

/**
 * Global default allocator — backed by malloc/free.
 * Declared here; defined in dvs_block_alloc.c.
 */
extern dvs_alloc_t dvs_malloc_alloc;

#endif /* DVS_ALLOC_H */
