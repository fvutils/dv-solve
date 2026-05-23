#ifndef ZSP_PROBLEM_H
#define ZSP_PROBLEM_H

#include <stddef.h>
#include <stdint.h>
#include "zsp_pool.h"   /* zsp_pool_t, EXPR_NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* ExprRef — 32-bit offset into the SolveProblem's embedded pool.      */
/* EXPR_NULL (0xFFFFFFFF) is the null/overflow sentinel (from pool.h). */
/* ------------------------------------------------------------------ */
typedef uint32_t ExprRef;

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
} ExprKind;

/* ------------------------------------------------------------------ */
/* BinOp — binary operator codes                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    BIN_ADD = 0, BIN_SUB,  BIN_MUL,  BIN_DIV,  BIN_MOD,
    BIN_BAND,    BIN_BOR,  BIN_BXOR, BIN_LSHIFT, BIN_RSHIFT,
    BIN_EQ,      BIN_NEQ,  BIN_LT,   BIN_LTE,
    BIN_GT,      BIN_GTE,
    BIN_AND,     BIN_OR,
} BinOp;

/* ------------------------------------------------------------------ */
/* UnaryOp — unary operator codes                                      */
/* ------------------------------------------------------------------ */
typedef enum {
    UN_NEG    = 0,  /* arithmetic negation   */
    UN_NOT    = 1,  /* logical NOT           */
    UN_INVERT = 2,  /* bitwise invert (~)    */
} UnaryOp;

/* ------------------------------------------------------------------ */
/* Expression node structs                                             */
/*                                                                     */
/* Every node begins with `ExprKind kind` so any ExprRef can be cast  */
/* to `ExprKind *` to read its type before casting to the specific     */
/* struct.                                                             */
/* ------------------------------------------------------------------ */

/** Integer literal */
typedef struct {
    ExprKind kind;       /* EXPR_CONST                           */
    uint8_t  is_signed;  /* non-zero for signed interpretation   */
    uint8_t  _pad[3];
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
    BinOp    op;
    ExprRef  lhs;
    ExprRef  rhs;
} ExprBinary;

/** Unary operation */
typedef struct {
    ExprKind kind;    /* EXPR_UNARY */
    UnaryOp  op;
    ExprRef  operand;
} ExprUnary;

/** If-then-else */
typedef struct {
    ExprKind kind;    /* EXPR_ITE */
    ExprRef  cond;
    ExprRef  then_e;
    ExprRef  else_e;
} ExprITE;

/** Range membership: value in [lo, hi] */
typedef struct {
    ExprKind kind;    /* EXPR_IN_RANGE */
    ExprRef  value;
    ExprRef  lo;
    ExprRef  hi;
} ExprInRange;

/**
 * Set membership: value in {elems[0], elems[1], ..., elems[n_elems-1]}.
 *
 * n_elems ExprRef values are stored immediately after this struct in pool
 * memory.  Use expr_in_set_elems() to obtain the pointer.
 */
typedef struct {
    ExprKind kind;     /* EXPR_IN_SET */
    ExprRef  value;
    uint32_t n_elems;
} ExprInSet;

/** Zero / sign extend operand from `from_bits` to `to_bits` bits */
typedef struct {
    ExprKind kind;        /* EXPR_EXTEND      */
    uint8_t  sign_extend; /* 0=zero, 1=sign   */
    uint8_t  from_bits;   /* source width     */
    uint8_t  to_bits;     /* destination width */
    uint8_t  _pad;
    ExprRef  operand;
} ExprExtend;

/** Bit-slice extract: result = operand[hi_bit:lo_bit] */
typedef struct {
    ExprKind kind;     /* EXPR_EXTRACT */
    uint8_t  hi_bit;
    uint8_t  lo_bit;
    uint8_t  _pad[2];
    ExprRef  operand;
} ExprExtract;


/** Bit concatenation: result = {hi, lo}
 *  lo_width is the bit width of the lo operand. */
typedef struct {
    ExprKind kind;       /* EXPR_CONCAT  */
    uint8_t  lo_width;   /* width of lo operand in bits */
    uint8_t  _pad[3];
    ExprRef  hi;
    ExprRef  lo;
} ExprConcat;

