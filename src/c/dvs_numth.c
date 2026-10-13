/* Number theory for the multiply reasoning (dvs_numth.h). */
#include "dvs_numth.h"

uint64_t dvs_isqrt_u64(uint64_t n) {
    if (n < 2) return n;
    /* Newton from above: x0 = 2^ceil(bits/2) >= sqrt(n). */
    unsigned bits = 64 - (unsigned)__builtin_clzll(n);
    uint64_t x = (uint64_t)1 << ((bits + 1) / 2);
    for (;;) {
        uint64_t y = (x + n / x) >> 1;
        if (y >= x) return x;
        x = y;
    }
}

uint64_t dvs_isqrt_ceil_u64(uint64_t n) {
    uint64_t s = dvs_isqrt_u64(n);
    return (s * s == n) ? s : s + 1;
}

int dvs_is_square_residue(uint64_t n, unsigned w) {
    uint64_t mask = (w >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1);
    n &= mask;
    if (n == 0) return 1;
    unsigned t = (unsigned)__builtin_ctzll(n);
    if (t & 1) return 0;
    unsigned left = w - t;            /* bits of the odd part that are fixed */
    uint64_t o = n >> t;
    if (left >= 3) return (o & 7) == 1;
    if (left == 2) return (o & 3) == 1;
    return 1;
}
