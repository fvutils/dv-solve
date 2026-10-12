"""B118: an incremental add that declares a variable at an id the compile
took for an internal variable was read as that variable (a wrong unsat).
It is now refused (-4); new variables are numbered from SolveCtx.n_vars."""
from dv_solve.ctx import SolveCtx, SOLVE_OK
from dv_solve.problem import BIN_ADD, BIN_EQ, SolveProblem

BAD_VAR = -4


def _base():
    sp = SolveProblem()
    sp.add_var(0, 8, False, 0, 200)
    sp.add_var(1, 8, False, 0, 200)
    sp.add_var(2, 16, False, 0, 65535)
    # x1 + 1000 gets an aux variable of the compile's own, at id 3.
    sp.add_constraint(sp.expr_binary(
        BIN_EQ, sp.expr_var(2),
        sp.expr_binary(BIN_ADD, sp.expr_var(1), sp.expr_const(1000))))
    return sp


def _add(vid):
    ap = SolveProblem()
    ap.add_var(vid, 16, False, 0, 1000)
    ap.add_constraint(ap.expr_binary(
        BIN_EQ, ap.expr_var(vid),
        ap.expr_binary(BIN_ADD, ap.expr_var(0), ap.expr_const(3))))
    return ap


def test_internal_id_is_refused():
    with SolveCtx(_base()) as ctx:
        n = ctx.n_vars
        assert n > 3
        assert ctx.add_constraint(_add(3)) == BAD_VAR
        assert ctx.n_vars == n
        assert ctx.solve(seed=1) == SOLVE_OK


def test_new_var_from_n_vars():
    with SolveCtx(_base()) as ctx:
        n = ctx.n_vars
        assert ctx.add_constraint(_add(n)) == 0
        assert ctx.solve(seed=1) == SOLVE_OK
        assert ctx.get_value(n) == ctx.get_value(0) + 3
        assert ctx.get_value(2) == ctx.get_value(1) + 1000
