/* Test: dvs_spool_t, the segmented context pool (docs/dynamic_pool_plan.md
 * P1) -- decode across slots, large objects, mark/release and slot reuse,
 * fixed mode, a buffer smaller than one slot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvs_spool.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

/* Counts live bytes so leaks and double frees show. */
typedef struct { dvs_alloc_t base; long live; long allocs; } count_alloc_t;
static void *c_alloc(dvs_alloc_t *s, size_t n) {
    count_alloc_t *c = (count_alloc_t *)s; c->live += (long)n; c->allocs++; return malloc(n);
}
static void c_release(dvs_alloc_t *s, void *p, size_t n) {
    count_alloc_t *c = (count_alloc_t *)s; c->live -= (long)n; free(p);
}

/* Fill an allocation with a pattern derived from its offset; check it. */
static void fill(dvs_spool_t *p, uint32_t off, uint32_t n) {
    uint8_t *m = (uint8_t *)dvs_spool_ptr(p, off);
    for (uint32_t i = 0; i < n; i++) m[i] = (uint8_t)(off * 7u + i);
}
static int intact(dvs_spool_t *p, uint32_t off, uint32_t n) {
    uint8_t *m = (uint8_t *)dvs_spool_ptr(p, off);
    for (uint32_t i = 0; i < n; i++)
        if (m[i] != (uint8_t)(off * 7u + i)) return 0;
    return 1;
}

static void test_growing(void) {
    count_alloc_t ca = { { c_alloc, c_release, NULL }, 0, 0 };
    static uint8_t buf[3000];
    dvs_spool_t p;
    CHECK(dvs_spool_init(&p, buf, sizeof buf, &ca.base) == 0);

    enum { N = 4000 };
    static uint32_t off[N], len[N];
    uint32_t n = 0;
    srand(1);
    /* Mostly small, some spanning several slots, a few exactly a slot. */
    for (; n < N; n++) {
        uint32_t r = (uint32_t)rand() % 100;
        len[n] = r < 90 ? 1 + (uint32_t)rand() % 200
               : r < 97 ? DVS_SEG_SIZE + (uint32_t)rand() % (3 * DVS_SEG_SIZE)
               : DVS_SEG_SIZE;
        off[n] = dvs_spool_alloc(&p, len[n], 8);
        CHECK(off[n] != EXPR_NULL && off[n] >= 16);
        CHECK(n == 0 || off[n] >= off[n - 1] + len[n - 1]);   /* in order */
        CHECK(((uintptr_t)dvs_spool_ptr(&p, off[n]) & 7u) == 0);
        fill(&p, off[n], len[n]);
    }
    for (uint32_t i = 0; i < N; i++) CHECK(intact(&p, off[i], len[i]));
    /* A large object is contiguous: its last byte decodes to base + len-1. */
    for (uint32_t i = 0; i < N; i++)
        if (len[i] > DVS_SEG_SIZE)
            CHECK((uint8_t *)dvs_spool_ptr(&p, off[i] + len[i] - 1)
                  == (uint8_t *)dvs_spool_ptr(&p, off[i]) + len[i] - 1);

    /* Mark, allocate a lot, release: everything before survives, the
     * pool returns to the mark, and push/pop cycles reuse slots. */
    dvs_spool_mark_t m = dvs_spool_mark(&p);
    uint32_t used = dvs_spool_used(&p);
    long live_at_mark = ca.live;
    for (int cycle = 0; cycle < 200; cycle++) {
        dvs_spool_mark_t mc = dvs_spool_mark(&p);
        for (int j = 0; j < 50; j++) {
            uint32_t sz = j == 7 ? 2 * DVS_SEG_SIZE + 5 : 300;
            uint32_t o = dvs_spool_alloc(&p, sz, 4);
            CHECK(o != EXPR_NULL && o >= dvs_spool_mark_offset(mc));
        }
        dvs_spool_release(&p, mc);
        CHECK(dvs_spool_used(&p) == used);
    }
    long allocs_before = ca.allocs;
    for (int cycle = 0; cycle < 100; cycle++) {
        dvs_spool_mark_t mc = dvs_spool_mark(&p);
        for (int j = 0; j < 10; j++) dvs_spool_alloc(&p, DVS_SEG_SIZE / 8, 8);
        dvs_spool_release(&p, mc);
    }
    /* Small slots come back from the spare list, not the allocator. */
    CHECK(ca.allocs - allocs_before <= (long)DVS_SPOOL_SPARES);
    dvs_spool_release(&p, m);
    CHECK(dvs_spool_used(&p) == used);
    CHECK(ca.live >= live_at_mark);
    for (uint32_t i = 0; i < N; i++) CHECK(intact(&p, off[i], len[i]));

    dvs_spool_destroy(&p);
    CHECK(ca.live == 0);
}

