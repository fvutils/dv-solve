"""Pins and soft constraints outside a checkpoint scope.

A pin is a hard fact; a soft constraint gives way to it. Two ways this went
wrong, both a spurious ``unsat`` or a model that broke the pin:

* ``dvs_solver_pin_var`` propagated the pin with every soft constraint
  assumed, and refused it (-1) when that conflicted -- although relaxing a
  soft constraint resolves it. ``len <= m; soft len == 64`` with ``m`` pinned
  to 32 was ``unsat`` (Verilator's ``randomize()`` returned 0).
* Relaxing a soft constraint reset the context to its compiled state, which
  outside a checkpoint dropped every pin: the retry solved without them.
"""
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_ADD, BIN_AND, BIN_EQ, BIN_LTE, BIN_NEQ, BIN_OR

SOLVE_OK = 0
SOLVE_UNSAT = 1
LEN, M = 0, 1


def _len_le_m():
    """len <= m; soft len == 64 (8-bit unsigned)."""
    b = SolveProblemBuilder()
    b.add_var(LEN, 8, False, 0, 255)
    b.add_var(M, 8, False, 0, 255)
    b.add_constraint(b.expr_binary(BIN_LTE, b.expr_var(LEN), b.expr_var(M)))
    b.add_soft_constraint(b.expr_binary(
        BIN_EQ, b.expr_var(LEN), b.expr_const(64, width=8)), 0)
    buf, _ = b.finalize()
    return b, SolveCtx(buf)


def test_a_pin_that_conflicts_only_with_a_soft_is_accepted():
    b, ctx = _len_le_m()
    try:
        for seed in range(1, 21):
            ctx.reset()
            assert ctx.pin(M, 32)
            assert ctx.solve(seed=seed) == SOLVE_OK
            assert ctx.get_value(M) == 32
            assert ctx.get_value(LEN) <= 32
    finally:
        ctx.destroy()
        b.destroy()


def test_the_soft_holds_when_the_pin_allows_it():
    b, ctx = _len_le_m()
    try:
        ctx.reset()
        assert ctx.pin(M, 32)
        assert ctx.solve(seed=1) == SOLVE_OK        # relaxes the soft
        ctx.reset()
        assert ctx.pin(M, 100)
        assert ctx.solve(seed=1) == SOLVE_OK
        assert ctx.get_value(LEN) == 64              # the soft is back
    finally:
        ctx.destroy()
        b.destroy()


def test_a_pin_against_a_hard_constraint_is_still_unsat():
    """len < m with m == 0: no soft can help."""
    b = SolveProblemBuilder()
    b.add_var(LEN, 8, False, 0, 255)
    b.add_var(M, 8, False, 0, 255)
    b.add_constraint(b.expr_binary(BIN_LTE, b.expr_var(LEN), b.expr_var(M)))
    b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_var(LEN), b.expr_var(M)))
    b.add_soft_constraint(b.expr_binary(
        BIN_EQ, b.expr_var(LEN), b.expr_const(64, width=8)), 0)
    buf, _ = b.finalize()
    ctx = SolveCtx(buf)
    try:
        ctx.reset()
        ctx.pin(M, 0)
        assert ctx.solve(seed=1) == SOLVE_UNSAT
    finally:
        ctx.destroy()
        b.destroy()


A, B, C, E, D = range(5)


def test_a_pin_survives_relaxing_a_soft_outside_a_checkpoint():
    """The soft conflicts only through search, so the pins are accepted at
    once; the relaxation that follows must keep them (test_soft_checkpoint
    has the same problem inside a checkpoint scope).

        a, b, c in {0, 1};  e, d free bytes
        hard:  e == 1  ->  (a != c && b != c)
        soft:  a + b == 1
    """
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
    ctx = SolveCtx(buf)
    try:
        for seed in range(1, 21):
            ctx.reset()
            assert ctx.pin(E, 1) and ctx.pin(D, 77)
            assert ctx.solve(seed=seed) == SOLVE_OK
            vals = [ctx.get_value(i) for i in range(5)]
            assert vals[E] == 1 and vals[D] == 77, vals
            assert vals[A] == vals[B] != vals[C], vals
    finally:
        ctx.destroy()
        b.destroy()
