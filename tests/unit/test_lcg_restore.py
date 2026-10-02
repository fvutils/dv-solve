"""A context reused with LCG solves exactly as a freshly compiled one.

A caller that solves one problem many times compiles it once and solves it
between a checkpoint and a restore. With lazy clause generation on, a solve
learns clauses, bumps variable activity and fills the clause arena. A restore
to a checkpoint taken before any learning must undo all of it: the next solve
must search -- and answer -- exactly as on a fresh context. Before
``lcg_reset``, the activity survived (the next solve chose its decisions by
the previous solve's conflicts) and the arena was never reclaimed, so after
enough solves learning silently stopped.

8-queens is the problem: satisfiable, and a random first placement conflicts
often enough that every few seeds learn.
"""
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_NEQ, BIN_SUB

SOLVE_OK = 0
N = 8


def _queens():
    b = SolveProblemBuilder()
    for v in range(N):
        b.add_var(v, 8, False, 0, N - 1)
    for i in range(N):
        for j in range(i + 1, N):
            b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_var(i), b.expr_var(j)))
            for lhs, rhs in ((j, i), (i, j)):
                d = b.expr_binary(BIN_SUB, b.expr_var(lhs), b.expr_var(rhs))
                b.add_constraint(b.expr_binary(BIN_NEQ, d, b.expr_const(j - i, width=8)))
    buf, _ = b.finalize()
    return b, buf


def _is_solution(q):
    return all(q[i] != q[j] and abs(q[i] - q[j]) != j - i
               for i in range(N) for j in range(i + 1, N))


def _fresh(buf, seed):
    ctx = SolveCtx(buf)
    try:
        assert ctx.solve(seed=seed, use_lcg=True) == SOLVE_OK
        return [ctx.get_value(i) for i in range(N)]
    finally:
        ctx.destroy()


def _reused(ctx, seed):
    cp = ctx.checkpoint()
    try:
        assert ctx.solve(seed=seed, use_lcg=True) == SOLVE_OK
        return [ctx.get_value(i) for i in range(N)]
    finally:
        ctx.restore(cp)


def test_reused_context_answers_as_a_fresh_one():
    b, buf = _queens()
    ctx = SolveCtx(buf)
    try:
        for seed in range(1, 201):
            got = _reused(ctx, seed)
            assert _is_solution(got)
            assert got == _fresh(buf, seed), "seed %d" % seed
    finally:
        ctx.destroy()
        b.destroy()


def test_thousands_of_reuses_keep_answering_as_fresh():
    """The arena is reclaimed: late solves still learn, and still match."""
    b, buf = _queens()
    ctx = SolveCtx(buf)
    try:
        for seed in range(1, 4001):
            got = _reused(ctx, seed)
        for seed in range(4001, 4051):
            got = _reused(ctx, seed)
            assert _is_solution(got)
            assert got == _fresh(buf, seed), "seed %d" % seed
    finally:
        ctx.destroy()
        b.destroy()
