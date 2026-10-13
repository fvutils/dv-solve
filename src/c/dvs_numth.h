/* Number theory for the multiply reasoning: integer square roots
 * (dvs_numth.c). */
#ifndef INCLUDED_DVS_NUMTH_H
#define INCLUDED_DVS_NUMTH_H
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
#endif
