#ifndef DVS_ALLOC_H
#define DVS_ALLOC_H

#include <stddef.h>

/**
 * dvs_alloc_t — system-level allocator interface.
 *
 * All components that need raw memory call through this interface rather
 * than malloc/free directly.  A default implementation backed by malloc is
 * provided in dvs_block_alloc.c.
 *
 * Growing a block goes through DVS_RESIZE (dvs_alloc_resize), never through
 * the system realloc, so a custom allocator sees every byte the solver uses.
 */
typedef struct dvs_alloc_s {
    /** Allocate at least `size` bytes; return NULL on failure. */
    void *(*alloc)(struct dvs_alloc_s *self, size_t size);
    /** Release a block previously returned by `alloc`. */
    void  (*release)(struct dvs_alloc_s *self, void *ptr, size_t size);
    /**
     * Optional. Resize a block from `old_size` to `new_size` bytes, keeping
     * the first min(old_size, new_size) bytes; return NULL on failure and
     * leave `ptr` valid. NULL means dvs_alloc_resize() falls back to
     * alloc + copy + release, so an allocator written before this member
     * existed still works once recompiled.
     */
    void *(*resize)(struct dvs_alloc_s *self, void *ptr,
                    size_t old_size, size_t new_size);
} dvs_alloc_t;

/** Convenience wrappers */
#define DVS_ALLOC(a, sz)       ((a)->alloc((a), (sz)))
#define DVS_RELEASE(a, p, sz)  ((a)->release((a), (p), (sz)))

/**
 * Resize `ptr` (of `old_size` bytes) to `new_size` bytes through `a`.
 *
 * `a` == NULL means the default malloc allocator. `ptr` == NULL allocates.
 * `new_size` == 0 releases `ptr` and returns NULL. On failure returns NULL
 * and `ptr` is unchanged and still owned by the caller.
 *
 * Built with DVS_DEBUG_ALWAYS_MOVE, every resize moves the block and fills
 * the old one with 0xA5 before releasing it, so a pointer held across a
 * growth shows up as a use-after-free (ASan) or as garbage.
 */
void *dvs_alloc_resize(dvs_alloc_t *a, void *ptr,
                       size_t old_size, size_t new_size);

#define DVS_RESIZE(a, p, o, n)  dvs_alloc_resize((a), (p), (o), (n))

/**
 * Global default allocator — backed by malloc/realloc/free.
 * Declared here; defined in dvs_block_alloc.c.
 */
extern dvs_alloc_t dvs_malloc_alloc;

#endif /* DVS_ALLOC_H */
