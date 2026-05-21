#ifndef ZSP_VARIABLE_H
#define ZSP_VARIABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Variable flags                                                      */
/* ------------------------------------------------------------------ */
#define VAR_SIGNED  0x01u   /* signed integer domain                  */
#define VAR_RANDC   0x02u   /* cyclic-random (randc) semantics        */
#define VAR_STATE   0x04u   /* non-rand state variable                */
#define VAR_TIER1   0x08u   /* bounds stored in WideBounds64 (33-64b) */
#define VAR_TIER2   0x10u   /* bounds stored in WideBoundsN  (>64b)   */

/* Convenience: tier-0 when neither TIER1 nor TIER2 is set (width ≤ 32) */
#define VAR_IS_TIER0(f)  (((f) & (VAR_TIER1|VAR_TIER2)) == 0)
#define VAR_IS_TIER1(f)  (((f) & VAR_TIER1) != 0)
#define VAR_IS_TIER2(f)  (((f) & VAR_TIER2) != 0)

/* ------------------------------------------------------------------ */
/* Variable — 16 bytes, one entry in the flat Variable[] array        */
/*                                                                     */
/* For tier-0 (width ≤ 32):                                           */
/*   lo / hi hold the 32-bit current bounds directly.                 */
/*   holes_offset = 0 means no holes; non-zero is a pool offset to a  */
/*   sorted int32_t hole list (Phase 6+).                             */
/*                                                                     */
/* For tier-1 (33 ≤ width ≤ 64):                                      */
/*   lo / hi are unused (set to 0).                                   */
/*   holes_offset is a pool offset to a WideBounds64 struct.          */
/*                                                                     */
/* For tier-2 (width > 64):                                           */
/*   lo / hi are unused (set to 0).                                   */
/*   holes_offset is a pool offset to a WideBoundsN struct.           */
/* ------------------------------------------------------------------ */
typedef struct {
    int32_t   lo;            /* current lower bound (tier-0 only)     */
    int32_t   hi;            /* current upper bound (tier-0 only)     */
    uint32_t  holes_offset;  /* pool offset to holes/wide-bounds      */
    uint16_t  width;         /* bit-width (1–N)                       */
    uint8_t   flags;         /* VAR_* flag bits                       */
    uint8_t   _pad;
} Variable;                  /* 16 bytes */

/* ------------------------------------------------------------------ */
/* WideBounds64 — bounds for tier-1 variables (33–64 bits)            */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t  lo;
    int64_t  hi;
} WideBounds64;

/* ------------------------------------------------------------------ */
/* WideBoundsN — bounds for tier-2 variables (> 64 bits)              */
/*                                                                     */
/* n_limbs little-endian uint64_t lo-limbs are stored immediately     */
/* after this struct, followed by n_limbs hi-limbs.                   */
/*                                                                     */
/* Bounds are stored as unsigned integers; the sign of the variable   */
/* is recorded in the Variable flags (VAR_SIGNED).                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t  n_limbs;   /* ceil(width / 64) */
    uint32_t  _pad;
    /* uint64_t lo_limbs[n_limbs] immediately follow */
    /* uint64_t hi_limbs[n_limbs] immediately follow lo_limbs */
} WideBoundsN;

#ifdef __cplusplus
}
#endif

#endif /* ZSP_VARIABLE_H */
