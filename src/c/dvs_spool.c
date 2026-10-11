#include <string.h>
#include "dvs_spool.h"

/* AddressSanitizer: bytes a release frees stay poisoned until allocated
 * again, so a reference kept across dvs_spool_release() faults. */
#if defined(__SANITIZE_ADDRESS__)
#  define DVS_SPOOL_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define DVS_SPOOL_ASAN 1
#  endif
#endif
#ifdef DVS_SPOOL_ASAN
#  include <sanitizer/asan_interface.h>
#  define POISON(p, n)    ASAN_POISON_MEMORY_REGION((p), (n))
#  define UNPOISON(p, n)  ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#  define POISON(p, n)    ((void)(p), (void)(n))
#  define UNPOISON(p, n)  ((void)(p), (void)(n))
#endif

/* Offsets below this are never handed out (0 means "none" in tables). */
#define SPOOL_FIRST    16u
/* The offset space ends here; EXPR_NULL (0xFFFFFFFF) stays out of it. */
#define SPOOL_LIMIT    ((uint64_t)0xFFFF0000u)

static uint32_t _slots_for(uint64_t bytes) {
    return (uint32_t)((bytes + DVS_SEG_MASK) >> DVS_SEG_SHIFT);
}

int dvs_spool_init(dvs_spool_t *p, void *buf, size_t size, dvs_alloc_t *alloc) {
    memset(p, 0, sizeof(*p));
#ifdef DVS_POOL_SINGLE_SEGMENT
    alloc = NULL;                     /* one segment: the buffer, no growth */
#endif
    p->alloc = alloc;
    if (!buf) size = 0;
    if (size > SPOOL_LIMIT) size = (size_t)SPOOL_LIMIT;

    uint8_t *data = (uint8_t *)buf;
    uint32_t n_fixed = _slots_for(size);
    if (!alloc) {
        /* Fixed mode: the slot table lives at the front of the buffer. */
        size_t tb = ((size_t)n_fixed * sizeof(uint8_t *) + 15u) & ~(size_t)15u;
        if (tb >= size) return -1;
        p->seg = (uint8_t **)buf;
        p->seg_cap = n_fixed;
        data += tb;
        size -= tb;
        n_fixed = _slots_for(size);
    } else {
        uint32_t cap = n_fixed + 16u;
        p->seg = (uint8_t **)DVS_ALLOC(alloc, cap * sizeof(uint8_t *));
        if (!p->seg) return -1;
        p->seg_cap = cap;
        p->seg_owned = 1;
    }
    for (uint32_t i = 0; i < n_fixed; i++)
        p->seg[i] = data + (size_t)i * DVS_SEG_SIZE;
    p->fixed     = data;
    p->fixed_end = (uint32_t)size;
    p->top       = SPOOL_FIRST;
    p->end       = p->fixed_end;
    return 0;
}

static void _free_backing(dvs_spool_t *p, const dvs_spool_backing_t *b) {
    UNPOISON(b->mem, b->size);
    DVS_RELEASE(p->alloc, b->mem, b->size);
}

void dvs_spool_destroy(dvs_spool_t *p) {
    if (!p->alloc) return;
    while (p->n_back) _free_backing(p, &p->back[--p->n_back]);
    for (uint32_t i = 0; i < p->n_spare; i++) {
        UNPOISON(p->spare[i], DVS_SEG_SIZE);
        DVS_RELEASE(p->alloc, p->spare[i], DVS_SEG_SIZE);
    }
    p->n_spare = 0;
    if (p->back) DVS_RELEASE(p->alloc, p->back, p->back_cap * sizeof(*p->back));
    if (p->seg_owned) DVS_RELEASE(p->alloc, p->seg, p->seg_cap * sizeof(uint8_t *));
    p->back = NULL; p->back_cap = 0;
    p->seg = NULL; p->seg_cap = 0;
    p->top = p->end = 0;
    p->overflow = 1;
}

/* Open a backing for `bytes` at the next slot boundary past the current
 * one: a whole slot for a small object, exactly `bytes` across consecutive
 * slots for a larger one. */
