/*
 * Unit tests for the SMT-LIB2 frontend (Phase 5 array support).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"
#include "smt2/smt2_frontend.h"

static int _passed = 0;
static int _failed = 0;

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        _failed++; return; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d: %s == %d, expected %d\n", \
                __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
        _failed++; return; \
    } \
} while (0)

#define RUN(fn) do { \
    int prev = _failed; \
    fn(); \
    if (_failed == prev) { _passed++; printf("  PASS  %s\n", #fn); } \
    else printf("  FAIL  %s\n", #fn); \
} while (0)

/* ------------------------------------------------------------------ */
/* Helper: dispatch a string of SMT2 commands                         */
/* ------------------------------------------------------------------ */

static int _run(Smt2Frontend *fe, const char *input) {
    Smt2Lexer lex;
    SexprArena arena;
    smt2_lexer_init(&lex, input, strlen(input));
    sexpr_arena_init(&arena, 0);
    int rc = 0;
    for (;;) {
        Sexpr *s = sexpr_parse(&lex, &arena);
        if (!s) break;
        rc = smt2_frontend_dispatch(fe, s);
        if (rc != 0) break;
    }
    sexpr_arena_destroy(&arena);
    return rc;
}

/* Dispatch all commands, capture stdout into buf. */
static void _run_capture(Smt2Frontend *fe, const char *input,
                         char *buf, size_t bufsz) {
    FILE *tmp = tmpfile();
    FILE *saved = fe->out;
    fe->out = tmp;
    _run(fe, input);
    fe->out = saved;
    rewind(tmp);
    size_t n = fread(buf, 1, bufsz - 1, tmp);
    buf[n] = '\0';
    fclose(tmp);
}

/* ------------------------------------------------------------------ */
/* Task 5.0: accept QF_AUFBV / QF_ABV                                 */
/* ------------------------------------------------------------------ */

static void test_set_logic_qf_aufbv(void) {
    Smt2Frontend fe;
    smt2_frontend_init(&fe, stderr, stderr);
    int rc = _run(&fe, "(set-logic QF_AUFBV)");
    ASSERT_EQ_INT(rc, 0);
    smt2_frontend_destroy(&fe);
}

static void test_set_logic_qf_abv(void) {
    Smt2Frontend fe;
    smt2_frontend_init(&fe, stderr, stderr);
    int rc = _run(&fe, "(set-logic QF_ABV)");
    ASSERT_EQ_INT(rc, 0);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.1: declare-const Array                                       */
/* ------------------------------------------------------------------ */

static void test_declare_array_const_2x4(void) {
    Smt2Frontend fe;
    smt2_frontend_init(&fe, stderr, stderr);
    int rc = _run(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))");
    ASSERT_EQ_INT(rc, 0);
    /* Should have created 4 element vars: a[0], a[1], a[2], a[3] */
    ASSERT_EQ_INT(fe.n_array_vars, 1);
    ASSERT_EQ_INT(fe.array_vars[0].value->n_elems, 4);
    ASSERT_EQ_INT(fe.array_vars[0].sort.data_width, 4);
    /* 4 element BV vars added to var table */
    /* Find a[0] in the var table */
    uint32_t found = 0;
    for (uint32_t i = 0; i < fe.n_vars; i++) {
        if (strcmp(fe.vars[i].name, "a[0]") == 0) { found++; }
        if (strcmp(fe.vars[i].name, "a[3]") == 0) { found++; }
    }
    ASSERT_EQ_INT(found, 2);
    smt2_frontend_destroy(&fe);
}