/** N-ary sum: result == var_ids[0] + var_ids[1] + ... + var_ids[n-1].
 *  n_vars uint32_t var_ids follow immediately after this struct in pool. */
typedef struct {
    ExprKind kind;       /* EXPR_SUM */
    ExprRef  result;     /* result variable ExprRef   */
    uint32_t n_vars;     /* number of summand variables */
    /* uint32_t var_ids[n_vars] follow in pool */
} ExprSum;

/** Popcount: result == number of 1-bits in operand. */
typedef struct {
    ExprKind kind;       /* EXPR_COUNTONES */
    ExprRef  result;     /* result variable ExprRef  */
    ExprRef  operand;    /* input variable ExprRef   */
} ExprCountones;

/** Ceil-log2: result == ceil(log2(operand)). operand must be > 0. */
typedef struct {
    ExprKind kind;       /* EXPR_CLOG2   */
    ExprRef  result;     /* result variable ExprRef  */
    ExprRef  operand;    /* input variable ExprRef   */
} ExprClog2;

/** Array element select: result = base_var[index].
 *  Elements are contiguous variables: base_var_id .. base_var_id+n_elems-1.
 *  The compiler lowers this into an ITE chain for small n_elems. */
typedef struct {
    ExprKind kind;          /* EXPR_ARRAY_SELECT         */
    uint32_t base_var_id;   /* first element variable ID */
    uint32_t n_elems;       /* number of elements        */
    ExprRef  result;        /* result variable ExprRef   */
    ExprRef  index;         /* index expression ExprRef  */
} ExprArraySelect;

/* ------------------------------------------------------------------ */
/* Variable / Constraint / Source specifications                       */
/* ------------------------------------------------------------------ */

/**
 * VarSpec — declares one variable.
 * Lives in the problem pool; linked together via `next`.
 */
typedef struct {
    ExprRef  next;       /* next VarSpec, or EXPR_NULL */
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
    ExprRef  next;  /* next ConstraintSpec, or EXPR_NULL */
    ExprRef  root;  /* root ExprRef of the expression    */
    uint32_t constraint_id; /* user-visible constraint ID (contradiction analysis) */
} ConstraintSpec;

/**
 * SourceSpec — a group of variables to randomize together.
 *
 * `n_vars` uint32_t variable IDs are stored immediately after this
 * struct in pool memory.  Use source_spec_vars() to obtain the pointer.
 */
typedef struct {
    ExprRef  next;    /* next SourceSpec, or EXPR_NULL */
    uint32_t n_vars;  /* number of variable IDs that follow */
} SourceSpec;

/**
 * AllDiffSpec -- declares an all-different constraint over a set of vars.
 * n_vars uint32_t variable IDs follow immediately in pool memory.
 */
typedef struct {
    ExprRef  next;       /* next AllDiffSpec, or EXPR_NULL */
    uint32_t n_vars;     /* number of variable IDs that follow */
} AllDiffSpec;


/**
 * SoftSpec -- a soft (relaxable) constraint with a priority.
 * Higher priority value = lower priority (relaxed first on conflict).
 */
typedef struct {
    ExprRef  next;       /* next SoftSpec, or EXPR_NULL */
    ExprRef  root;       /* root ExprRef of the constraint expression */
    uint32_t priority;   /* 0 = highest priority, larger = relaxed first */
    uint32_t constraint_id; /* original hard constraint ID (contradiction analysis) */
} SoftSpec;

/**
 * DistEntry -- one range/weight pair in a distribution constraint.
 */
typedef struct {
    int64_t  lo;           /* lower bound of the range               */
    int64_t  hi;           /* upper bound of the range               */
    uint32_t weight;       /* := weight (per-value) or :/ weight     */
    uint8_t  is_per_value; /* 1 = := (weight per value), 0 = :/ (weight divided across range) */
    uint8_t  _dpad[3];
} DistEntry;

/**
 * DistSpec -- declares a distribution constraint on a variable.
 * n_entries DistEntry values follow immediately in pool memory.
 * Use dist_spec_entries() to obtain the pointer.
 */
typedef struct {
    ExprRef  next;       /* next DistSpec, or EXPR_NULL */
    uint32_t var_id;     /* variable this distribution applies to   */
    uint32_t n_entries;  /* number of DistEntry items that follow   */
} DistSpec;


