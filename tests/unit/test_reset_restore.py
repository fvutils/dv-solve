"""solver_reset must fully restore variable domains for ALL width tiers.

Regression test for a tier-1 (width >= 32) reset bug: compile saved a separate
pristine copy of each tier-1 var's WideBounds64, but solver_reset memcpy'd the
whole Variable array — repointing the var at the saved copy, making the restore
a self-copy no-op and letting the next solve corrupt the saved copy. Result:
tier-1 vars stayed pinned to their first solved value across resets.

Reusing a compiled SolveCtx via reset()+solve(new_seed) (the "compile once,
solve many" optimization) depends entirely on this working.
"""
import pytest
from dv_solve.builder import SolveProblemBuilder
from dv_solve import problem as P
from dv_solve.ctx import SolveCtx, SOLVE_OK


def _distinct_after_reset(result_width, n=8):
    """r == a + b with a,b in [1,200]; reset+solve n times with different
    seeds; return the set of distinct r values produced."""
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 1, 200)
    b.add_var(1, 8, False, 1, 200)
    hi = (1 << result_width) - 1 if result_width < 64 else (1 << 63) - 1
    b.add_var(2, result_width, False, 0, hi)
    b.add_constraint(b.expr_binary(
        P.BIN_EQ, b.expr_var(2),
        b.expr_binary(P.BIN_ADD, b.expr_var(0), b.expr_var(1))))
    buf, _ = b.finalize()
    ctx = SolveCtx(buf)
    seen = set()
    try:
        for s in range(n):
            ctx.reset()
            rc = ctx.solve(seed=s + 1, fair_pick=True)
            assert rc == SOLVE_OK
            a, bb, r = ctx.get_value(0), ctx.get_value(1), ctx.get_value(2)
            # r == (a + b) mod 2^width
            assert r == ((a + bb) & ((1 << result_width) - 1))
            seen.add(r)
    finally:
        ctx.destroy()
        b.destroy()
    return seen


@pytest.mark.parametrize("width", [8, 16, 31, 32, 40, 48, 63])
def test_reset_restores_all_tiers(libzsp, width):
    # A working reset must produce several distinct values across seeds; a
    # broken reset pins the variable and yields exactly one.
    seen = _distinct_after_reset(width)
    assert len(seen) > 1, (
        "width %d: reset did not restore the result var (only %r seen)"
        % (width, seen))
