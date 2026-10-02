"""Clause learning generalizes through an unsigned add or subtract.

``x + n <= 2^20`` with x a 64-bit variable: a search that decides x picks a
random 64-bit value, and the add fails. Explained by x's exact value, the
learnt clause is ``x != X0`` and the next decision fails the same way, for
ever. Explained by the weakest bounds that imply the failure
(``_explain_bvsum_lifted``), it is ``x <= 2^20 - 1 or x >= 2^64 - 256``: one
conflict, and x is back in range.

The random problems hold the lifted explanation to soundness: every verdict
must match a search without learning, and every model must satisfy the
constraints. (The step-checker build checks each explanation directly.)
"""
import random

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_ADD, BIN_GTE, BIN_LTE, BIN_SUB

SOLVE_OK, SOLVE_UNSAT = 0, 1
MASK64 = (1 << 64) - 1


def test_a_wide_decision_learns_the_range_not_a_value():
    b = SolveProblemBuilder()
    b.add_var(0, 64, False, 0, MASK64)          # x
    b.add_var(1, 64, False, 1, 256)             # n
    b.add_constraint(b.expr_binary(BIN_LTE, b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_var(1)),
                                   b.expr_const(1 << 20)))
    buf, _ = b.finalize()
    try:
        for seed in range(1, 51):
            ctx = SolveCtx(buf)
            try:
                assert ctx.solve(seed=seed, use_lcg=True, max_restarts=5) == SOLVE_OK, seed
                x, n = ctx.get_value(0) & MASK64, ctx.get_value(1)
                assert ((x + n) & MASK64) <= 1 << 20
            finally:
                ctx.destroy()
    finally:
        b.destroy()


W = 5                                      # small enough to enumerate
MASK = (1 << W) - 1


def _random_problem(rng):
    """Four 5-bit unsigned variables with narrow ranges, a few
    `p +- q <= / >= r` rows (wrapping at 2^5, as the solver must)."""
    b = SolveProblemBuilder()
    doms = []
    for v in range(4):
        lo = rng.randint(0, MASK)
        hi = min(MASK, lo + rng.randint(0, 11))
        b.add_var(v, W, False, lo, hi)
        doms.append(range(lo, hi + 1))
    rows = []
    for _ in range(rng.randint(2, 4)):
        p, q, r = rng.sample(range(4), 3)
        op = rng.choice([BIN_ADD, BIN_SUB])
        cmp = rng.choice([BIN_LTE, BIN_GTE])
        e = b.expr_binary(op, b.expr_var(p), b.expr_var(q))
        b.add_constraint(b.expr_binary(cmp, e, b.expr_var(r)))
        rows.append((p, q, r, op, cmp))
    buf, _ = b.finalize()
    return b, buf, rows, doms


def _holds(vals, rows):
    for p, q, r, op, cmp in rows:
        e = (vals[p] + vals[q]) if op == BIN_ADD else (vals[p] - vals[q])
        e &= MASK
        if not (e <= vals[r] if cmp == BIN_LTE else e >= vals[r]):
            return False
    return True


def _satisfiable(rows, doms):
    import itertools
    return any(_holds(v, rows) for v in itertools.product(*doms))


def test_random_sums_match_enumeration_with_and_without_learning():
    rng = random.Random(11)
    for _ in range(200):
        b, buf, rows, doms = _random_problem(rng)
        try:
            truth = SOLVE_OK if _satisfiable(rows, doms) else SOLVE_UNSAT
            seed = rng.randint(1, 1 << 30)
            for lcg in (False, True):
                ctx = SolveCtx(buf)
                try:
                    rc = ctx.solve(seed=seed, use_lcg=lcg, time_limit_ms=5000)
                    assert rc == truth, (rows, [list(d)[:1] + list(d)[-1:] for d in doms], lcg, rc)
                    if rc == SOLVE_OK:
                        assert _holds([ctx.get_value(i) for i in range(4)], rows)
                finally:
                    ctx.destroy()
        finally:
            b.destroy()


def test_a_product_with_holes_is_learnt_by_range():
    """n == k * s, s in {512, 4096}, n <= 2^18 (an NVMe transfer size).

    Learning used to decide n (a wide variable it had seen in conflicts) to a
    value, find it no product, and learn `n != X` -- one value at a time,
    over a 2^18 range. Now the product's operands follow from an interval of
    n (`_bv_mul_divide`), and a wide variable seen in conflicts is decided by
    bounds (`_bound_decision`), so a failure teaches a range.
    """
    from dv_solve.problem import BIN_EQ, BIN_MUL
    N, K, S = 0, 1, 2
    b = SolveProblemBuilder()
    b.add_var(N, 32, False, 0, 1 << 18)
    b.add_var(K, 32, False, 1, 256)
    b.add_var(S, 16, False, 512, 4096)
    b.add_constraint(b.expr_in_set(b.expr_var(S), [b.expr_const(512), b.expr_const(4096)]))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(N),
                                   b.expr_binary(BIN_MUL, b.expr_var(K), b.expr_var(S))))
    buf, _ = b.finalize()
    try:
        for seed in range(1, 101):
            ctx = SolveCtx(buf)
            try:
                assert ctx.solve(seed=seed, use_lcg=True, max_restarts=5) == SOLVE_OK, seed
                n, k, s = (ctx.get_value(i) for i in (N, K, S))
                assert s in (512, 4096) and n == k * s and n <= 1 << 18
            finally:
                ctx.destroy()
    finally:
        b.destroy()
