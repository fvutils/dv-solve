/* Known bits in the CDCL search: a tnum (known-bits mask and their values)
 * per variable beside its bounds, propagated through the bitwise operators.
 *
 * A variable's tnum lives in an immutable record on the dynamic stack,
 * ctx->kb[v] (NULL: the static bits ctx->sk_mask/sk_val only). A change
 * pushes a new record and a TRAIL_BITS entry whose old_value is the previous
 * pointer, so backtrack and the analyzer's rewind restore it.
 *
 * Learnt clauses stay bound literals: each record carries a set of bound
 * literals that held when it was made and imply its whole tnum (the bounds
 * of the propagator's variables and their own records' literals). An
 * explanation that rests on known bits cites those literals.
 *
 * Bit patterns are the variable's two's-complement pattern of its width;
 * value order for a signed variable is pattern order with the sign bit
 * flipped. Design: docs/design/simbench/engines/work/kb/design.md in the
 * verilator-dvs-integ packet. DVS_KB=0 turns it off. */
#ifndef DVS_KB_H
#define DVS_KB_H

#include <stdint.h>
#include "dvs_lcg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KB_MAX_LITS  32u
#define KB_NOEXPL    0xFFFFFFFFu   /* KbRec.n: grounding overflowed */

typedef struct KbRec {
    struct KbRec *prev;   /* the record this one replaced */
    uint64_t      mask;   /* known bits */
    uint64_t      val;    /* their values (bits outside mask are 0) */
    uint32_t      n;      /* lits[n], or KB_NOEXPL */
    uint32_t      _pad;
    Literal       lits[];
} KbRec;

typedef enum { KB_AND, KB_OR, KB_XOR, KB_NOT } KbOp;

/* Is the domain on for this context (DVS_KB, a variable array)? */
int kb_enabled(dvs_ctx_t *ctx);

/* After a bitwise propagator's bounds rules: tnum transfer between
 * r = a op b (b ignored for KB_NOT), store new bits, round the bounds to
 * the tnums. PROP_CONFLICT on a clash or an empty domain. */
PropResult kb_bitwise(dvs_ctx_t *ctx, KbOp op, uint32_t r, uint32_t a, uint32_t b);

/* Explanation for any narrowing of a propagator over vars[n] made with the
 * domain on: every variable's bounds plus its record's literals. -1 when a
 * record is unexplainable or the literals do not fit. */
int kb_explain(dvs_ctx_t *ctx, const uint32_t *vars, uint32_t n, Explanation *out);

/* The literals of var's record (n = 0 when none), or -1 if unexplainable. */
int kb_lits(const dvs_ctx_t *ctx, uint32_t var, const Literal **lits, uint32_t *n);

/* Drop every record (a reset that discards the trail). */
void kb_clear(dvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DVS_KB_H */
