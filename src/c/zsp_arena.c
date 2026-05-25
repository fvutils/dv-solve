#include "zsp_arena.h"

#include <stdlib.h>
#include <string.h>

struct zsp_arena_s {
    zsp_alloc_t *alloc;
    uint8_t     *buf;
    uint32_t     used;
    uint32_t     cap;
};

static void *xalloc(zsp_alloc_t *a, size_t sz) {
    return a ? ZSP_ALLOC(a, sz) : malloc(sz);
}
static void xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) ZSP_RELEASE(a, p, sz); else free(p);
}

static int grow_to(zsp_arena_t *a, uint32_t want_cap) {
    if (want_cap <= a->cap) return 0;
    if (want_cap > ZSP_AREF_MAX) return -1;
    uint32_t new_cap = a->cap ? a->cap : 16;
    while (new_cap < want_cap) {
        if (new_cap > ZSP_AREF_MAX / 2) {
            new_cap = ZSP_AREF_MAX;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < want_cap) return -1;
    uint8_t *nb = (uint8_t *)xalloc(a->alloc, new_cap);
    if (!nb) return -1;
    if (a->buf && a->used) memcpy(nb, a->buf, a->used);
    if (a->buf) xfree(a->alloc, a->buf, a->cap);
    a->buf = nb;
    a->cap = new_cap;
    return 0;
}

zsp_arena_t *zsp_arena_create(zsp_alloc_t *alloc, uint32_t initial_cap) {
    zsp_arena_t *a = (zsp_arena_t *)xalloc(alloc, sizeof(*a));
    if (!a) return NULL;
    a->alloc = alloc;
    a->buf = NULL;
    a->used = 0;
    a->cap = 0;
    if (initial_cap == 0) initial_cap = 4096;
    if (grow_to(a, initial_cap) != 0) {
        xfree(alloc, a, sizeof(*a));
        return NULL;
    }
    return a;
}

void zsp_arena_destroy(zsp_arena_t *a) {
    if (!a) return;
    if (a->buf) xfree(a->alloc, a->buf, a->cap);
    xfree(a->alloc, a, sizeof(*a));
}

zsp_aref_t zsp_arena_alloc(zsp_arena_t *a, uint32_t bytes, uint32_t align) {
    if (!a) return ZSP_AREF_NULL;
    if (align < 1) align = 1;

    uint32_t mask = align - 1;
    /* Sanity: align should be power of two. */
    if (align > 1 && (align & mask)) return ZSP_AREF_NULL;

    uint32_t base = (a->used + mask) & ~mask;
    /* Overflow check for base + bytes. */
    if (base < a->used) return ZSP_AREF_NULL;
    uint32_t end;
    if (__builtin_add_overflow(base, bytes, &end)) return ZSP_AREF_NULL;
    if (end > ZSP_AREF_MAX) return ZSP_AREF_NULL;

    if (end > a->cap) {
        if (grow_to(a, end) != 0) return ZSP_AREF_NULL;
    }
    a->used = end;
    return (zsp_aref_t)base;
}

void *zsp_arena_ptr(const zsp_arena_t *a, zsp_aref_t ref) {
    if (!a || ref == ZSP_AREF_NULL || ref > a->used) return NULL;
    return a->buf + ref;
}

uint32_t zsp_arena_used(const zsp_arena_t *a)     { return a ? a->used : 0; }
uint32_t zsp_arena_capacity(const zsp_arena_t *a) { return a ? a->cap : 0; }

void zsp_arena_reset(zsp_arena_t *a) { if (a) a->used = 0; }

zsp_arena_mark_t zsp_arena_mark(const zsp_arena_t *a) {
    zsp_arena_mark_t m = { a ? a->used : 0 };
    return m;
}

void zsp_arena_release(zsp_arena_t *a, zsp_arena_mark_t mark) {
    if (!a) return;
    if (mark.used <= a->used) a->used = mark.used;
}

void zsp_arena_shrink_to_fit(zsp_arena_t *a, uint32_t min_cap) {
    if (!a || !a->buf) return;
    uint32_t target = a->used > min_cap ? a->used : min_cap;
    if (target == 0) target = 16;
    /* Round target up to a power of two for the new capacity. */
    uint32_t new_cap = 16;
    while (new_cap < target) {
        if (new_cap > ZSP_AREF_MAX / 2) { new_cap = ZSP_AREF_MAX; break; }
        new_cap *= 2;
    }
    if (new_cap >= a->cap) return;  /* nothing to gain */
    uint8_t *nb = (uint8_t *)xalloc(a->alloc, new_cap);
    if (!nb) return;
    if (a->used) memcpy(nb, a->buf, a->used);
    xfree(a->alloc, a->buf, a->cap);
    a->buf = nb;
    a->cap = new_cap;
}
