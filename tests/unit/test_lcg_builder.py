"""Builder problems solved with clause learning on (`use_lcg`), as C callers can.

B60: `ite_value` declared itself entailed when r and the selected branch's
LOWER bounds matched, without checking the branch was fixed. With learning on,
a tightening inside the firing can propagate a learnt clause and narrow r on
the spot, so the branch could still be [8, 9] when the propagator retired;
y = 9 later went unenforced and `(y*x if a else x*x) < itself` came back sat
with a model that violates it. Found by the soundness campaign's builder door.
"""
from __future__ import annotations

import ctypes

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_UNSAT, _SolveOpts
from dv_solve.problem import BIN_LT, BIN_LTE, BIN_MUL


def test_ite_product_less_than_itself_is_unsat_with_learning():
    b = SolveProblemBuilder()
    A, X, Y = 0, 1, 2
    b.add_var(A, width=1, is_signed=False, lo=0, hi=1)
    b.add_var(X, width=5, is_signed=False, lo=0, hi=31)
    b.add_var(Y, width=5, is_signed=False, lo=0, hi=31)

    def t():
        sel = b.expr_ite(b.expr_binary(BIN_LTE, b.expr_const(1, width=1), b.expr_var(A)),
                         b.expr_var(Y), b.expr_var(X))
        return b.expr_binary(BIN_MUL, sel, b.expr_var(X))
    b.add_constraint(b.expr_binary(BIN_LT, t(), t()))
    with SolveCtx(b.finalize()[0]) as ctx:
        for lcg in (0, 1):
            ctx.reset()
            rc = ctx._lib.dvs_solver_solve(ctx._ctx, ctypes.byref(
                _SolveOpts(seed=1, use_lcg=lcg, time_limit_ms=10000)))
            assert rc == SOLVE_UNSAT, (lcg, rc)