/* ------------------------------------------------------------------ */
/* SolveProblem                                                        */
/*                                                                     */
/* The caller supplies a buffer; solve_problem_init() places this      */
/* struct at the front and initialises a zsp_pool_t for all expression */
/* and spec data immediately after it.                                 */
/*                                                                     */
/* All ExprRef values are offsets from &sp->pool (i.e. from the start  */
/* of the embedded pool, NOT from the start of the buffer).           */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t   n_vars;            /* number of variables added         */
    uint32_t   n_constraints;     /* number of constraints added       */
    uint32_t   n_sources;         /* number of source groups added     */
    ExprRef    vars_head;         /* head of VarSpec linked list       */
    ExprRef    constraints_head;  /* head of ConstraintSpec linked list */
    ExprRef    sources_head;      /* head of SourceSpec linked list    */
    uint32_t   n_alldiffs;         /* number of AllDifferent constraints */
    ExprRef    allDiff_head;       /* head of AllDiffSpec linked list    */
    uint32_t   n_softs;            /* number of soft constraints         */
    ExprRef    softs_head;         /* head of SoftSpec linked list       */
    uint32_t   n_dists;            /* number of distribution constraints */
    ExprRef    dists_head;         /* head of DistSpec linked list       */
    uint32_t   next_constraint_id; /* auto-incrementing constraint ID counter */
    zsp_pool_t pool;              /* MUST be last field                */
    /* pool data region follows immediately in the same buffer         */
} SolveProblem;

/** Convert a pool offset (ExprRef) to a real pointer. */
#define POOL_PTR(sp, ref)  zsp_pool_ptr(&(sp)->pool, (ref))

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * Initialise a SolveProblem in a caller-supplied buffer.
 *
 * @param buf       Suitably aligned buffer (e.g. from malloc / block_alloc).
 * @param buf_size  Total size of `buf`.
 * @return  Pointer to the initialised problem (== buf), or NULL on error.
 */
SolveProblem *solve_problem_init(void *buf, size_t buf_size);

/**
 * Initialise with sizing hints (n_vars/constraints/sources are ignored
 * in Phase 3 — the pool is flat; hints will be used in Phase 4 for
 * static-segment pre-allocation).
 */
SolveProblem *solve_problem_init_sized(void *buf, size_t buf_size,
                                       uint32_t n_vars,
                                       uint32_t n_constraints,
                                       uint32_t n_sources);

/**
 * Reset the problem for reuse.  Clears all counts, linked-list heads,
 * and resets the pool (all previous ExprRefs become invalid).
 */
void solve_problem_reset(SolveProblem *sp);

/**
 * No-op for caller-managed buffers.  Provided for symmetry with future
 * heap-managed variants; safe to call unconditionally.
 */
void solve_problem_destroy(SolveProblem *sp);

/* ------------------------------------------------------------------ */
/* Expression builders — return EXPR_NULL on pool overflow             */
/* ------------------------------------------------------------------ */

ExprRef expr_const(SolveProblem *sp, int64_t value, uint8_t is_signed);
ExprRef expr_var(SolveProblem *sp, uint32_t var_id);
ExprRef expr_binary(SolveProblem *sp, BinOp op, ExprRef lhs, ExprRef rhs);
ExprRef expr_unary(SolveProblem *sp, UnaryOp op, ExprRef operand);
ExprRef expr_ite(SolveProblem *sp, ExprRef cond, ExprRef then_e, ExprRef else_e);
ExprRef expr_in_range(SolveProblem *sp, ExprRef value, ExprRef lo, ExprRef hi);

/**
 * Build an in-set node.
 *
 * @param n_elems  Number of elements in `elems`.
 * @param elems    Array of ExprRef values (the allowed set members).
 */
ExprRef expr_in_set(SolveProblem *sp, ExprRef value,
                    uint32_t n_elems, const ExprRef *elems);

ExprRef expr_extend(SolveProblem *sp, ExprRef operand,
                    uint8_t from_bits, uint8_t to_bits, uint8_t sign_extend);
ExprRef expr_extract(SolveProblem *sp, ExprRef operand,
                     uint8_t hi_bit, uint8_t lo_bit);

