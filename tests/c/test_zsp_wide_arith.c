/* Width-64 ADD/SUB: every SOLVE_OK model must satisfy the constraint.
 *
 * Three defects, each of which returned SOLVE_OK with a model violating
 * `t == a op b` when the operands' true result lies outside int64:
 *
 *   - bounds_add_64 declined every tightening it could not compute in int64
 *     and had no other check, so once a and b were fixed with a + b >= 2^63
 *     nothing constrained t at all (it now checks feasibility exactly);
 *   - the decision heuristic computed domain sizes as `hi - lo` in int64,
 *     signed overflow for any span above INT64_MAX: undefined, and under -O2
 *     a model with a = b = 0 and an arbitrary t;
 *   - var_repr_max shifted 1 into bit 63 of an int64 for unsigned width 63.
 *
 * Expected values use SystemVerilog semantics, as compile does: the operation
 * is evaluated at the width of the context (64 here), so the result is
 * (a op b) mod 2^64 compared as a bit pattern. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zsp_problem.h"
#include "zsp_block_alloc.h"
#include "zsp_ctx.h"
#include "zsp_search.h"

#define CTX_BUF_SIZE (1u << 20)
#define N_SEEDS      40

static int failures = 0;

typedef struct {
    const char *name;
    int op;
    int aw, as_; int64_t alo, ahi;       /* both operands */
    int tw, ts;  int64_t tlo, thi;       /* result */
} Case;

static int in_domain(int64_t v, int is_signed, int64_t lo, int64_t hi) {
    if (is_signed) return v >= lo && v <= hi;
    return (uint64_t)v >= (uint64_t)lo && (uint64_t)v <= (uint64_t)hi;
}

static void run(const Case *c) {
    /* SolveProblem needs 8-byte alignment. */
    static uint64_t sp_buf[(1u << 16) / sizeof(uint64_t)];
    SolveProblem *sp = solve_problem_init(sp_buf, sizeof(sp_buf));
    problem_add_var(sp, 0, (uint16_t)c->aw, (uint8_t)c->as_, c->alo, c->ahi);
    problem_add_var(sp, 1, (uint16_t)c->aw, (uint8_t)c->as_, c->alo, c->ahi);
    problem_add_var(sp, 2, (uint16_t)c->tw, (uint8_t)c->ts, c->tlo, c->thi);
    problem_add_constraint(sp, expr_binary(sp, BIN_EQ, expr_var(sp, 2),
        expr_binary(sp, (BinOp)c->op, expr_var(sp, 0), expr_var(sp, 1))));

    uint8_t *buf = (uint8_t *)malloc(CTX_BUF_SIZE);
    int ok = 0, bad = 0, other = 0;
    for (int i = 0; i < N_SEEDS; i++) {
        void *ba = zsp_block_alloc_create(NULL, CTX_BUF_SIZE);
        SolveCtx *ctx = solver_create(buf, CTX_BUF_SIZE, ba);
        if (solver_compile(ctx, sp) != 0) {
            fprintf(stderr, "FAIL %s: compile\n", c->name);
            failures++;
            zsp_block_alloc_destroy(ba);
            break;
        }
        SolveOpts o;
        memset(&o, 0, sizeof(o));
        o.seed = (uint64_t)i + 1;
        SolveResult sr = solver_solve(ctx, &o);
        if (sr != SOLVE_OK) {
            other++;
        } else {
            uint64_t a = (uint64_t)solver_get_value(ctx, 0);
            uint64_t b = (uint64_t)solver_get_value(ctx, 1);
            int64_t  t = solver_get_value(ctx, 2);
            uint64_t want = c->op == BIN_ADD ? a + b : a - b;
            if ((uint64_t)t != want || !in_domain(t, c->ts, c->tlo, c->thi)) {
                if (!bad)
                    fprintf(stderr, "  %s seed %d: a=%" PRIu64 " b=%" PRIu64
                            " t=%" PRId64 "\n", c->name, i + 1, a, b, t);
                bad++;
            } else {
                ok++;
            }
        }
        zsp_block_alloc_destroy(ba);
    }
    free(buf);
    /* A timeout is incompleteness, not a wrong answer -- but these are all
     * easy, so require most seeds to produce a model. */
    if (bad || ok < N_SEEDS / 2) {
        fprintf(stderr, "FAIL %s: ok=%d bad=%d other=%d\n", c->name, ok, bad, other);
        failures++;
    } else {
        printf("PASS %s (ok=%d other=%d)\n", c->name, ok, other);
    }
}

int main(void) {
    const int64_t MINA = INT64_C(0x100000000), MAXOP = INT64_C(0x7FFFFFFFFFFF8000);
    const int64_t P62 = INT64_C(1) << 62;
    const Case cases[] = {
        /* the tier-2 wide_add_64 shape: result range forces a + b < 2^62 */
        { "add 63u -> 64s [-2^62, 2^62)", BIN_ADD, 63, 0, MINA, MAXOP,
          64, 1, -P62, P62 - 1 },
        { "add 63u -> 64s full",  BIN_ADD, 63, 0, MINA, MAXOP,
          64, 1, INT64_MIN, INT64_MAX },
        { "add 63u -> 64u full",  BIN_ADD, 63, 0, MINA, MAXOP,
          64, 0, 0, -1 },
        { "sub 63u -> 64s full",  BIN_SUB, 63, 0, 0, INT64_MAX,
          64, 1, INT64_MIN, INT64_MAX },
        { "sub 64u -> 64s full",  BIN_SUB, 64, 0, 0, -1,
          64, 1, INT64_MIN, INT64_MAX },
        { "sub 64s -> 64s full",  BIN_SUB, 64, 1, INT64_MIN, INT64_MAX,
          64, 1, INT64_MIN, INT64_MAX },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run(&cases[i]);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    return 0;
}
