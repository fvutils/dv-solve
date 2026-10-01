#ifndef ZSP_I128_H
#define ZSP_I128_H
/*
 * Portable wide / overflow-checked integer helpers.
 *
 * MSVC has neither __int128 nor the GCC/Clang __builtin_*_overflow family for
 * signed operands, and the release wheels are built with MSVC. Everything in
 * the solver that needs more than 64 bits, or an int64 overflow check, goes
 * through this header:
 *
 *   - the FAST path (GCC/Clang with __int128): __int128 and the builtins;
 *   - the PORTABLE path (MSVC, or any build with -DZSP_NO_INT128): plain C on
 *     64-bit halves, using the x64 MSVC intrinsics where they exist.
 *
 * ZSP_NO_INT128 (CMake option of the same name) forces the portable path so
 * it can be built and tested on Linux.
 */
#include <stdint.h>

#if defined(__SIZEOF_INT128__) && !defined(ZSP_NO_INT128)
#define ZSP_HAVE_INT128 1
#endif

#if defined(ZSP_HAVE_INT128) && (defined(__GNUC__) || defined(__clang__))
#define ZSP_HAVE_OVF_BUILTINS 1
#endif

#if defined(_MSC_VER) && defined(_M_X64) && !defined(ZSP_HAVE_INT128)
#include <intrin.h>
#define ZSP_HAVE_MSVC_X64 1
#endif

/* ------------------------------------------------------------------ */
/* Checked int64 arithmetic: return 1 on OVERFLOW (like the builtins). */
/* *r is the wrapped result either way.                                 */
/* ------------------------------------------------------------------ */

static inline int zsp_add_i64_ovf(int64_t a, int64_t b, int64_t *r) {
#if defined(ZSP_HAVE_OVF_BUILTINS)
    return __builtin_add_overflow(a, b, r);
#else
    *r = (int64_t)((uint64_t)a + (uint64_t)b);
    return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
#endif
}

static inline int zsp_sub_i64_ovf(int64_t a, int64_t b, int64_t *r) {
#if defined(ZSP_HAVE_OVF_BUILTINS)
    return __builtin_sub_overflow(a, b, r);
#else
    *r = (int64_t)((uint64_t)a - (uint64_t)b);
    return (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
#endif
}

static inline int zsp_add_u64_ovf(uint64_t a, uint64_t b, uint64_t *r) {
    *r = a + b;
    return *r < a;
}

/* Full 64x64 -> 128-bit unsigned product, as (hi, lo). */
static inline void zsp_mul_u64_wide(uint64_t a, uint64_t b,
                                    uint64_t *hi, uint64_t *lo) {
#if defined(ZSP_HAVE_INT128)
    unsigned __int128 p = (unsigned __int128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
#elif defined(ZSP_HAVE_MSVC_X64)
    *lo = _umul128(a, b, hi);
#else
    uint64_t al = a & 0xFFFFFFFFu, ah = a >> 32;
    uint64_t bl = b & 0xFFFFFFFFu, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
    *lo = (mid << 32) | (ll & 0xFFFFFFFFu);
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}

static inline int zsp_mul_u64_ovf(uint64_t a, uint64_t b, uint64_t *r) {
    uint64_t hi;
    zsp_mul_u64_wide(a, b, &hi, r);
    return hi != 0;
}

static inline int zsp_mul_i64_ovf(int64_t a, int64_t b, int64_t *r) {
#if defined(ZSP_HAVE_OVF_BUILTINS)
    return __builtin_mul_overflow(a, b, r);
#else
    uint64_t ua = a < 0 ? 0u - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? 0u - (uint64_t)b : (uint64_t)b;
    uint64_t hi, lo;
    zsp_mul_u64_wide(ua, ub, &hi, &lo);
    int neg = (a < 0) != (b < 0);
    *r = (int64_t)(neg ? 0u - lo : lo);
    if (hi != 0) return 1;
    return neg ? (lo > (uint64_t)1 << 63) : (lo > (uint64_t)INT64_MAX);
#endif
}

/* v * 2^s for 0 <= s <= 63: return 1 on overflow. */
static inline int zsp_shl_i64_ovf(int64_t v, int64_t s, int64_t *r) {
    if (s <= 0) { *r = v; return 0; }
    if (s >= 63) {
        *r = (v == -1) ? INT64_MIN : 0;
        return !(v == 0 || v == -1);
    }
    return zsp_mul_i64_ovf(v, (int64_t)1 << s, r);
}

/* (a * b) mod m, for a, b < m. */
static inline uint64_t zsp_mulmod_u64(uint64_t a, uint64_t b, uint64_t m) {
#if defined(ZSP_HAVE_INT128)
    return (uint64_t)((unsigned __int128)a * b % m);
#elif defined(ZSP_HAVE_MSVC_X64)
    unsigned __int64 hi, rem;
    unsigned __int64 lo = _umul128(a, b, &hi);
    _udiv128(hi, lo, m, &rem);   /* a, b < m: the quotient fits 64 bits */
    return rem;
#else
    /* Shift-and-add with modular doubling; never overflows for a, b < m. */
    uint64_t r = 0;
    a %= m;
    while (b) {
        if (b & 1u) r = (r >= m - a) ? r - (m - a) : r + a;
        a = (a >= m - a) ? a - (m - a) : a + a;
        b >>= 1;
    }
    return r;
#endif
}

/* ------------------------------------------------------------------ */
/* A signed 128-bit value, for comparisons that must not round.        */
/* ------------------------------------------------------------------ */

#if defined(ZSP_HAVE_INT128)
typedef __int128 zsp_i128;
static inline zsp_i128 zsp_i128_from_i64(int64_t v)  { return (zsp_i128)v; }
static inline zsp_i128 zsp_i128_from_u64(uint64_t v) { return (zsp_i128)v; }
static inline int zsp_i128_lt(zsp_i128 a, zsp_i128 b) { return a < b; }
#else
typedef struct { int64_t hi; uint64_t lo; } zsp_i128;
static inline zsp_i128 zsp_i128_from_i64(int64_t v) {
    zsp_i128 r; r.hi = v < 0 ? -1 : 0; r.lo = (uint64_t)v; return r;
}
static inline zsp_i128 zsp_i128_from_u64(uint64_t v) {
    zsp_i128 r; r.hi = 0; r.lo = v; return r;
}
static inline int zsp_i128_lt(zsp_i128 a, zsp_i128 b) {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
}
#endif

static inline int zsp_i128_le(zsp_i128 a, zsp_i128 b) { return !zsp_i128_lt(b, a); }

#endif /* ZSP_I128_H */
