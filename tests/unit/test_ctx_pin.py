"""``SolveCtx.pin``: the Python face of ``dvs_solver_pin_var``.

bc's scope solve (pssc P1) pins every value an earlier traversal committed and
solves the rest with lookahead; this is the call it makes. The C function has
its own tests (``test_pin_var.py``); these hold the wrapper and the
checkpoint/pin/solve/restore protocol it documents.
"""
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_LT

SOLVE_OK = 0


def _chain():
    """a < b < c, each 4 bits (LRM Ex 179)."""
    b = SolveProblemBuilder()
    for v in range(3):
        b.add_var(v, 4, False, 0, 15)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(1), b.expr_var(2)))
    buf, _ = b.finalize()
    return b, SolveCtx(buf)


def test_a_pin_holds_through_the_solve():
    b, ctx = _chain()
    try:
        assert ctx.pin(0, 5)
        for seed in range(16):
            cp = ctx.checkpoint()
            assert ctx.solve(seed=seed) == SOLVE_OK
            a, bb, c = (ctx.get_value(i) for i in range(3))
            assert a == 5 and 5 < bb < c
            ctx.restore(cp)
    finally:
        ctx.destroy()
        b.destroy()


def test_a_conflicting_pin_is_reported():
    """a == 15 leaves no b > a in 4 bits."""
    b, ctx = _chain()
    try:
        cp = ctx.checkpoint()
        assert not ctx.pin(0, 15)
        ctx.restore(cp)
    finally:
        ctx.destroy()
        b.destroy()


def test_restore_undoes_a_pin():
    b, ctx = _chain()
    try:
        cp = ctx.checkpoint()
        assert ctx.pin(1, 14)
        ctx.restore(cp)
        seen = set()
        for seed in range(64):
            cp2 = ctx.checkpoint()
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
            seen.add(ctx.get_value(1))
            ctx.restore(cp2)
        assert seen != {14}
    finally:
        ctx.destroy()
        b.destroy()
