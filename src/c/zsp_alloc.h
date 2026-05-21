#ifndef ZSP_ALLOC_H
#define ZSP_ALLOC_H

#include <stddef.h>

/**
 * zsp_alloc_t — system-level allocator interface.
 *
 * All components that need raw memory call through this interface rather
 * than malloc/free directly.  A default implementation backed by malloc is
 * provided in zsp_block_alloc.c.
 */
typedef struct zsp_alloc_s {
    /** Allocate at least `size` bytes; return NULL on failure. */
    void *(*alloc)(struct zsp_alloc_s *self, size_t size);
    /** Release a block previously returned by `alloc`. */
    void  (*release)(struct zsp_alloc_s *self, void *ptr, size_t size);
} zsp_alloc_t;

/** Convenience wrappers */
#define ZSP_ALLOC(a, sz)       ((a)->alloc((a), (sz)))
#define ZSP_RELEASE(a, p, sz)  ((a)->release((a), (p), (sz)))

/**
 * Global default allocator — backed by malloc/free.
 * Declared here; defined in zsp_block_alloc.c.
 */
extern zsp_alloc_t zsp_malloc_alloc;

#endif /* ZSP_ALLOC_H */