static void test_declare_array_addr_too_wide(void) {
    Smt2Frontend fe;
    FILE *err_f = tmpfile();
    smt2_frontend_init(&fe, stderr, err_f);
    /* M=12 > MAX_ARRAY_ADDR_BITS=10: should print an error but continue */
    int rc = _run(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const big (Array (_ BitVec 12) (_ BitVec 8)))");
    ASSERT_EQ_INT(rc, 0);    /* session continues */
    ASSERT_EQ_INT(fe.n_array_vars, 0);  /* var not registered */
    /* Check error was printed */
    rewind(err_f);
    char buf[256] = {0};
    (void)fread(buf, 1, sizeof(buf)-1, err_f);
    ASSERT_TRUE(strstr(buf, "exceeds limit") != NULL);
    fclose(err_f);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: select                                                    */
/* ------------------------------------------------------------------ */

static void test_array_select_const_index(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* Write known value into slot 1 via as-const then store, then select */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        /* Force a[1] == #b0101 by asserting equality */
        "(assert (= (select a (_ bv1 2)) (_ bv5 4)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "sat") != NULL);
    smt2_frontend_destroy(&fe);
}

static void test_array_select_symbolic_idx(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* Use a symbolic index variable */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        "(declare-const i (_ BitVec 2))"
        /* a[i] must be nonzero -- satisfiable */
        "(assert (not (= (select a i) (_ bv0 4))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "sat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: store                                                     */
/* ------------------------------------------------------------------ */

static void test_array_store_const_index(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* store 5 at slot 1; select slot 1 must equal 5 (not 7) -- UNSAT */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        "(define-fun a2 () (Array (_ BitVec 2) (_ BitVec 4))"
        "  (store a (_ bv1 2) (_ bv5 4)))"
        /* select at slot 1 == 5 always; assert it equals 7 -> UNSAT */
        "(assert (= (select a2 (_ bv1 2)) (_ bv7 4)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

static void test_array_store_symbolic_idx(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* store at symbolic index, then select at same symbolic index == value */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        "(declare-const i (_ BitVec 2))"
        "(declare-const v (_ BitVec 4))"
        "(define-fun a2 () (Array (_ BitVec 2) (_ BitVec 4))"
        "  (store a i v))"
        "(assert (not (= (select a2 i) v)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: (as const ...)                                            */
/* ------------------------------------------------------------------ */

static void test_array_as_const(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* All elements are zero; select at any index must be 0 */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const i (_ BitVec 2))"
        "(define-fun zeros () (Array (_ BitVec 2) (_ BitVec 4))"
        "  ((as const (Array (_ BitVec 2) (_ BitVec 4))) (_ bv0 4)))"
        "(assert (not (= (select zeros i) (_ bv0 4))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: rewrite R1 -- select(store(A,I,V),I) → V                 */
/* ------------------------------------------------------------------ */

static void test_array_select_of_store_R1(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* store 3 at slot 1; R1 rewrite: select at same slot returns 3, not 7 */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        /* force select result == 7, but store wrote 3 -> UNSAT */
        "(assert (= (select (store a (_ bv1 2) (_ bv3 4)) (_ bv1 2)) (_ bv7 4)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: rewrite R2 -- select(store(A,I,V),J) with I!=J → select A J */
/* ------------------------------------------------------------------ */

static void test_array_select_of_store_R2(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* store at slot 0, read at slot 1 -- should see original a[1], not v */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        "(declare-const v (_ BitVec 4))"
        /* force a[1] = 7; force v = 3; store v at 0; read at 1 should be 7 */
        "(assert (= (select a (_ bv1 2)) (_ bv7 4)))"
        "(assert (= v (_ bv3 4)))"
        "(assert (not (= (select (store a (_ bv0 2) v) (_ bv1 2)) (_ bv7 4))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: array equality                                            */
/* ------------------------------------------------------------------ */

static void test_array_eq_elementwise(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* Two arrays that are forced equal must have equal elements */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        "(declare-const b (Array (_ BitVec 2) (_ BitVec 4)))"
        /* a == b, so a[2] == b[2] */
        "(assert (= a b))"
        "(assert (not (= (select a (_ bv2 2)) (select b (_ bv2 2)))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: ite over arrays                                           */
/* ------------------------------------------------------------------ */

static void test_array_ite(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* ite(cond, a, b): when cond, a[0] is returned; else b[0] */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        "(declare-const b (Array (_ BitVec 2) (_ BitVec 4)))"
        "(declare-const cond Bool)"
        /* When cond, select(ite(cond,a,b), 0) == a[0]; must hold */
        "(assert cond)"
        "(assert (not (= (select (ite cond a b) (_ bv0 2)) (select a (_ bv0 2)))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 5.5: declare-datatypes stub                                    */
/* ------------------------------------------------------------------ */

static void test_declare_datatypes_record_of_bv(void) {
    Smt2Frontend fe;
    smt2_frontend_init(&fe, stderr, stderr);
    /* Phase 6.5.3: single-constructor record-of-BV is supported. */
    int rc = _run(&fe, "(declare-datatypes ((T 0)) (((mk-t (fld (_ BitVec 8))))))\n");
    ASSERT_EQ_INT(rc, 0);
    /* Sort name 'T' should be registered as opaque, 'fld' as a sort-fun */
    ASSERT_TRUE(fe.n_sort_names >= 1);
    ASSERT_TRUE(fe.n_sort_funs  >= 1);
    /* Round-trip: declare an instance, read its field, assert a value */
    int rc2 = _run(&fe,
        "(declare-const x T)"
        "(assert (= (fld x) (_ bv5 8)))"
        "(check-sat)");
    ASSERT_EQ_INT(rc2, 0);
    smt2_frontend_destroy(&fe);
}

static void test_declare_datatypes_rejects_sum(void) {
    Smt2Frontend fe;
    FILE *err_f = tmpfile();
    smt2_frontend_init(&fe, stderr, err_f);
    /* Two constructors → sum type, rejected. */
    int rc = _run(&fe,
        "(declare-datatypes ((Color 0)) (((red) (green) (blue))))");
    ASSERT_EQ_INT(rc, -1);
    rewind(err_f);
    char buf[256] = {0};
    (void)fread(buf, 1, sizeof(buf)-1, err_f);
    ASSERT_TRUE(strstr(buf, "single-constructor") != NULL);
    fclose(err_f);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Integration: full regfile_simple round-trip                        */
/* ------------------------------------------------------------------ */

static void test_regfile_simple_unsat(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* write w_data to w_addr; read back at same address must equal w_data */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const rf (Array (_ BitVec 3) (_ BitVec 8)))"
        "(declare-const w_addr (_ BitVec 3))"
        "(declare-const w_data (_ BitVec 8))"
        "(define-fun rf2 () (Array (_ BitVec 3) (_ BitVec 8))"
        "  (store rf w_addr w_data))"
        "(assert (not (= (select rf2 w_addr) w_data)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

static void test_regfile_simple_sat(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* bug: read from wrong address -- should be sat (counterexample exists) */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const rf (Array (_ BitVec 3) (_ BitVec 8)))"
        "(declare-const w_addr (_ BitVec 3))"
        "(declare-const r_addr (_ BitVec 3))"
        "(declare-const w_data (_ BitVec 8))"
        "(define-fun rf2 () (Array (_ BitVec 3) (_ BitVec 8))"
        "  (store rf w_addr w_data))"
        /* addresses differ, initial rf[r_addr] != w_data: find counterexample */
        "(assert (not (= w_addr r_addr)))"
        "(assert (not (= (select rf r_addr) w_data)))"
        "(assert (not (= (select rf2 r_addr) w_data)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "sat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* define-fun with array parameter                                     */
/* ------------------------------------------------------------------ */

static void test_array_in_define_fun_param(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* define-fun taking an array parameter: read_at_1 returns element 1 */
    _run_capture(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))"
        "(define-fun read_at_1 ((arr (Array (_ BitVec 2) (_ BitVec 4)))) (_ BitVec 4)"
        "  (select arr (_ bv1 2)))"
        /* store 9 at slot 1; read_at_1 must return 9, not 6 -> UNSAT */
        "(assert (= (read_at_1 (store a (_ bv1 2) (_ bv9 4))) (_ bv6 4)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 6.2: let support                                               */
/* ------------------------------------------------------------------ */

static void test_let_basic(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* let binds t to x (forced = 3); body asserts not(t = 3), i.e. not(x = 3) -> UNSAT */
    _run_capture(&fe,
        "(set-logic QF_BV)"
        "(declare-const x (_ BitVec 4))"
        "(assert (= x (_ bv3 4)))"
        "(assert (not (let ((t x)) (= t (_ bv3 4)))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

static void test_let_shared_subexpr(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* t = constant 5; assert not(t = x) where x is also forced to 5 -> UNSAT */
    _run_capture(&fe,
        "(set-logic QF_BV)"
        "(declare-const x (_ BitVec 4))"
        "(assert (= x (_ bv5 4)))"
        "(assert (not (let ((t (_ bv5 4))) (= t x))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

static void test_let_shadowing(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* Inner y shadows outer y; inner y = 5, assert inner body not(y = 5) -> UNSAT */
    _run_capture(&fe,
        "(set-logic QF_BV)"
        "(declare-const x (_ BitVec 4))"
        "(assert (= x (_ bv3 4)))"
        "(assert (not (let ((y (_ bv3 4))) (let ((y (_ bv5 4))) (= y (_ bv5 4))))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

static void test_let_parallel_binding(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* Parallel swap: a=1, b=2; (let ((a b) (b a)) (bvadd a b)) = 1+2 = 3.
     * Assert result != 3 -> UNSAT. */
    _run_capture(&fe,
        "(set-logic QF_BV)"
        "(declare-const a (_ BitVec 4))"
        "(declare-const b (_ BitVec 4))"
        "(assert (= a (_ bv1 4)))"
        "(assert (= b (_ bv2 4)))"
        "(assert (not (= (let ((a b) (b a)) (bvadd a b)) (_ bv3 4))))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Task 6.4: declare-const Array under push/pop                       */
/* ------------------------------------------------------------------ */

static void test_array_push_pop_basic(void) {
    Smt2Frontend fe;
    smt2_frontend_init(&fe, stderr, stderr);
    int rc = _run(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 8)))"
        "(push 1)"
        "(declare-const b (Array (_ BitVec 2) (_ BitVec 8)))");
    ASSERT_EQ_INT(rc, 0);
    /* Two arrays declared, one inside push */
    ASSERT_EQ_INT(fe.n_array_vars, 2);
    ASSERT_EQ_INT(fe.push_depth, 1);
    /* Saved count = 1 (only `a` existed before push) */
    ASSERT_EQ_INT(fe.push_n_array_vars[0], 1);

    /* Pop: `b` should be cleaned up, `a` remains */
    rc = _run(&fe, "(pop 1)");
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(fe.n_array_vars, 1);
    ASSERT_EQ_INT(fe.push_depth, 0);
    ASSERT_TRUE(fe.array_vars[0].value != NULL);
    ASSERT_TRUE(fe.array_vars[1].value == NULL);
    smt2_frontend_destroy(&fe);
}

static void test_array_push_pop_repeated(void) {
    Smt2Frontend fe;
    smt2_frontend_init(&fe, stderr, stderr);
    int rc = _run(&fe,
        "(set-logic QF_AUFBV)"
        "(declare-const a (Array (_ BitVec 2) (_ BitVec 4)))");
    ASSERT_EQ_INT(rc, 0);
    /* 50 push/declare/pop cycles — leaks would be caught by ASan. */
    for (int i = 0; i < 50; i++) {
        rc = _run(&fe,
            "(push 1)"
            "(declare-const tmp (Array (_ BitVec 2) (_ BitVec 4)))"
            "(pop 1)");
        ASSERT_EQ_INT(rc, 0);
    }
    ASSERT_EQ_INT(fe.n_array_vars, 1);
    smt2_frontend_destroy(&fe);
}

static void test_let_malformed(void) {
    Smt2Frontend fe;
    FILE *err_f = tmpfile();
    smt2_frontend_init(&fe, stderr, err_f);
    /* Wrong arity: (let (a) body) -- binding list item is a symbol, not a pair */
    _run(&fe,
        "(set-logic QF_BV)"
        "(declare-const a (_ BitVec 8))"
        "(assert (let (a) a))");
    /* Translation failure is expected; check that a meaningful error was printed */
    rewind(err_f);
    char buf[256] = {0};
    (void)fread(buf, 1, sizeof(buf)-1, err_f);
    ASSERT_TRUE(strstr(buf, "error") != NULL || strstr(buf, "malformed") != NULL);
    fclose(err_f);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* Modular bvadd-with-constant (Phase 6 follow-up #1).                */
/* The non-modular bounds_add path produced unsound results on wrap;  */
/* the new propagator must report unsat when the wrap rules things out */
/* and sat when a valid wrap-yielding model exists.                   */
/* ------------------------------------------------------------------ */

static void test_bvadd_const_no_wrap_chain_unsat(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    _run_capture(&fe,
        "(set-logic QF_BV)"
        "(declare-const c0 (_ BitVec 8))"
        "(declare-const c1 (_ BitVec 8))"
        "(declare-const c2 (_ BitVec 8))"
        "(assert (= c0 #b00000000))"
        "(assert (= c1 (bvadd c0 ((_ zero_extend 7) #b1))))"
        "(assert (= c2 (bvadd c1 ((_ zero_extend 7) #b1))))"
        /* both c1, c2 < 256 since they are 8-bit; assertion forces one >= 256 */
        "(assert (or (bvuge ((_ zero_extend 1) c1) #b100000000)"
        "            (bvuge ((_ zero_extend 1) c2) #b100000000)))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "unsat") != NULL);
    smt2_frontend_destroy(&fe);
}

static void test_bvadd_const_wrap_to_zero_sat(void) {
    Smt2Frontend fe;
    char out[512];
    smt2_frontend_init(&fe, stderr, stderr);
    /* a + 1 == 0 (mod 256) is satisfiable with a = 255; the non-modular
     * propagator used to reject this as a contradiction. */
    _run_capture(&fe,
        "(set-logic QF_BV)"
        "(declare-const a (_ BitVec 8))"
        "(declare-const b (_ BitVec 8))"
        "(assert (= b (bvadd a #b00000001)))"
        "(assert (= b #b00000000))"
        "(check-sat)",
        out, sizeof(out));
    ASSERT_TRUE(strstr(out, "sat") != NULL);
    ASSERT_TRUE(strstr(out, "unsat") == NULL);
    smt2_frontend_destroy(&fe);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("test_smt2_frontend\n");

    RUN(test_set_logic_qf_aufbv);
    RUN(test_set_logic_qf_abv);
    RUN(test_declare_array_const_2x4);
    RUN(test_declare_array_addr_too_wide);
    RUN(test_array_select_const_index);
    RUN(test_array_select_symbolic_idx);
    RUN(test_array_store_const_index);
    RUN(test_array_store_symbolic_idx);
    RUN(test_array_as_const);
    RUN(test_array_select_of_store_R1);
    RUN(test_array_select_of_store_R2);
    RUN(test_array_eq_elementwise);
    RUN(test_array_ite);
    RUN(test_declare_datatypes_record_of_bv);
    RUN(test_declare_datatypes_rejects_sum);
    RUN(test_regfile_simple_unsat);
    RUN(test_regfile_simple_sat);
    RUN(test_array_in_define_fun_param);

    /* Task 6.2: let */
    RUN(test_let_basic);
    RUN(test_let_shared_subexpr);
    RUN(test_let_shadowing);
    RUN(test_let_parallel_binding);
    RUN(test_let_malformed);

    /* Task 6.4: declare-const Array under push/pop */
    RUN(test_array_push_pop_basic);
    RUN(test_array_push_pop_repeated);

    /* Phase 6 follow-up #1: modular bvadd-with-constant */
    RUN(test_bvadd_const_no_wrap_chain_unsat);
    RUN(test_bvadd_const_wrap_to_zero_sat);

    printf("\n%d passed, %d failed\n", _passed, _failed);
    return _failed ? 1 : 0;
}
