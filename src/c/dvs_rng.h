/*
 * dvs_rng.h -- the stateless per-variable random draws of a canonical solve.
 *
 * Candidate k of variable i in a solve with seed s is a pure function of
 * (s, i, k): a SplitMix64 stream whose state is the counter. Nothing is
 * saved or undone on backtrack, and a retry is the next k, so the values a
 * canonical solve returns cannot depend on the search path that led to them
 * (docs/design/dvs_random_stability.md in the Verilator integration).
 */
#ifndef DVS_RNG_H
#define DVS_RNG_H

#include <stdint.h>
#include "dvs_i128.h"

/* The SplitMix64 finalizer. */
static inline uint64_t dvs_rng_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Variable `var`'s stream key in a solve with seed `seed`. */
static inline uint64_t dvs_rng_key(uint64_t seed, uint32_t var) {
    return dvs_rng_mix(seed ^ dvs_rng_mix((uint64_t)var + 1u));
}

/* Draw k (0-based) of the stream `key`. */
static inline uint64_t dvs_rng_draw(uint64_t key, uint32_t k) {
    return dvs_rng_mix(key + ((uint64_t)k + 1u) * 0x9E3779B97F4A7C15ULL);
}

/* `r` mapped onto [0, span) by a multiply-high (Lemire, without the
 * rejection step: a stateless draw cannot draw again, and the bias is below
 * span / 2^64). span 0 stands for 2^64. */
static inline uint64_t dvs_rng_below(uint64_t r, uint64_t span) {
    if (span == 0) return r;
    uint64_t hi, lo;
    dvs_mul_u64_wide(r, span, &hi, &lo);
    (void)lo;
    return hi;
}

#endif /* DVS_RNG_H */
