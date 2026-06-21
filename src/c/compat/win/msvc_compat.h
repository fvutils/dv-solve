/* MSVC compatibility shim, force-included (/FI) into every translation unit
 * when building with MSVC. Provides the GCC/Clang builtins and POSIX stat
 * helpers the dv-solve and kissat sources rely on. POSIX functions live in the
 * unistd.h and sys-header shims under this directory; this header only covers
 * things that must be visible everywhere (builtins) or are macros (S_IS*).
 *
 * Targets x86_64 (the only Windows arch we build); the bit-scan / 128-bit
 * multiply intrinsics used below are x64-only.
 */
#ifndef ZSP_MSVC_COMPAT_H
#define ZSP_MSVC_COMPAT_H
#ifdef _MSC_VER

#include <intrin.h>
#include <malloc.h>
#include <stdint.h>
#include <sys/stat.h>   /* _S_IFMT/_S_IFDIR before we define S_IS* */

/* kissat decorates some declarations with GCC __attribute__((format(...))) and
 * ((always_inline)). MSVC has no such syntax; drop the decorations entirely.
 * (Force-included before any header, and MSVC's own headers never use it.) */
#ifndef __attribute__
#define __attribute__(x)
#endif

/* ---- count-leading/trailing-zeros & friends -------------------------- */
static __forceinline int zsp__clz32(unsigned x) {
    unsigned long i;
    return _BitScanReverse(&i, x) ? (int)(31 - i) : 32;
}
static __forceinline int zsp__clz64(unsigned long long x) {
    unsigned long i;
    return _BitScanReverse64(&i, x) ? (int)(63 - i) : 64;
}
static __forceinline int zsp__ctz32(unsigned x) {
    unsigned long i;
    return _BitScanForward(&i, x) ? (int)i : 32;
}
static __forceinline int zsp__ctz64(unsigned long long x) {
    unsigned long i;
    return _BitScanForward64(&i, x) ? (int)i : 64;
}

#define __builtin_clz(x)   zsp__clz32((unsigned)(x))
#define __builtin_clzl(x)  zsp__clz32((unsigned)(x))   /* LLP64: long is 32-bit */
#define __builtin_clzll(x) zsp__clz64((unsigned long long)(x))
#define __builtin_ctz(x)   zsp__ctz32((unsigned)(x))
#define __builtin_ctzll(x) zsp__ctz64((unsigned long long)(x))
#define __builtin_prefetch(addr, ...) ((void)(addr))
#define __builtin_alloca(n) _alloca(n)

/* ---- checked arithmetic ---------------------------------------------- *
 * Only the unsigned 32-bit add (zsp_arena.c) and unsigned 64-bit multiply
 * (zsp_prop_templates.c) are reached here; the int64 multiply elsewhere is
 * already guarded with #ifdef __GNUC__. _Generic flags any unexpected type at
 * compile time (no default case). Needs C11 -- CMAKE_C_STANDARD is 11. */
static __forceinline int zsp__add_ovf_u32(uint32_t a, uint32_t b, uint32_t *r) {
    *r = a + b; return *r < a;
}
static __forceinline int zsp__add_ovf_u64(uint64_t a, uint64_t b, uint64_t *r) {
    *r = a + b; return *r < a;
}
static __forceinline int zsp__mul_ovf_u64(uint64_t a, uint64_t b, uint64_t *r) {
    unsigned __int64 hi;
    *r = _umul128(a, b, &hi);
    return hi != 0;
}
#define __builtin_add_overflow(a, b, r) _Generic((r), \
    uint32_t *: zsp__add_ovf_u32,                     \
    uint64_t *: zsp__add_ovf_u64)((a), (b), (r))
#define __builtin_mul_overflow(a, b, r) _Generic((r), \
    uint64_t *: zsp__mul_ovf_u64)((a), (b), (r))

/* ---- stat() mode test macros MSVC lacks ------------------------------ */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

#endif /* _MSC_VER */
#endif /* ZSP_MSVC_COMPAT_H */