static void test_fixed(void) {
    /* Fixed mode: the slot table comes out of the buffer; an allocation
     * that does not fit fails and the failure is sticky until released. */
    static uint8_t buf[3 * 65536 + 100];
    size_t size = 2 * DVS_SEG_SIZE + 300 < sizeof buf ? 2 * DVS_SEG_SIZE + 300 : sizeof buf;
    dvs_spool_t p;
    CHECK(dvs_spool_init(&p, buf, size, NULL) == 0);
    uint32_t a = dvs_spool_alloc(&p, 100, 8);
    CHECK(a != EXPR_NULL);
    fill(&p, a, 100);
    dvs_spool_mark_t m = dvs_spool_mark(&p);
    /* Spans the fixed slots: contiguous, the fixed region is one block. */
    uint32_t big = dvs_spool_alloc(&p, DVS_SEG_SIZE + 10, 8);
    CHECK(big != EXPR_NULL);
    fill(&p, big, DVS_SEG_SIZE + 10);
    CHECK(intact(&p, big, DVS_SEG_SIZE + 10));
    CHECK(dvs_spool_alloc(&p, (uint32_t)size, 8) == EXPR_NULL);
    CHECK(p.overflow);
    CHECK(dvs_spool_alloc(&p, 1, 1) == EXPR_NULL);           /* sticky */
    dvs_spool_release(&p, m);
    CHECK(!p.overflow);
    CHECK(dvs_spool_alloc(&p, 1, 1) != EXPR_NULL);
    CHECK(intact(&p, a, 100));
    dvs_spool_destroy(&p);

    /* A buffer smaller than one slot. */
    static uint8_t tiny[512];
    CHECK(dvs_spool_init(&p, tiny, sizeof tiny, NULL) == 0);
    uint32_t t = dvs_spool_alloc(&p, 200, 8);
    CHECK(t != EXPR_NULL);
    fill(&p, t, 200);
    CHECK(intact(&p, t, 200));
    CHECK(dvs_spool_alloc(&p, 400, 8) == EXPR_NULL);
    dvs_spool_destroy(&p);

    /* Too small for even the slot table. */
    CHECK(dvs_spool_init(&p, tiny, 8, NULL) != 0);
}

static void test_no_buffer(void) {
    /* Growth only (the dvs_solver_new shape): no fixed region at all. */
    count_alloc_t ca = { { c_alloc, c_release, NULL }, 0, 0 };
    dvs_spool_t p;
    CHECK(dvs_spool_init(&p, NULL, 0, &ca.base) == 0);
    uint32_t a = dvs_spool_alloc(&p, 24, 8);
    CHECK(a != EXPR_NULL && a >= 16);
    fill(&p, a, 24);
    uint32_t b = dvs_spool_alloc(&p, 5 * DVS_SEG_SIZE, 16);
    CHECK(b != EXPR_NULL);
    fill(&p, b, 5 * DVS_SEG_SIZE);
    CHECK(intact(&p, a, 24) && intact(&p, b, 5 * DVS_SEG_SIZE));
    dvs_spool_destroy(&p);
    CHECK(ca.live == 0);
}

int main(void) {
    test_growing();
    test_fixed();
    test_no_buffer();
    printf("DVS_SEG_SHIFT=%u: %s\n", (unsigned)DVS_SEG_SHIFT, failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
