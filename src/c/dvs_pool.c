#include <string.h>
#include "dvs_pool.h"

/* AddressSanitizer: bytes a release frees stay poisoned until allocated
 * again, so a reference kept across dvs_pool_release() faults. */
#if defined(__SANITIZE_ADDRESS__)
#  define DVS_POOL_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define DVS_POOL_ASAN 1
#  endif
#endif
#ifdef DVS_POOL_ASAN
#  include <sanitizer/asan_interface.h>
#  define POOL_POISON(p, n)    ASAN_POISON_MEMORY_REGION((p), (n))
#  define POOL_UNPOISON(p, n)  ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#  define POOL_POISON(p, n)    ((void)(p), (void)(n))
#  define POOL_UNPOISON(p, n)  ((void)(p), (void)(n))
#endif

/* Offset of the first usable byte (i.e. right after the header). */
#define POOL_HEADER_SZ  ((uint32_t)sizeof(dvs_pool_t))

dvs_pool_t *dvs_pool_init(void *buf, size_t buf_size) {
    if (!buf || buf_size <= sizeof(dvs_pool_t))
        return NULL;

    dvs_pool_t *pool = (dvs_pool_t *)buf;
    pool->capacity = (uint32_t)(buf_size - sizeof(dvs_pool_t));
    pool->used     = 0;
    pool->overflow = 0;
    pool->_pad     = 0;
    return pool;
}

uint32_t dvs_pool_alloc(dvs_pool_t *pool, uint32_t bytes, uint32_t align) {
    if (pool->overflow)
        return EXPR_NULL;

    /* Normalise align */
    if (align < 1) align = 1;

    /* Current bump pointer relative to the data region */
    uint32_t base = pool->used;

    /* Round up to requested alignment */
    if (align > 1) {
        uint32_t mask = align - 1;
        base = (base + mask) & ~mask;
    }

    /* Check for overflow */
    if (bytes == 0) {
        /* Zero-byte allocation: return current (aligned) position */
        pool->used = base;
        /* Return offset from start of *buffer* (not data region) */
        return POOL_HEADER_SZ + base;
    }

    if ((uint64_t)base + bytes > pool->capacity) {
        pool->overflow = 1;
        return EXPR_NULL;
    }

    pool->used = base + bytes;
    POOL_UNPOISON((char *)pool + POOL_HEADER_SZ + base, bytes);
    return POOL_HEADER_SZ + base;
}

void dvs_pool_release(dvs_pool_t *pool, dvs_pool_mark_t mark) {
    if (mark.used >= pool->used) return;
    char *freed = (char *)pool + POOL_HEADER_SZ + mark.used;
    uint32_t n  = pool->used - mark.used;
#ifdef DVS_DEBUG_ALWAYS_MOVE
    memset(freed, 0xA5, n);
#endif
    POOL_POISON(freed, n);
    pool->used     = mark.used;
    pool->overflow = mark.overflow;
}

void *(dvs_pool_ptr)(const dvs_pool_t *pool, uint32_t offset) {
    return dvs_pool_ptr_inline(pool, offset);
}

void dvs_pool_reset(dvs_pool_t *pool) {
    pool->used     = 0;
    pool->overflow = 0;
}

uint32_t dvs_pool_used(const dvs_pool_t *pool) {
    return pool->used;
}
