/* Test the resizable offset-keyed arena. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsp_arena.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

int main(void) {
    zsp_arena_t *a = zsp_arena_create(NULL, 64);
    CHECK(a != NULL, "create");
    CHECK(zsp_arena_used(a) == 0, "initial used=0");
    CHECK(zsp_arena_capacity(a) == 64, "initial cap=64");

    /* Basic alloc + ptr roundtrip. */
    zsp_aref_t r1 = zsp_arena_alloc(a, 8, 4);
    CHECK(r1 == 0, "first ref = 0");
    int *p1 = (int *)zsp_arena_ptr(a, r1);
    p1[0] = 42; p1[1] = -7;

    zsp_aref_t r2 = zsp_arena_alloc(a, 12, 4);
    CHECK(r2 == 8, "second ref = 8");

    /* Re-read p1 — but a grow event may have moved the buffer, so we
     * must use ptr() again rather than the cached p1. We've only made
     * 20 bytes of allocations into cap=64; no grow expected. */
    int *p1b = (int *)zsp_arena_ptr(a, r1);
    CHECK(p1b[0] == 42 && p1b[1] == -7, "data preserved across alloc");

    /* Force a grow: allocate enough to push beyond 64. */
    uint32_t pre_cap = zsp_arena_capacity(a);
    zsp_aref_t r3 = zsp_arena_alloc(a, 100, 1);
    CHECK(r3 == 20, "third ref = 20 (no padding needed)");
    CHECK(zsp_arena_capacity(a) > pre_cap, "capacity grew");

    /* Refs survive the grow — but pointers may have moved. Re-fetch. */
    int *p1c = (int *)zsp_arena_ptr(a, r1);
    CHECK(p1c[0] == 42 && p1c[1] == -7, "data preserved across grow");

    /* mark/release. */
    zsp_arena_mark_t m = zsp_arena_mark(a);
    uint32_t before = zsp_arena_used(a);
    zsp_arena_alloc(a, 200, 1);
    zsp_arena_alloc(a, 50, 1);
    CHECK(zsp_arena_used(a) > before, "used grew after allocs");
    zsp_arena_release(a, m);
    CHECK(zsp_arena_used(a) == before, "release rolls back used");

    /* Big growth + shrink_to_fit. */
    while (zsp_arena_used(a) < 100000) {
        zsp_arena_alloc(a, 1000, 1);
    }
    uint32_t big_cap = zsp_arena_capacity(a);
    CHECK(big_cap >= 100000, "grew past 100k");
    zsp_arena_release(a, m);
    CHECK(zsp_arena_used(a) == before, "release back to before");
    zsp_arena_shrink_to_fit(a, 0);
    CHECK(zsp_arena_capacity(a) < big_cap, "shrink reduced capacity");
    CHECK(zsp_arena_capacity(a) >= zsp_arena_used(a), "shrink keeps used data fitting");

    /* And data is still there. */
    int *p1d = (int *)zsp_arena_ptr(a, r1);
    CHECK(p1d[0] == 42 && p1d[1] == -7, "data preserved across shrink");

    /* Alignment honored. */
    zsp_arena_reset(a);
    zsp_arena_alloc(a, 1, 1);          /* used=1 */
    zsp_aref_t r4 = zsp_arena_alloc(a, 4, 4);   /* must align to 4 */
    CHECK(r4 == 4, "alignment forces ref=4");

    zsp_arena_destroy(a);
    return failures ? 1 : 0;
}
