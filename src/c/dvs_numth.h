/* Number theory for the multiply reasoning: integer square roots, and the
 * divisors of a fixed product (dvs_numth.c). */
#ifndef INCLUDED_DVS_NUMTH_H
#define INCLUDED_DVS_NUMTH_H
#include <stdint.h>
#include "dvs_alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* floor(sqrt(n)) */
uint64_t dvs_isqrt_u64(uint64_t n);

/* ceil(sqrt(n)) */
uint64_t dvs_isqrt_ceil_u64(uint64_t n);

/* Whether n can be a square mod 2^w (1 <= w <= 64): zero, or an even number
 * t of trailing zeros with the odd part 1 mod 8 (mod 4 when w - t == 2,
 * anything when w - t == 1). */
int dvs_is_square_residue(uint64_t n, unsigned w);

/* The divisors of a fixed product, sorted ascending (dvs_numth.c). One per
 * context: the last n factored is cached, since a search pins the same
 * product many times. */
typedef struct {
    uint64_t  n;        /* the number divs holds the divisors of; 0: none */
    uint64_t *divs;
    uint32_t  n_divs;
    uint32_t  cap;
    dvs_alloc_t *alloc; /* where divs comes from; NULL: dvs_malloc_alloc */
} DvsDivisors;

/* Divisors of n (n >= 1) into d. Returns the count, or 0 when n has more
 * than `max` divisors or memory runs out (the caller then learns nothing). */
uint32_t dvs_divisors(DvsDivisors *d, uint64_t n, uint32_t max);

/* Release divs. alloc is kept, so d can be used again. */
void dvs_divisors_free(DvsDivisors *d);

/* Index of the first divisor >= x, and of the first > x (n_divs if none). */
uint32_t dvs_divisors_lower(const DvsDivisors *d, uint64_t x);
uint32_t dvs_divisors_upper(const DvsDivisors *d, uint64_t x);

/* The inverse of a mod m (0 < a < m, m >= 1), by extended Euclid. Returns 1
 * and writes *out_inv in [0, m) when gcd(a, m) == 1; 0 when a and m share a
 * factor (no inverse exists). */
int dvs_modinv_u64(uint64_t a, uint64_t m, uint64_t *out_inv);

/* Merge x ≡ r1 (mod m1) and x ≡ r2 (mod m2) (m1, m2 >= 1) by the Chinese
 * remainder theorem into one x ≡ R (mod M), M = lcm(m1, m2).
 * Returns 1 and writes *out_r, *out_m when the two facts are consistent;
 * 0 when they contradict (no x satisfies both -- UNSAT); -1 when m1 or m2
 * is too large to merge without overflow (the caller keeps whichever fact
 * it already had rather than treat this as a contradiction). */
int dvs_crt_merge(int64_t r1, uint64_t m1, int64_t r2, uint64_t m2,
                   int64_t *out_r, uint64_t *out_m);

#ifdef __cplusplus
}
#endif
#endif
