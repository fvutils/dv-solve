#include <string.h>
#include "zsp_pool.h"

/* Offset of the first usable byte (i.e. right after the header). */
#define POOL_HEADER_SZ  ((uint32_t)sizeof(zsp_pool_t))

zsp_pool_t *zsp_pool_init(void *buf, size_t buf_size) {
    if (!buf || buf_size <= sizeof(zsp_pool_t))
        return NULL;

    zsp_pool_t *pool = (zsp_pool_t *)buf;
    pool->capacity = (uint32_t)(buf_size - sizeof(zsp_pool_t));
    pool->used     = 0;
    pool->overflow = 0;
    pool->_pad     = 0;
    return pool;
}

uint32_t zsp_pool_alloc(zsp_pool_t *pool, uint32_t bytes, uint32_t align) {
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
    return POOL_HEADER_SZ + base;
}

void *zsp_pool_ptr(const zsp_pool_t *pool, uint32_t offset) {
    if (offset == EXPR_NULL)
        return NULL;
    return (void *)((char *)pool + offset);
}

void zsp_pool_reset(zsp_pool_t *pool) {
    pool->used     = 0;
    pool->overflow = 0;
}

uint32_t zsp_pool_used(const zsp_pool_t *pool) {
    return pool->used;
}