static uint32_t _grow(dvs_spool_t *p, uint32_t bytes) {
    if (!p->alloc) goto full;
    uint64_t start = ((uint64_t)p->end + DVS_SEG_MASK) & ~(uint64_t)DVS_SEG_MASK;
    if (start < SPOOL_FIRST) start = DVS_SEG_SIZE;
    uint32_t size  = bytes <= DVS_SEG_SIZE ? DVS_SEG_SIZE : bytes;
    if (start + size > SPOOL_LIMIT) goto full;
    uint32_t s = (uint32_t)(start >> DVS_SEG_SHIFT);
    uint32_t k = _slots_for(size);

    if (s + k > p->seg_cap) {
        uint32_t cap = p->seg_cap * 2u;
        if (cap < s + k) cap = s + k + 16u;
        uint8_t **ns = (uint8_t **)DVS_RESIZE(p->alloc, p->seg,
                                              p->seg_cap * sizeof(uint8_t *),
                                              cap * sizeof(uint8_t *));
        if (!ns) goto full;
        p->seg = ns;
        p->seg_cap = cap;
    }
    if (p->n_back == p->back_cap) {
        uint32_t cap = p->back_cap ? 2u * p->back_cap : 16u;
        dvs_spool_backing_t *nb = (dvs_spool_backing_t *)DVS_RESIZE(
            p->alloc, p->back, p->back_cap * sizeof(*p->back), cap * sizeof(*p->back));
        if (!nb) goto full;
        p->back = nb;
        p->back_cap = cap;
    }
    uint8_t *mem;
    if (size == DVS_SEG_SIZE && p->n_spare) {
        mem = p->spare[--p->n_spare];
    } else {
        mem = (uint8_t *)DVS_ALLOC(p->alloc, size);
        if (!mem) goto full;
        POISON(mem, size);
    }
    for (uint32_t i = 0; i < k; i++)
        p->seg[s + i] = mem + (size_t)i * DVS_SEG_SIZE;
    dvs_spool_backing_t *b = &p->back[p->n_back++];
    b->start = (uint32_t)start;
    b->size  = size;
    b->mem   = mem;
    p->top = (uint32_t)start + bytes;
    p->end = (uint32_t)start + size;
    UNPOISON(mem, bytes);
    return (uint32_t)start;
full:
    p->overflow = 1;
    return EXPR_NULL;
}

uint32_t dvs_spool_alloc(dvs_spool_t *p, uint32_t bytes, uint32_t align) {
    if (p->overflow) return EXPR_NULL;
    if (bytes == 0) bytes = 1;          /* every offset names a backed byte */
    if (align < 1) align = 1;
    uint64_t base = ((uint64_t)p->top + (align - 1)) & ~(uint64_t)(align - 1);
    if (base + bytes <= p->end) {
        p->top = (uint32_t)(base + bytes);
        UNPOISON(dvs_spool_ptr(p, (uint32_t)base), bytes);
        return (uint32_t)base;
    }
    return _grow(p, bytes);
}

void dvs_spool_release(dvs_spool_t *p, dvs_spool_mark_t m) {
    if (m.n_back > p->n_back || (m.n_back == p->n_back && m.top >= p->top))
        return;
    /* What the retained backing (the one m.top is in) gives back. */
    uint32_t kept_end = m.n_back == p->n_back ? p->top : m.end;
    if (kept_end > m.top) {
        uint8_t *freed = (uint8_t *)dvs_spool_ptr(p, m.top);
#ifdef DVS_DEBUG_ALWAYS_MOVE
        memset(freed, 0xA5, kept_end - m.top);
#endif
        POISON(freed, kept_end - m.top);
    }
    while (p->n_back > m.n_back) {
        dvs_spool_backing_t *b = &p->back[--p->n_back];
        uint32_t s = b->start >> DVS_SEG_SHIFT, k = _slots_for(b->size);
        for (uint32_t i = 0; i < k; i++) p->seg[s + i] = NULL;
        if (b->size == DVS_SEG_SIZE && p->n_spare < DVS_SPOOL_SPARES) {
#ifdef DVS_DEBUG_ALWAYS_MOVE
            memset(b->mem, 0xA5, b->size);
#endif
            POISON(b->mem, b->size);
            p->spare[p->n_spare++] = b->mem;
        } else {
            _free_backing(p, b);
        }
    }
    p->top      = m.top;
    p->end      = m.end;
    p->overflow = m.overflow;
}

uint32_t dvs_spool_fixed_free(const dvs_spool_t *p) {
    return p->top < p->fixed_end ? p->fixed_end - p->top : 0;
}
