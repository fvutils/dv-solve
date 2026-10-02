"""Soft constraints inside a checkpoint scope.

A solve that relaxes a soft constraint retries from where the solve began.
It used to retry from the COMPILED state (``dvs_solver_reset``), which
discarded pins made after the caller's checkpoint and the trail every open
checkpoint points into -- the next ``restore`` crashed -- and its relaxations
were untrailed writes that no restore undid. Inside a checkpoint scope a reset
now rewinds to the start of the last solve, and the relaxations go on the
trail.

The problem makes the soft constraint conflict only through search, so pins
are accepted (an active soft constraint is propagated like a hard one, so a
pin that contradicts it by propagation alone is refused -- a separate
limitation):

    a, b, c in {0, 1};  e, d free bytes
    hard:  e == 1  ->  (a != c && b != c)        (two values: then a == b)
    soft:  a + b == 1        (a != b, written so that bounds propagation
                              cannot decide it; a `!=` soft has its own
                              known spurious-unsat case)
"""
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_ADD, BIN_AND, BIN_EQ, BIN_NEQ, BIN_OR

SOLVE_OK = 0
A, B, C, E, D = range(5)


def _problem():
    b = SolveProblemBuilder()
    for v in (A, B, C):
        b.add_var(v, 8, False, 0, 1)
    b.add_var(E, 8, False, 0, 255)
    b.add_var(D, 8, False, 0, 255)
    v = b.expr_var
    ne = lambda x, y: b.expr_binary(BIN_NEQ, v(x), v(y))
    b.add_constraint(b.expr_binary(
        BIN_OR, b.expr_binary(BIN_NEQ, v(E), b.expr_const(1, width=8)),
        b.expr_binary(BIN_AND, ne(A, C), ne(B, C))))
    b.add_soft_constraint(b.expr_binary(
        BIN_EQ, b.expr_binary(BIN_ADD, v(A), v(B)), b.expr_const(1, width=8)), 0)
    buf, _ = b.finalize()
    return b, buf


def test_restore_after_a_relaxing_solve():
    """The crash: three rounds of checkpoint, relaxing solve, restore."""
    b, buf = _problem()
    ctx = SolveCtx(buf)
    try:
        for seed in range(1, 4):
            cp = ctx.checkpoint()
            assert ctx.pin(E, 1)
            assert ctx.solve(seed=seed) == SOLVE_OK
            ctx.restore(cp)
    finally:
        ctx.destroy()
        b.destroy()


def test_a_pin_survives_relaxing_a_soft():
    b, buf = _problem()
    ctx = SolveCtx(buf)
    try:
        for seed in range(1, 21):
            cp = ctx.checkpoint()
            assert ctx.pin(E, 1) and ctx.pin(D, 77)
            assert ctx.solve(seed=seed) == SOLVE_OK
            vals = [ctx.get_value(i) for i in range(5)]
            assert vals[E] == 1 and vals[D] == 77, vals
            assert vals[A] == vals[B] != vals[C], vals     # the soft had to go
            ctx.restore(cp)
    finally:
        ctx.destroy()
        b.destroy()


def test_restore_reactivates_the_relaxed_soft():
    b, buf = _problem()
    ctx = SolveCtx(buf)
    try:
        for seed in range(1, 21):
            cp = ctx.checkpoint()
            assert ctx.pin(E, 1)
            assert ctx.solve(seed=seed) == SOLVE_OK        # relaxes a + b == 1
            ctx.restore(cp)
            cp = ctx.checkpoint()
            assert ctx.pin(E, 0)
            assert ctx.solve(seed=seed) == SOLVE_OK
            assert ctx.get_value(A) + ctx.get_value(B) == 1  # the soft is back
            ctx.restore(cp)
    finally:
        ctx.destroy()
        b.destroy()


def test_reset_in_a_scope_keeps_what_the_scope_established():
    """A reset inside a checkpoint scope (an SMT-LIB check-sat after another,
    inside a push) undoes the last solve only: a pin and a constraint added
    in the scope -- `x <= 4` compiles to a bare bound, no propagator to
    re-derive it -- still hold, and the scope's checkpoint still restores."""
    from dv_solve.problem import BIN_LTE
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 100)
    b.add_var(1, 8, False, 0, 100)
    buf, _ = b.finalize()
    aux = SolveProblemBuilder()
    aux.add_var(0, 8, False, 0, 100)
    aux.add_constraint(aux.expr_binary(BIN_LTE, aux.expr_var(0), aux.expr_const(4, width=8)))
    abuf, _ = aux.finalize()
    ctx = SolveCtx(buf)
    try:
        cp = ctx.checkpoint()
        assert ctx.add_constraint(abuf) == 0
        assert ctx.pin(1, 7)
        for seed in range(1, 6):
            assert ctx.solve(seed=seed) == SOLVE_OK
            assert ctx.get_value(0) <= 4 and ctx.get_value(1) == 7
            ctx.reset()
        ctx.restore(cp)
        seen = set()
        for seed in range(1, 40):
            cp = ctx.checkpoint()
            assert ctx.solve(seed=seed) == SOLVE_OK
            seen.add(ctx.get_value(0))
            ctx.restore(cp)
        assert max(seen) > 4                          # the scope's bound is gone
    finally:
        ctx.destroy()
        b.destroy()
        aux.destroy()
