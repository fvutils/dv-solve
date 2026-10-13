/* Number theory for the multiply reasoning (dvs_numth.h).
 *
 * A fixed product r = a * b that cannot wrap confines each operand to the
 * divisors of r. A number below 2^32 has at most 1344 of them, and one below
 * 2^64 at most 103680, so they are listed outright: factor by trial division
 * of the small primes, then Miller-Rabin and Pollard's rho for what is left. */
#include <stdlib.h>
#include <string.h>
#include "dvs_numth.h"
#include "dvs_i128.h"

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

static uint64_t _powmod(uint64_t b, uint64_t e, uint64_t m) {
    uint64_t r = 1 % m;
    b %= m;
    while (e) {
        if (e & 1) r = dvs_mulmod_u64(r, b, m);
        b = dvs_mulmod_u64(b, b, m);
        e >>= 1;
    }
    return r;
}

/* Deterministic Miller-Rabin for 64-bit n (these twelve bases suffice). */
static int _is_prime(uint64_t n) {
    static const uint64_t P[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };
    if (n < 2) return 0;
    for (unsigned i = 0; i < 12; i++) {
        if (n % P[i] == 0) return n == P[i];
    }
    uint64_t d = n - 1;
    unsigned s = 0;
    while (!(d & 1)) { d >>= 1; s++; }
    for (unsigned i = 0; i < 12; i++) {
        uint64_t x = _powmod(P[i], d, n);
        if (x == 1 || x == n - 1) continue;
        unsigned r;
        for (r = 1; r < s; r++) {
            x = dvs_mulmod_u64(x, x, n);
            if (x == n - 1) break;
        }
        if (r == s) return 0;
    }
    return 1;
}

static uint64_t _gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

/* (y*y + c) mod n, for c < n, without overflow. */
static inline uint64_t _rho_step(uint64_t y, uint64_t c, uint64_t n) {
    uint64_t t = dvs_mulmod_u64(y, y, n);
    return (t >= n - c) ? t - (n - c) : t + c;
}

/* A non-trivial factor of composite, odd n (Brent's variant of rho). */
static uint64_t _rho(uint64_t n) {
    for (uint64_t c = 1;; c++) {
        uint64_t y = 2, x = 2, g = 1, q = 1, ys = 2;
        uint64_t r = 1;
        while (g == 1) {
            x = y;
            for (uint64_t i = 0; i < r; i++) y = _rho_step(y, c, n);
            for (uint64_t k = 0; k < r && g == 1; k += 128) {
                ys = y;
                uint64_t lim = (r - k < 128) ? r - k : 128;
                for (uint64_t i = 0; i < lim; i++) {
                    y = _rho_step(y, c, n);
                    q = dvs_mulmod_u64(q, x > y ? x - y : y - x, n);
                }
                g = _gcd(q, n);
            }
            r <<= 1;
        }
        if (g == n) {
            do {
                ys = _rho_step(ys, c, n);
                g = _gcd(x > ys ? x - ys : ys - x, n);
            } while (g == 1);
        }
        if (g != n) return g;
    }
}

/* Prime factors of n, with repeats, appended to f[*nf] (at most 64). The
 * composites still to split wait on a stack: each split leaves two factors
 * of at least 2, so at most 64 are ever pending. */
static void _factor(uint64_t n, uint64_t *f, unsigned *nf) {
    uint64_t todo[64];
    unsigned nt = 0;
    if (n > 1) todo[nt++] = n;
    while (nt) {
        uint64_t m = todo[--nt];
        if (_is_prime(m)) { f[(*nf)++] = m; continue; }
        uint64_t d = _rho(m);
        todo[nt++] = d;
        todo[nt++] = m / d;
    }
}

static int _cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

uint32_t dvs_divisors(DvsDivisors *d, uint64_t n, uint32_t max) {
    if (n == 0) return 0;
    if (d->n == n && d->n_divs) return d->n_divs;
    d->n = 0; d->n_divs = 0;

    /* Prime factorisation: small primes by trial division, the rest by rho. */
    uint64_t f[64];
    unsigned nf = 0;
    uint64_t m = n;
    static const uint64_t SP[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47 };
    for (unsigned i = 0; i < sizeof SP / sizeof SP[0]; i++) {
        while (m % SP[i] == 0) { f[nf++] = SP[i]; m /= SP[i]; }
    }
    _factor(m, f, &nf);
    qsort(f, nf, sizeof f[0], _cmp_u64);

    /* Count the divisors: the product of (exponent + 1). */
    uint64_t count = 1;
    for (unsigned i = 0; i < nf;) {
        unsigned j = i;
        while (j < nf && f[j] == f[i]) j++;
        count *= (uint64_t)(j - i + 1);
        i = j;
    }
    if (count > max) return 0;
    if (count > d->cap) {
        uint64_t *p = (uint64_t *)realloc(d->divs, (size_t)count * sizeof(uint64_t));
        if (!p) return 0;
        d->divs = p;
        d->cap = (uint32_t)count;
    }
    /* Expand prime by prime: each power of p times every divisor so far. */
    uint32_t nd = 1;
    d->divs[0] = 1;
    for (unsigned i = 0; i < nf;) {
        unsigned j = i;
        while (j < nf && f[j] == f[i]) j++;
        uint32_t base = nd;
        uint64_t pk = 1;
        for (unsigned e = 0; e < j - i; e++) {
            pk *= f[i];
            for (uint32_t k = 0; k < base; k++) d->divs[nd++] = d->divs[k] * pk;
        }
        i = j;
    }
    qsort(d->divs, nd, sizeof(uint64_t), _cmp_u64);
    d->n = n;
    d->n_divs = nd;
    return nd;
}

void dvs_divisors_free(DvsDivisors *d) {
    free(d->divs);
    memset(d, 0, sizeof *d);
}

uint32_t dvs_divisors_lower(const DvsDivisors *d, uint64_t x) {
    uint32_t lo = 0, hi = d->n_divs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d->divs[mid] < x) lo = mid + 1; else hi = mid;
    }
    return lo;
}

uint32_t dvs_divisors_upper(const DvsDivisors *d, uint64_t x) {
    uint32_t lo = 0, hi = d->n_divs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d->divs[mid] <= x) lo = mid + 1; else hi = mid;
    }
    return lo;
}