ExprRef expr_concat(SolveProblem *sp, ExprRef hi, ExprRef lo,
                    uint8_t lo_width);

/** Build an N-ary sum expression: result == sum of var_ids[].
 *  @param result   ExprRef of the result variable.
 *  @param n_vars   Number of summand variable ExprRefs.
 *  @param var_refs Array of ExprRef values for summand variables. */
ExprRef expr_sum(SolveProblem *sp, ExprRef result,
                 uint32_t n_vars, const ExprRef *var_refs);

/** Build a countones (popcount) expression: result == popcount(operand). */
ExprRef expr_countones(SolveProblem *sp, ExprRef result, ExprRef operand);

/** Build a clog2 expression: result == ceil(log2(operand)). */
ExprRef expr_clog2(SolveProblem *sp, ExprRef result, ExprRef operand);

/** Build an array-select expression: result = base[index].
 *  @param base_var_id  First element variable ID (elements are contiguous).
 *  @param n_elems      Number of array elements.
 *  @param result       ExprRef for the result variable.
 *  @param index        ExprRef for the index expression. */
ExprRef expr_array_select(SolveProblem *sp, uint32_t base_var_id,
                          uint32_t n_elems, ExprRef result, ExprRef index);

/* ------------------------------------------------------------------ */
/* Problem builders                                                    */
/* ------------------------------------------------------------------ */

/** Add a variable; returns ExprRef to its VarSpec, or EXPR_NULL. */
ExprRef problem_add_var(SolveProblem *sp, uint32_t var_id,
                        uint8_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi);

/** Add a constraint; returns ExprRef to its ConstraintSpec, or EXPR_NULL. */
ExprRef problem_add_constraint(SolveProblem *sp, ExprRef root);

/**
 * Add a source group; returns ExprRef to its SourceSpec, or EXPR_NULL.
 *
 * @param n_vars   Number of variable IDs.
 * @param var_ids  Array of variable IDs.
 */
ExprRef problem_add_source(SolveProblem *sp,
                           uint32_t n_vars, const uint32_t *var_ids);


/**
 * Add an AllDifferent constraint over the given variable IDs.
 * @param n_vars   Number of variable IDs.
 * @param var_ids  Array of variable IDs.
 * @return ExprRef to the AllDiffSpec, or EXPR_NULL on overflow.
 */
ExprRef problem_add_all_different(SolveProblem *sp,
                                  uint32_t n_vars, const uint32_t *var_ids);

/**
 * Add a soft (relaxable) constraint.
 * @param root     ExprRef of the constraint expression root.
 * @param priority Priority (0 = highest, larger = relaxed first on conflict).
 * @return ExprRef to the SoftSpec, or EXPR_NULL on overflow.
 */
ExprRef problem_add_soft_constraint(SolveProblem *sp, ExprRef root,
                                    uint32_t priority);

/**
 * Add a distribution constraint on a variable.
 * @param var_id     Variable ID this distribution applies to.
 * @param n_entries  Number of DistEntry items.
 * @param entries    Array of DistEntry values (copied into the pool).
 * @return ExprRef to the DistSpec, or EXPR_NULL on overflow.
 */
ExprRef problem_add_dist(SolveProblem *sp, uint32_t var_id,
                         uint32_t n_entries, const DistEntry *entries);

/** Return a pointer to the entry array of a DistSpec node. */
DistEntry *dist_spec_entries(SolveProblem *sp, ExprRef dist_ref);

/* ------------------------------------------------------------------ */
/* Access helpers for variable-length nodes                            */
/* ------------------------------------------------------------------ */

/**
 * Return a pointer to the element array of an EXPR_IN_SET node.
 * The returned pointer is valid until the problem is reset or destroyed.
 */
ExprRef *expr_in_set_elems(SolveProblem *sp, ExprRef set_ref);

/**
 * Return a pointer to the variable-ID array of a SourceSpec node.
 */
uint32_t *source_spec_vars(SolveProblem *sp, ExprRef src_ref);

/**
 * Return a pointer to the embedded pool base (&sp->pool).
 * Useful for ctypes tests that need to compute POOL_PTR manually.
 */
void *solve_problem_pool_base(SolveProblem *sp);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_PROBLEM_H */
