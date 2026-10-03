#ifndef DVS_PROBLEM_H
#define DVS_PROBLEM_H

#include <stddef.h>
#include <stdint.h>
#include "dv_solve.h"  /* public types: dvs_expr_t, dvs_binop_t, ... */
#include "dvs_pool.h"   /* dvs_pool_t, EXPR_NULL */

#ifdef __cplusplus
extern "C" {
#endif


/* ------------------------------------------------------------------ */
/* ExprKind — discriminates expression node types                      */
/* ------------------------------------------------------------------ */
typedef enum {
    EXPR_CONST    = 0,  /* integer constant          */
    EXPR_VAR      = 1,  /* variable reference by id  */
    EXPR_BINARY   = 2,  /* binary operation          */
    EXPR_UNARY    = 3,  /* unary operation           */
    EXPR_ITE      = 4,  /* if-then-else              */
    EXPR_IN_RANGE = 5,  /* value in [lo, hi]         */
    EXPR_IN_SET   = 6,  /* value in {v0, v1, ...}    */
    EXPR_EXTEND   = 7,  /* zero/sign extend          */
    EXPR_EXTRACT  = 8,  /* bit-slice extract         */
    EXPR_CONCAT   = 9,  /* bit concatenation         */
    EXPR_SUM      = 10, /* n-ary sum: r == v0+v1+...+vN */
    EXPR_COUNTONES = 11, /* popcount: r == countones(x)  */
    EXPR_CLOG2    = 12, /* r == ceil(log2(x))           */
    EXPR_ARRAY_SELECT = 13, /* r = base[index]              */
    EXPR_IN_RANGES = 14, /* value in [lo0,hi0] U [lo1,hi1] U ... */
    EXPR_SV_CAST  = 15, /* INTERNAL: explicit width/signedness conversion,
                         * produced only by SV elaboration (dvs_sv.h) */
    EXPR_CAST     = 16, /* cast to a type: SV `T'(x)`  */
} ExprKind;



/* ------------------------------------------------------------------ */
/* Expression node structs                                             */
/*                                                                     */
/* Every node begins with `ExprKind kind` so any dvs_expr_t can be cast  */
/* to `ExprKind *` to read its type before casting to the specific     */
/* struct.                                                             */
/* ------------------------------------------------------------------ */

/** Integer literal
 *
 * `width` == 0 is an UNSIZED literal, typed like a SystemVerilog integer
 * literal: 32 bits, signed, when the value fits in int32; otherwise 32 bits
 * unsigned when `is_signed` is 0 and the value fits in uint32; otherwise 64
 * bits (signed if the value is negative or `is_signed` is set). See
 * dvs_sv_const_type().
 *
 * `width` > 0 is a SIZED literal of exactly that many bits; `is_signed`
 * selects its signedness and `value` supplies its low `width` bits (the
 * SMT-LIB front end emits these, e.g. #x03 is an 8-bit 3). */
typedef struct {
    ExprKind kind;       /* EXPR_CONST                           */
    uint8_t  is_signed;  /* non-zero for signed interpretation   */
    uint8_t  width;      /* 0 = unsized, else the literal's width */
    uint8_t  _pad[2];
    int64_t  value;      /* bit pattern / signed value           */
} ExprConst;

/** Variable reference */
typedef struct {
    ExprKind kind;      /* EXPR_VAR   */
    uint32_t var_id;    /* 0-based variable index               */
} ExprVar;

/** Binary operation */
typedef struct {
    ExprKind kind;  /* EXPR_BINARY */
    dvs_binop_t    op;
    dvs_expr_t  lhs;
    dvs_expr_t  rhs;
} ExprBinary;

/** Unary operation */
typedef struct {
    ExprKind kind;    /* EXPR_UNARY */
    dvs_unop_t  op;
    dvs_expr_t  operand;
} ExprUnary;

/** If-then-else */
typedef struct {
    ExprKind kind;    /* EXPR_ITE */
    dvs_expr_t  cond;
    dvs_expr_t  then_e;
    dvs_expr_t  else_e;
} ExprITE;

/** Range membership: value in [lo, hi] */
typedef struct {
    ExprKind kind;    /* EXPR_IN_RANGE */
    dvs_expr_t  value;
    dvs_expr_t  lo;
    dvs_expr_t  hi;
} ExprInRange;

/**
 * Set membership: value in {elems[0], elems[1], ..., elems[n_elems-1]}.
 *
 * n_elems dvs_expr_t values are stored immediately after this struct in pool
 * memory.  Use expr_in_set_elems() to obtain the pointer.
 */
typedef struct {
    ExprKind kind;     /* EXPR_IN_SET */
    dvs_expr_t  value;
    uint32_t n_elems;
} ExprInSet;

/**
 * Multi-range membership: value in [lo0,hi0] U [lo1,hi1] U ... U [lo{n-1},hi{n-1}].
 *
 * 2*n_ranges dvs_expr_t values follow this struct in pool memory: the lo refs
 * (n_ranges of them) then the hi refs (n_ranges of them). Use
 * expr_in_ranges_los()/expr_in_ranges_his() to obtain the pointers.
 */
typedef struct {
    ExprKind kind;     /* EXPR_IN_RANGES */
    dvs_expr_t  value;
    uint32_t n_ranges;
} ExprInRanges;

/** Zero / sign extend operand from `from_bits` to `to_bits` bits */
typedef struct {
    ExprKind kind;        /* EXPR_EXTEND      */
    uint8_t  sign_extend; /* 0=zero, 1=sign   */
    uint8_t  from_bits;   /* source width     */
    uint8_t  to_bits;     /* destination width */
    uint8_t  _pad;
    dvs_expr_t  operand;
} ExprExtend;

/** INTERNAL explicit conversion (EXPR_SV_CAST), never built by a front end.
 *
 * Takes the low `from_bits` bits of `operand`, extends them to `to_bits`
 * (sign-extending when `sign_extend`), and reads the result as a signed value
 * when `dst_signed`, else unsigned. SV elaboration inserts it where a
 * SystemVerilog context changes a value's meaning -- a signed operand used in
 * an unsigned context -- so the engines downstream never have to infer that
 * conversion themselves. Same layout as ExprExtend. */
typedef struct {
    ExprKind kind;        /* EXPR_SV_CAST     */
    uint8_t  sign_extend; /* 0=zero, 1=sign   */
    uint8_t  from_bits;   /* source width     */
    uint8_t  to_bits;     /* destination width */
    uint8_t  dst_signed;  /* result signedness */
    dvs_expr_t  operand;
} ExprSvCast;

/** A cast to the type (`to_bits`, `to_signed`): SystemVerilog's
 * `T'(operand)` for an integral T, and the cast of PSS and similar languages.
 *
 * The operand is an assignment-like context (IEEE 1800 6.24.1, 11.8.1): when
 * `to_bits` is wider than the operand's own width it is propagated into the
 * operand, so `cast(a * b, 64, 0)` multiplies 32-bit `a` and `b` at 64 bits;
 * the operand's signedness stays its own. The value is then truncated to
 * `to_bits`, or extended by the operand's signedness, and read as signed when
 * `to_signed`. The node's own type is (`to_bits`, `to_signed`). SV
 * elaboration rewrites it into extracts and EXPR_SV_CAST nodes, so no engine
 * sees it. */
typedef struct {
    ExprKind kind;        /* EXPR_CAST        */
    uint8_t  to_bits;     /* result width     */
    uint8_t  to_signed;   /* result signedness */
    uint8_t  _pad[2];
    dvs_expr_t  operand;
} ExprCast;

/** Bit-slice extract: result = operand[hi_bit:lo_bit] */
typedef struct {
    ExprKind kind;     /* EXPR_EXTRACT */
    uint8_t  hi_bit;
    uint8_t  lo_bit;
    uint8_t  _pad[2];
    dvs_expr_t  operand;
} ExprExtract;


/** Bit concatenation: result = {hi, lo}
 *  lo_width is the bit width of the lo operand. */
typedef struct {
    ExprKind kind;       /* EXPR_CONCAT  */
    uint8_t  lo_width;   /* width of lo operand in bits */
    uint8_t  _pad[3];
    dvs_expr_t  hi;
    dvs_expr_t  lo;
} ExprConcat;

/** N-ary sum: result == var_ids[0] + var_ids[1] + ... + var_ids[n-1].
 *  n_vars uint32_t var_ids follow immediately after this struct in pool. */
typedef struct {
    ExprKind kind;       /* EXPR_SUM */
    dvs_expr_t  result;     /* result variable dvs_expr_t   */
    uint32_t n_vars;     /* number of summand variables */
    /* uint32_t var_ids[n_vars] follow in pool */
} ExprSum;

/** Popcount: result == number of 1-bits in operand. */
typedef struct {
    ExprKind kind;       /* EXPR_COUNTONES */
    dvs_expr_t  result;     /* result variable dvs_expr_t  */
    dvs_expr_t  operand;    /* input variable dvs_expr_t   */
} ExprCountones;

/** Ceil-log2: result == ceil(log2(operand)). operand must be > 0. */
typedef struct {
    ExprKind kind;       /* EXPR_CLOG2   */
    dvs_expr_t  result;     /* result variable dvs_expr_t  */
    dvs_expr_t  operand;    /* input variable dvs_expr_t   */
} ExprClog2;

/** Array element select: result = base_var[index].
 *  Elements are contiguous variables: base_var_id .. base_var_id+n_elems-1.
 *  The compiler lowers this into an ITE chain for small n_elems. */
typedef struct {
    ExprKind kind;          /* EXPR_ARRAY_SELECT         */
    uint32_t base_var_id;   /* first element variable ID */
    uint32_t n_elems;       /* number of elements        */
    dvs_expr_t  result;        /* result variable dvs_expr_t   */
    dvs_expr_t  index;         /* index expression dvs_expr_t  */
} ExprArraySelect;

/* ------------------------------------------------------------------ */
/* Variable / Constraint / Source specifications                       */
/* ------------------------------------------------------------------ */

/**
 * VarSpec — declares one variable.
 * Lives in the problem pool; linked together via `next`.
 */
typedef struct {
    dvs_expr_t  next;       /* next VarSpec, or EXPR_NULL */
    uint32_t var_id;     /* 0-based index              */
    uint8_t  width;      /* bit width (1–64)           */
    uint8_t  is_signed;  /* non-zero = signed          */
    uint8_t  is_aux;     /* non-zero = compiler-generated aux (never a
                          * search decision; determined by propagation) */
    uint8_t  _pad;
    int64_t  lo;         /* initial lower bound        */
    int64_t  hi;         /* initial upper bound        */
} VarSpec;

/**
 * ConstraintSpec — one constraint expression tree.
 * Lives in the problem pool; linked via `next`.
 */
typedef struct {
    dvs_expr_t  next;  /* next ConstraintSpec, or EXPR_NULL */
    dvs_expr_t  root;  /* root dvs_expr_t of the expression    */
    uint32_t constraint_id; /* user-visible constraint ID (contradiction analysis) */
} ConstraintSpec;

/**
 * SourceSpec — a group of variables to randomize together.
 *
 * `n_vars` uint32_t variable IDs are stored immediately after this
 * struct in pool memory.  Use source_spec_vars() to obtain the pointer.
 */
typedef struct {
    dvs_expr_t  next;    /* next SourceSpec, or EXPR_NULL */
    uint32_t n_vars;  /* number of variable IDs that follow */
} SourceSpec;

/**
 * AllDiffSpec -- declares an all-different constraint over a set of vars.
 * n_vars uint32_t variable IDs follow immediately in pool memory.
 */
typedef struct {
    dvs_expr_t  next;       /* next AllDiffSpec, or EXPR_NULL */
    uint32_t n_vars;     /* number of variable IDs that follow */
} AllDiffSpec;


/**
 * SoftSpec -- a soft (relaxable) constraint with a priority.
 * Higher priority value = lower priority (relaxed first on conflict).
 */
typedef struct {
    dvs_expr_t  next;       /* next SoftSpec, or EXPR_NULL */
    dvs_expr_t  root;       /* root dvs_expr_t of the constraint expression */
    uint32_t priority;   /* 0 = highest priority, larger = relaxed first */
    uint32_t constraint_id; /* original hard constraint ID (contradiction analysis) */
} SoftSpec;


/**
 * DistSpec -- declares a distribution constraint on a variable.
 * n_entries dvs_dist_entry_t values follow immediately in pool memory.
 * Use dist_spec_entries() to obtain the pointer.
 */
typedef struct {
    dvs_expr_t  next;       /* next DistSpec, or EXPR_NULL */
    uint32_t var_id;     /* variable this distribution applies to   */
    uint32_t n_entries;  /* number of dvs_dist_entry_t items that follow   */
} DistSpec;


/* ------------------------------------------------------------------ */
/* dvs_problem_t                                                        */
/*                                                                     */
/* The caller supplies a buffer; solve_problem_init() places this      */
/* struct at the front and initialises a dvs_pool_t for all expression */
/* and spec data immediately after it.                                 */
/*                                                                     */
/* All dvs_expr_t values are offsets from &sp->pool (i.e. from the start  */
/* of the embedded pool, NOT from the start of the buffer).           */
/* ------------------------------------------------------------------ */
struct dvs_problem_s {
    uint32_t   n_vars;            /* number of variables added         */
    uint32_t   n_constraints;     /* number of constraints added       */
    uint32_t   n_sources;         /* number of source groups added     */
    dvs_expr_t    vars_head;         /* head of VarSpec linked list       */
    dvs_expr_t    constraints_head;  /* head of ConstraintSpec linked list */
    dvs_expr_t    sources_head;      /* head of SourceSpec linked list    */
    uint32_t   n_alldiffs;         /* number of AllDifferent constraints */
    dvs_expr_t    allDiff_head;       /* head of AllDiffSpec linked list    */
    uint32_t   n_softs;            /* number of soft constraints         */
    dvs_expr_t    softs_head;         /* head of SoftSpec linked list       */
    uint32_t   n_dists;            /* number of distribution constraints */
    dvs_expr_t    dists_head;         /* head of DistSpec linked list       */
    uint32_t   next_constraint_id; /* auto-incrementing constraint ID counter */
    uint32_t   flags;             /* DVS_PROBLEM_F_* */
    dvs_pool_t pool;              /* MUST be last field                */
    /* pool data region follows immediately in the same buffer         */
};

/** The problem's expressions are already EXPLICIT: every constant is sized
 * and every operator's operands already share the width and signedness the
 * operator works at (SMT-LIB bit-vector semantics). SV elaboration (dvs_sv.h)
 * is skipped for such a problem. Set by the SMT-LIB2 front end. */
#define DVS_PROBLEM_F_EXPLICIT  0x1u

/** Convert a pool offset (dvs_expr_t) to a real pointer. */
#define POOL_PTR(sp, ref)  dvs_pool_ptr(&(sp)->pool, (ref))

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * Initialise a dvs_problem_t in a caller-supplied buffer.
 *
 * @param buf       Suitably aligned buffer (e.g. from malloc / block_alloc).
 * @param buf_size  Total size of `buf`.
 * @return  Pointer to the initialised problem (== buf), or NULL on error.
 */
dvs_problem_t *solve_problem_init(void *buf, size_t buf_size);

/**
 * Initialise with sizing hints (n_vars/constraints/sources are ignored
 * in Phase 3 — the pool is flat; hints will be used in Phase 4 for
 * static-segment pre-allocation).
 */
dvs_problem_t *solve_problem_init_sized(void *buf, size_t buf_size,
                                       uint32_t n_vars,
                                       uint32_t n_constraints,
                                       uint32_t n_sources);

/**
 * Reset the problem for reuse.  Clears all counts, linked-list heads,
 * and resets the pool (all previous ExprRefs become invalid).
 */
void solve_problem_reset(dvs_problem_t *sp);

/**
 * No-op for caller-managed buffers.  Provided for symmetry with future
 * heap-managed variants; safe to call unconditionally.
 */
void solve_problem_destroy(dvs_problem_t *sp);

/* ------------------------------------------------------------------ */
/* Expression builders — return EXPR_NULL on pool overflow             */
/* ------------------------------------------------------------------ */

dvs_expr_t expr_const(dvs_problem_t *sp, int64_t value, uint8_t is_signed);
/** A sized constant of `width` bits (see ExprConst). width 0 == expr_const. */
dvs_expr_t expr_const_sized(dvs_problem_t *sp, int64_t value, uint8_t is_signed,
                         uint8_t width);
dvs_expr_t expr_var(dvs_problem_t *sp, uint32_t var_id);
dvs_expr_t expr_binary(dvs_problem_t *sp, dvs_binop_t op, dvs_expr_t lhs, dvs_expr_t rhs);
dvs_expr_t expr_unary(dvs_problem_t *sp, dvs_unop_t op, dvs_expr_t operand);
dvs_expr_t expr_ite(dvs_problem_t *sp, dvs_expr_t cond, dvs_expr_t then_e, dvs_expr_t else_e);
dvs_expr_t expr_in_range(dvs_problem_t *sp, dvs_expr_t value, dvs_expr_t lo, dvs_expr_t hi);

/**
 * Build an in-set node.
 *
 * @param n_elems  Number of elements in `elems`.
 * @param elems    Array of dvs_expr_t values (the allowed set members).
 */
dvs_expr_t expr_in_set(dvs_problem_t *sp, dvs_expr_t value,
                    uint32_t n_elems, const dvs_expr_t *elems);

dvs_expr_t expr_extend(dvs_problem_t *sp, dvs_expr_t operand,
                    uint8_t from_bits, uint8_t to_bits, uint8_t sign_extend);
dvs_expr_t expr_extract(dvs_problem_t *sp, dvs_expr_t operand,
                     uint8_t hi_bit, uint8_t lo_bit);
/** A cast of `operand` to (`to_bits`, `to_signed`); see ExprCast. */
dvs_expr_t expr_cast(dvs_problem_t *sp, dvs_expr_t operand,
                  uint8_t to_bits, uint8_t to_signed);

dvs_expr_t expr_concat(dvs_problem_t *sp, dvs_expr_t hi, dvs_expr_t lo,
                    uint8_t lo_width);

/** Build an N-ary sum expression: result == sum of var_ids[].
 *  @param result   dvs_expr_t of the result variable.
 *  @param n_vars   Number of summand variable ExprRefs.
 *  @param var_refs Array of dvs_expr_t values for summand variables. */
dvs_expr_t expr_sum(dvs_problem_t *sp, dvs_expr_t result,
                 uint32_t n_vars, const dvs_expr_t *var_refs);

/** Build a countones (popcount) expression: result == popcount(operand). */
dvs_expr_t expr_countones(dvs_problem_t *sp, dvs_expr_t result, dvs_expr_t operand);

/** Build a clog2 expression: result == ceil(log2(operand)). */
dvs_expr_t expr_clog2(dvs_problem_t *sp, dvs_expr_t result, dvs_expr_t operand);

/** Build an array-select expression: result = base[index].
 *  @param base_var_id  First element variable ID (elements are contiguous).
 *  @param n_elems      Number of array elements.
 *  @param result       dvs_expr_t for the result variable.
 *  @param index        dvs_expr_t for the index expression. */
dvs_expr_t expr_array_select(dvs_problem_t *sp, uint32_t base_var_id,
                          uint32_t n_elems, dvs_expr_t result, dvs_expr_t index);

/* ------------------------------------------------------------------ */
/* Problem builders                                                    */
/* ------------------------------------------------------------------ */

/** Add a variable; returns dvs_expr_t to its VarSpec, or EXPR_NULL. */
dvs_expr_t problem_add_var(dvs_problem_t *sp, uint32_t var_id,
                        uint8_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi);

/** Add a constraint; returns dvs_expr_t to its ConstraintSpec, or EXPR_NULL. */
dvs_expr_t problem_add_constraint(dvs_problem_t *sp, dvs_expr_t root);

/**
 * Add a source group; returns dvs_expr_t to its SourceSpec, or EXPR_NULL.
 *
 * @param n_vars   Number of variable IDs.
 * @param var_ids  Array of variable IDs.
 */
dvs_expr_t problem_add_source(dvs_problem_t *sp,
                           uint32_t n_vars, const uint32_t *var_ids);


/**
 * Add an AllDifferent constraint over the given variable IDs.
 * @param n_vars   Number of variable IDs.
 * @param var_ids  Array of variable IDs.
 * @return dvs_expr_t to the AllDiffSpec, or EXPR_NULL on overflow.
 */
dvs_expr_t problem_add_all_different(dvs_problem_t *sp,
                                  uint32_t n_vars, const uint32_t *var_ids);

/**
 * Add a soft (relaxable) constraint.
 * @param root     dvs_expr_t of the constraint expression root.
 * @param priority Priority (0 = highest, larger = relaxed first on conflict).
 * @return dvs_expr_t to the SoftSpec, or EXPR_NULL on overflow.
 */
dvs_expr_t problem_add_soft_constraint(dvs_problem_t *sp, dvs_expr_t root,
                                    uint32_t priority);

/**
 * Add a distribution constraint on a variable.
 * @param var_id     Variable ID this distribution applies to.
 * @param n_entries  Number of dvs_dist_entry_t items.
 * @param entries    Array of dvs_dist_entry_t values (copied into the pool).
 * @return dvs_expr_t to the DistSpec, or EXPR_NULL on overflow.
 */
dvs_expr_t problem_add_dist(dvs_problem_t *sp, uint32_t var_id,
                         uint32_t n_entries, const dvs_dist_entry_t *entries);

/** Return a pointer to the entry array of a DistSpec node. */
dvs_dist_entry_t *dist_spec_entries(dvs_problem_t *sp, dvs_expr_t dist_ref);

/* ------------------------------------------------------------------ */
/* Access helpers for variable-length nodes                            */
/* ------------------------------------------------------------------ */

/**
 * Return a pointer to the element array of an EXPR_IN_SET node.
 * The returned pointer is valid until the problem is reset or destroyed.
 */
dvs_expr_t *expr_in_set_elems(dvs_problem_t *sp, dvs_expr_t set_ref);

/** Lo / hi dvs_expr_t arrays of an EXPR_IN_RANGES node (n_ranges each). */
dvs_expr_t *expr_in_ranges_los(dvs_problem_t *sp, dvs_expr_t ref);
dvs_expr_t *expr_in_ranges_his(dvs_problem_t *sp, dvs_expr_t ref);

/**
 * Return a pointer to the variable-ID array of a SourceSpec node.
 */
uint32_t *source_spec_vars(dvs_problem_t *sp, dvs_expr_t src_ref);

/**
 * Return a pointer to the embedded pool base (&sp->pool).
 * Useful for ctypes tests that need to compute POOL_PTR manually.
 */
void *solve_problem_pool_base(dvs_problem_t *sp);

#ifdef __cplusplus
}
#endif

#endif /* DVS_PROBLEM_H */
