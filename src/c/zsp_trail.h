#ifndef ZSP_TRAIL_H
#define ZSP_TRAIL_H

#include <stddef.h>
#include <stdint.h>
#include "zsp_stack.h"   /* zsp_stack_mark_t */

#ifdef __cplusplus
extern "C" {
#endif

/* forward declaration — SolveCtx is defined in zsp_ctx.h */
typedef struct SolveCtx SolveCtx;

/* ------------------------------------------------------------------ */
/* TrailKind — what kind of domain change was recorded                 */
/* ------------------------------------------------------------------ */
typedef enum {
    TRAIL_LB   = 0,  /* lower bound tightened  */
    TRAIL_UB   = 1,  /* upper bound tightened  */
    TRAIL_HOLE = 2,  /* value removed from interior (hole) */
} TrailKind;

/* ------------------------------------------------------------------ */
/* TrailValueSize — size of old_value stored in the entry             */
/* ------------------------------------------------------------------ */
typedef enum {
    TRAIL_VALUE_32   = 0,  /* old_value is a 32-bit quantity */
    TRAIL_VALUE_64   = 1,  /* old_value is a 64-bit quantity */
    TRAIL_VALUE_WIDE = 2,  /* future: wide (>64-bit) variable */
} TrailValueSize;

/* ------------------------------------------------------------------ */
/* TrailEntry — singly-linked list node allocated in the dynamic stack */
/*                                                                     */
/* Entries are allocated newest-first; prev points toward older        */
/* entries.  Backtracking walks the chain from trail_top backward.     */
/* ------------------------------------------------------------------ */
typedef struct TrailEntry {
    struct TrailEntry *prev;          /* 8: previous entry (or NULL)   */
    uint32_t           var_id;        /* 4: which variable changed     */
    uint8_t            kind;          /* 1: TrailKind                  */
    uint8_t            value_size;    /* 1: TrailValueSize             */
    uint16_t           decision_level;/* 2: level at time of recording */
    int64_t            old_value;     /* 8: old bound (LB or UB)       */
    uint32_t           prop_ref;      /* 4: pool offset of causing propagator
                                       *    EXPR_NULL = decision (no propagator) */
    uint32_t           _te_pad;       /* 4: alignment padding          */
} TrailEntry;                         /* 32 bytes                      */

/* ------------------------------------------------------------------ */
/* LevelMark — checkpoint recorded at each decision push              */
/* ------------------------------------------------------------------ */
typedef struct {
    zsp_stack_mark_t   stack_mark;   /* dynamic-stack checkpoint       */
    TrailEntry        *trail_top;    /* trail_top at time of push      */
    uint64_t           trail_count;  /* trail_count at time of push    */
} LevelMark;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * Record a decision-level push.
 *
 * Must be called before making any propagation changes at the new level.
 * Saves the current dynamic-stack position and trail_top into
 * ctx->level_marks[ctx->decision_level], then increments decision_level.
 */
void trail_push_level(SolveCtx *ctx);

/**
 * Record (and apply) a lower-bound tightening on `var_id`.
 *
 * Saves the current lower bound, applies `new_lb`, and pushes a trail entry.
 *
 * @return 0 on success, -1 if the dynamic stack is full or tier-2 (unsupported).
 */
int trail_record_lb(SolveCtx *ctx, uint32_t var_id, int64_t new_lb);

/**
 * Record (and apply) an upper-bound tightening on `var_id`.
 */
int trail_record_ub(SolveCtx *ctx, uint32_t var_id, int64_t new_ub);

/**
 * Record (and mark) a value removal (hole) on `var_id`.
 *
 * Phase 5: records the trail entry; hole restoration is a no-op until
 * Phase 6 implements the hole list.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int trail_record_hole(SolveCtx *ctx, uint32_t var_id, int64_t removed_val);

/**
 * Backtrack to `target_level`.
 *
 * Undoes all trail entries recorded after the push at `target_level`,
 * then pops the dynamic stack to recover trail memory.
 * Sets ctx->decision_level = target_level.
 *
 * Requires target_level <= ctx->decision_level.
 */
void trail_backtrack(SolveCtx *ctx, uint32_t target_level);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_TRAIL_H */
