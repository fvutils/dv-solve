/* Test: cache limit + trim on dvs_block_alloc. */
#include <stdio.h>
#include <stdlib.h>

#include "dvs_block_alloc.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

int main(void) {
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 256);

    /* Default: unlimited cache. */
    void *b[8];
    for (int i = 0; i < 8; i++) b[i] = dvs_block_alloc_get(ba);
    for (int i = 0; i < 8; i++) dvs_block_alloc_put(ba, b[i]);
    CHECK(dvs_block_alloc_cached_count(ba) == 8, "default cache holds 8 blocks");

    /* Set limit = 3 — already at 8, but trim doesn't fire on set. */
    dvs_block_alloc_set_max_cached(ba, 3);
    CHECK(dvs_block_alloc_cached_count(ba) == 8, "set_max_cached does not trim immediately");

    /* Trim to 3. */
    dvs_block_alloc_trim(ba, 3);
    CHECK(dvs_block_alloc_cached_count(ba) == 3, "trim(3) leaves 3 cached");

    /* Get 3 from cache, then 2 more (fresh allocations). */
    for (int i = 0; i < 5; i++) b[i] = dvs_block_alloc_get(ba);
    CHECK(dvs_block_alloc_cached_count(ba) == 0, "cache empty after 5 gets");

    /* Put 5 back — only 3 should be cached (limit). */
    for (int i = 0; i < 5; i++) dvs_block_alloc_put(ba, b[i]);
    CHECK(dvs_block_alloc_cached_count(ba) == 3, "put respects max_cached=3");

    /* Remove limit — next put caches normally. */
    dvs_block_alloc_set_max_cached(ba, 0);
    void *b2 = dvs_block_alloc_get(ba);
    dvs_block_alloc_put(ba, b2);
    CHECK(dvs_block_alloc_cached_count(ba) == 3, "after get+put: 3-1+1 = 3");
    void *b3 = dvs_block_alloc_get(ba);
    void *b4 = dvs_block_alloc_get(ba);
    void *b5 = dvs_block_alloc_get(ba);
    void *b6 = dvs_block_alloc_get(ba);  /* fresh — cache was 3, now empty + 1 fresh */
    dvs_block_alloc_put(ba, b3);
    dvs_block_alloc_put(ba, b4);
    dvs_block_alloc_put(ba, b5);
    dvs_block_alloc_put(ba, b6);
    CHECK(dvs_block_alloc_cached_count(ba) == 4, "no limit: cache grows freely");

    dvs_block_alloc_destroy(ba);
    return failures ? 1 : 0;
}
