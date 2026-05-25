/* Test zsp_bvdom: predicates and transformations of the 3-valued
 * bit-vector domain. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsp_bvdom.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

static void test_init_and_predicates(void) {
    zsp_bvdom_t d;
    zsp_bvdom_init_unknown(&d, 8);
    CHECK(d.width == 8 && d.lo == 0 && d.hi == 0xFF, "unknown init: lo=0 hi=ff");
    CHECK(zsp_bvdom_is_valid(&d), "unknown is valid");
    CHECK(!zsp_bvdom_is_fixed(&d), "unknown not fixed");
    CHECK(!zsp_bvdom_has_fixed_bits(&d), "unknown has no fixed bits");

    zsp_bvdom_init_value(&d, 8, 0xa5);
    CHECK(d.lo == 0xa5 && d.hi == 0xa5, "value init");
    CHECK(zsp_bvdom_is_fixed(&d), "fixed value is fixed");
    CHECK(zsp_bvdom_has_fixed_bits_true(&d), "0xa5 has fixed-1 bits");
    CHECK(zsp_bvdom_has_fixed_bits_false(&d), "0xa5 has fixed-0 bits");

    zsp_bvdom_init_unknown(&d, 4);
    zsp_bvdom_fix_bit(&d, 0, 1);
    zsp_bvdom_fix_bit(&d, 3, 0);
    CHECK(zsp_bvdom_is_fixed_bit_true(&d, 0), "bit0=1");
    CHECK(zsp_bvdom_is_fixed_bit_false(&d, 3), "bit3=0");
    CHECK(!zsp_bvdom_is_fixed_bit(&d, 1), "bit1 still unknown");
}

static void test_match_and_copy(void) {
    zsp_bvdom_t d;
    zsp_bvdom_init_unknown(&d, 8);
    zsp_bvdom_fix_bit(&d, 0, 1);    /* bit0 = 1 */
    zsp_bvdom_fix_bit(&d, 1, 0);    /* bit1 = 0 */

    CHECK(zsp_bvdom_match(&d, 0x01), "match 0x01 (bit0=1, bit1=0)");
    CHECK(!zsp_bvdom_match(&d, 0x00), "no match 0x00 (bit0 should be 1)");
    CHECK(!zsp_bvdom_match(&d, 0x03), "no match 0x03 (bit1 should be 0)");
    CHECK(zsp_bvdom_match(&d, 0xfd), "match 0xfd (bit0=1, bit1=0 — others free)");

    /* copy_with_fixed_bits: take 0xff, override bits 0,1 with their fixed values */
    uint64_t r = zsp_bvdom_copy_with_fixed_bits(&d, 0xff);
    CHECK(r == 0xfd, "0xff with d's fixed bits = 0xfd");
}

static void test_meet(void) {
    zsp_bvdom_t a, b, r;
    /* a: bit0 fixed to 1; b: bit0 fixed to 0 → meet should be invalid */
    zsp_bvdom_init_unknown(&a, 4); zsp_bvdom_fix_bit(&a, 0, 1);
    zsp_bvdom_init_unknown(&b, 4); zsp_bvdom_fix_bit(&b, 0, 0);
    zsp_bvdom_meet(&r, &a, &b);
    CHECK(!zsp_bvdom_is_valid(&r), "meet of conflicting bit0 is invalid");

    /* a: bit0=1, bit3=0; b: bit2=1; meet should fix bits 0, 2, 3 (bit1 free) */
    zsp_bvdom_init_unknown(&a, 4); zsp_bvdom_fix_bit(&a, 0, 1); zsp_bvdom_fix_bit(&a, 3, 0);
    zsp_bvdom_init_unknown(&b, 4); zsp_bvdom_fix_bit(&b, 2, 1);
    zsp_bvdom_meet(&r, &a, &b);
    CHECK(zsp_bvdom_is_valid(&r), "non-conflict meet is valid");
    CHECK(zsp_bvdom_is_fixed_bit_true(&r, 0)  && zsp_bvdom_is_fixed_bit_true(&r, 2)
       && zsp_bvdom_is_fixed_bit_false(&r, 3) && !zsp_bvdom_is_fixed_bit(&r, 1),
          "meet propagates each fixed bit");
}

