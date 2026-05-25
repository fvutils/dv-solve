/* Test zsp_bvbounds: range + signed-split bounds. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsp_bvbounds.h"
#include "zsp_bvdom.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

static void test_range_basics(void) {
    zsp_bvrange_t r;
    zsp_bvrange_init(&r, 8, 10, 100);
    CHECK(!zsp_bvrange_is_empty(&r), "non-empty after init");
    CHECK(zsp_bvrange_is_valid(&r), "10..100 valid");
    CHECK(zsp_bvrange_contains_u(&r, 50), "contains 50");
    CHECK(!zsp_bvrange_contains_u(&r, 200), "does not contain 200");
    CHECK(!zsp_bvrange_contains_u(&r, 9), "does not contain 9");

    zsp_bvrange_init_empty(&r, 8);
    CHECK(zsp_bvrange_is_empty(&r), "empty after init_empty");
    CHECK(zsp_bvrange_is_valid(&r), "empty is valid");
    CHECK(!zsp_bvrange_contains_u(&r, 0), "empty contains nothing");
}

static void test_range_intersect(void) {
    zsp_bvrange_t a, b, c;
    zsp_bvrange_init(&a, 8, 10, 50);
    zsp_bvrange_init(&b, 8, 30, 80);
    zsp_bvrange_intersect(&c, &a, &b);
    CHECK(c.min == 30 && c.max == 50, "[10,50] ∩ [30,80] = [30,50]");

    zsp_bvrange_init(&a, 8, 10, 20);
    zsp_bvrange_init(&b, 8, 30, 80);
    zsp_bvrange_intersect(&c, &a, &b);
    CHECK(zsp_bvrange_is_empty(&c), "disjoint ranges intersect to empty");
}

static void test_bounds_from_range(void) {
    zsp_bvbounds_t b;

    /* 8-bit. max_signed = 127. min_signed (as unsigned) = 128. */

    /* entirely in lo half */
    zsp_bvbounds_init_range(&b, 8, 10, 100);
    CHECK(zsp_bvbounds_has_lo(&b) && !zsp_bvbounds_has_hi(&b),
          "[10,100] lo only");
    CHECK(b.lo.min == 10 && b.lo.max == 100, "lo = [10,100]");

    /* entirely in hi half */
    zsp_bvbounds_init_range(&b, 8, 130, 200);
    CHECK(!zsp_bvbounds_has_lo(&b) && zsp_bvbounds_has_hi(&b),
          "[130,200] hi only");
    CHECK(b.hi.min == 130 && b.hi.max == 200, "hi = [130,200]");

    /* straddling */
    zsp_bvbounds_init_range(&b, 8, 100, 200);
    CHECK(zsp_bvbounds_has_lo(&b) && zsp_bvbounds_has_hi(&b),
          "[100,200] both halves");
    CHECK(b.lo.min == 100 && b.lo.max == 127, "lo half = [100,127]");
    CHECK(b.hi.min == 128 && b.hi.max == 200, "hi half = [128,200]");

    /* full range */
    zsp_bvbounds_init_range(&b, 8, 0, 255);
    CHECK(zsp_bvbounds_has_lo(&b) && zsp_bvbounds_has_hi(&b),
          "[0,255] both halves");

    /* invalid (min > max) */
    zsp_bvbounds_init_range(&b, 8, 200, 100);
    CHECK(zsp_bvbounds_empty(&b), "min>max -> empty");
}

static void test_bounds_contains(void) {
    zsp_bvbounds_t b;
    zsp_bvbounds_init_range(&b, 8, 100, 200);
    CHECK(zsp_bvbounds_contains(&b, 100), "contains 100 (lo)");
    CHECK(zsp_bvbounds_contains(&b, 150), "contains 150 (hi)");
    CHECK(zsp_bvbounds_contains(&b, 127), "contains boundary 127");
    CHECK(zsp_bvbounds_contains(&b, 128), "contains boundary 128");
    CHECK(!zsp_bvbounds_contains(&b, 50), "does not contain 50");
    CHECK(!zsp_bvbounds_contains(&b, 250), "does not contain 250");
}

static void test_bounds_is_valid(void) {
    zsp_bvbounds_t b;
    zsp_bvbounds_init_range(&b, 8, 100, 200);
    CHECK(zsp_bvbounds_is_valid(&b), "split bounds valid");

    /* manually construct invalid: put msb=1 value in lo */
    zsp_bvrange_init(&b.lo, 8, 200, 220);  /* msb=1 but in lo half */
    CHECK(!zsp_bvbounds_is_valid(&b), "msb=1 in lo half is invalid");
}

static void test_bounds_intersect(void) {
    zsp_bvbounds_t a, b, c;
    /* a covers [50, 180], b covers [100, 220] */
    zsp_bvbounds_init_range(&a, 8, 50, 180);
    zsp_bvbounds_init_range(&b, 8, 100, 220);
    zsp_bvbounds_intersect(&c, &a, &b);
    /* Expected: lo half [100, 127] ∩ [50, 127] = [100, 127];
     *           hi half [128, 220] ∩ [128, 180] = [128, 180]. */
    CHECK(zsp_bvbounds_has_lo(&c) && c.lo.min == 100 && c.lo.max == 127,
          "intersect lo = [100,127]");
    CHECK(zsp_bvbounds_has_hi(&c) && c.hi.min == 128 && c.hi.max == 180,
          "intersect hi = [128,180]");
}

static void test_bounds_from_domain(void) {
    zsp_bvdom_t d;
    zsp_bvdom_t d2;
    zsp_bvbounds_t b;

    /* fixed value 42 — bounds is [42,42] */
    zsp_bvdom_init_value(&d, 8, 42);
    zsp_bvbounds_init_from_domain(&b, &d);
    CHECK(zsp_bvbounds_has_lo(&b) && !zsp_bvbounds_has_hi(&b)
       && b.lo.min == 42 && b.lo.max == 42, "domain=42 -> bounds=[42,42] in lo");

    /* domain 1xxx_xxxx (msb=1, others unknown): lo=0x80=128, hi=0xff=255 */
    zsp_bvdom_init_unknown(&d2, 8);
    zsp_bvdom_fix_bit(&d2, 7, 1);
    zsp_bvbounds_init_from_domain(&b, &d2);
    CHECK(!zsp_bvbounds_has_lo(&b) && zsp_bvbounds_has_hi(&b)
       && b.hi.min == 128 && b.hi.max == 255, "domain 1xxxxxxx -> [128,255] hi");
}

int main(void) {
    test_range_basics();
    test_range_intersect();
    test_bounds_from_range();
    test_bounds_contains();
    test_bounds_is_valid();
    test_bounds_intersect();
    test_bounds_from_domain();
    return failures ? 1 : 0;
}