static void test_bvnot(void) {
    zsp_bvdom_t a, r;
    /* a = 1x01 (msb-first): bit3=1, bit2=x, bit1=0, bit0=1 */
    zsp_bvdom_init_unknown(&a, 4);
    zsp_bvdom_fix_bit(&a, 3, 1); zsp_bvdom_fix_bit(&a, 1, 0); zsp_bvdom_fix_bit(&a, 0, 1);
    zsp_bvdom_bvnot(&r, &a);
    /* ~a = 0x10 (msb-first): bit3=0, bit2=x, bit1=1, bit0=0 */
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 3), "~: bit3=0");
    CHECK(!zsp_bvdom_is_fixed_bit(&r, 2),      "~: bit2 still x");
    CHECK(zsp_bvdom_is_fixed_bit_true(&r, 1),  "~: bit1=1");
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 0), "~: bit0=0");
}

static void test_bvand_bvor_bvxor(void) {
    zsp_bvdom_t a, b, r;
    /* a = 1x10, b = 11x0 (4 bits, bit3 MSB) */
    zsp_bvdom_init_unknown(&a, 4); zsp_bvdom_fix_bit(&a, 3, 1); zsp_bvdom_fix_bit(&a, 1, 1); zsp_bvdom_fix_bit(&a, 0, 0);
    zsp_bvdom_init_unknown(&b, 4); zsp_bvdom_fix_bit(&b, 3, 1); zsp_bvdom_fix_bit(&b, 2, 1); zsp_bvdom_fix_bit(&b, 0, 0);

    zsp_bvdom_bvand(&r, &a, &b);
    /* and: bit3=1&1=1, bit2=x&1=x, bit1=1&x=x, bit0=0&0=0 */
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 3), "and bit3=1");
    CHECK(!zsp_bvdom_is_fixed_bit     (&r, 2), "and bit2=x");
    CHECK(!zsp_bvdom_is_fixed_bit     (&r, 1), "and bit1=x");
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 0), "and bit0=0");

    zsp_bvdom_bvor(&r, &a, &b);
    /* or: bit3=1|1=1, bit2=x|1=1, bit1=1|x=1, bit0=0|0=0 */
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 3), "or bit3=1");
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 2), "or bit2=1");
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 1), "or bit1=1");
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 0), "or bit0=0");

    zsp_bvdom_bvxor(&r, &a, &b);
    /* xor: bit3=1^1=0, bit2=x^1=x, bit1=1^x=x, bit0=0^0=0 */
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 3), "xor bit3=0");
    CHECK(!zsp_bvdom_is_fixed_bit     (&r, 2), "xor bit2=x");
    CHECK(!zsp_bvdom_is_fixed_bit     (&r, 1), "xor bit1=x");
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 0), "xor bit0=0");
}

static void test_shifts(void) {
    zsp_bvdom_t a, r;
    /* a = 1x1x (4 bits, bit3 MSB): bit3=1, bit1=1, bit2/0 unknown */
    zsp_bvdom_init_unknown(&a, 4); zsp_bvdom_fix_bit(&a, 3, 1); zsp_bvdom_fix_bit(&a, 1, 1);

    zsp_bvdom_bvshl_const(&r, &a, 1);
    /* shl 1: bit3=x(was bit2), bit2=1(was bit1), bit1=x(was bit0), bit0=0 */
    CHECK(!zsp_bvdom_is_fixed_bit(&r, 3),       "shl1 bit3=x");
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 2),  "shl1 bit2=1");
    CHECK(!zsp_bvdom_is_fixed_bit(&r, 1),       "shl1 bit1=x");
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 0),  "shl1 bit0=0");

    zsp_bvdom_bvshr_const(&r, &a, 1);
    /* shr 1: bit3=0, bit2=1(was 3), bit1=x(was 2), bit0=1(was 1) */
    CHECK(zsp_bvdom_is_fixed_bit_false(&r, 3),  "shr1 bit3=0");
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 2),  "shr1 bit2=1");
    CHECK(!zsp_bvdom_is_fixed_bit     (&r, 1),  "shr1 bit1=x");
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 0),  "shr1 bit0=1");

    /* arithmetic shift: sign bit (bit3) is fixed-1, so fill with 1s */
    zsp_bvdom_bvashr_const(&r, &a, 1);
    /* ashr 1: bit3=1(sign), bit2=1(was 3), bit1=x(was 2), bit0=1(was 1) */
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 3),  "ashr1 bit3=1 (sign)");
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 2),  "ashr1 bit2=1");
    CHECK(!zsp_bvdom_is_fixed_bit     (&r, 1),  "ashr1 bit1=x");
    CHECK(zsp_bvdom_is_fixed_bit_true (&r, 0),  "ashr1 bit0=1");
}

static void test_extract_concat(void) {
    zsp_bvdom_t a, b, r;
    zsp_bvdom_init_value(&a, 4, 0xa);   /* 1010 */
    zsp_bvdom_init_value(&b, 4, 0x5);   /* 0101 */
    zsp_bvdom_bvconcat(&r, &a, &b);
    CHECK(r.width == 8 && zsp_bvdom_is_fixed(&r) && r.lo == 0xa5, "concat 1010||0101 = 0xa5");

    zsp_bvdom_init_value(&a, 8, 0xa5);
    zsp_bvdom_bvextract(&r, &a, 3, 0);
    CHECK(r.width == 4 && r.lo == 0x5, "extract [3:0] of 0xa5 = 0x5");
    zsp_bvdom_bvextract(&r, &a, 7, 4);
    CHECK(r.width == 4 && r.lo == 0xa, "extract [7:4] of 0xa5 = 0xa");
}

static void test_zero_sign_ext(void) {
    zsp_bvdom_t a, r;

    /* zero-extend 4 -> 8: 0xa (1010) -> 0x0a */
    zsp_bvdom_init_value(&a, 4, 0xa);
    zsp_bvdom_zero_ext(&r, &a, 4);
    CHECK(r.width == 8 && r.lo == 0x0a, "zext 0xa to 8 bits = 0x0a");

    /* sign-extend 4 -> 8 of negative (msb=1): 0xa -> 0xfa */
    zsp_bvdom_sign_ext(&r, &a, 4);
    CHECK(r.width == 8 && r.lo == 0xfa, "sext 0xa (neg) to 8 bits = 0xfa");

    /* sign-extend 4 -> 8 of positive (msb=0): 0x5 -> 0x05 */
    zsp_bvdom_init_value(&a, 4, 0x5);
    zsp_bvdom_sign_ext(&r, &a, 4);
    CHECK(r.width == 8 && r.lo == 0x05, "sext 0x5 (pos) to 8 bits = 0x05");

    /* sign-extend with unknown sign bit: 4-bit x x x x -> top 4 bits unknown,
     * bottom 4 unknown. So result is 8-bit fully unknown. */
    zsp_bvdom_init_unknown(&a, 4);
    zsp_bvdom_sign_ext(&r, &a, 4);
    CHECK(r.width == 8 && r.lo == 0 && r.hi == 0xff, "sext unknown is fully unknown");
}

static void test_to_str(void) {
    zsp_bvdom_t d;
    zsp_bvdom_init_unknown(&d, 4);
    zsp_bvdom_fix_bit(&d, 3, 1);
    zsp_bvdom_fix_bit(&d, 0, 0);
    char buf[32];
    zsp_bvdom_to_str(&d, buf, sizeof(buf));
    CHECK(strcmp(buf, "1xx0") == 0, "to_str produces '1xx0'");
}

int main(void) {
    test_init_and_predicates();
    test_match_and_copy();
    test_meet();
    test_bvnot();
    test_bvand_bvor_bvxor();
    test_shifts();
    test_extract_concat();
    test_zero_sign_ext();
    test_to_str();
    return failures ? 1 : 0;
}
