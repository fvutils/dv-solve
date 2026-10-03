"""``expr_cast``: SystemVerilog's ``T'(x)`` for an integral T.

The operand is an assignment-like context: a cast wider than the operand
widens the operand's evaluation (``64'(a * b)`` does not wrap at 32 bits),
and the value is then truncated, or extended by the operand's own signedness,
and read at the cast's signedness. Each case is small enough to enumerate:
the solver's verdict must match, every model must be a solution, and the
model validator, which evaluates the original problem independently of the
engines, must accept it.
"""
from itertools import product

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT
from dv_solve.problem import BIN_ADD, BIN_BAND, BIN_EQ, BIN_GT, BIN_LT, BIN_MUL


def _s(v, w):
    """The low w bits of v, read as signed."""
    v &= (1 << w) - 1
    return v - (1 << w) if v >> (w - 1) else v


def _u(v, w):
    return v & ((1 << w) - 1)


# name -> (vars {id: (width, signed)}, build(b, V, K) -> constraint, oracle(vals))
CASES = {
    # 8-bit cast of a 4x4 product: evaluated at 8 bits, so 15 * 15 = 225.
    "widen_product": (
        {0: (4, False), 1: (4, False)},
        lambda b, V, K: b.expr_binary(BIN_EQ, b.expr_cast(
            b.expr_binary(BIN_MUL, V(0), V(1)), 8), K(225, 8)),
        lambda v: v[0] * v[1] == 225),
    # Without the cast, the product of two 4-bit values in a 4-bit context
    # wraps: the same constant is unreachable through a 4-bit result.
    "narrow_product_wraps": (
        {0: (4, False), 1: (4, False), 2: (4, False)},
        lambda b, V, K: b.expr_binary(BIN_EQ, V(2), b.expr_binary(BIN_MUL, V(0), V(1))),
        lambda v: _u(v[0] * v[1], 4) == v[2]),
    # Truncation: the low 2 bits.
    "truncate": (
        {0: (6, False)},
        lambda b, V, K: b.expr_binary(BIN_EQ, b.expr_cast(V(0), 2), K(3, 2)),
        lambda v: v[0] & 3 == 3),
    # Truncation of a sum computed at the operand's own width (5 bits).
    "truncate_sum": (
        {0: (5, False), 1: (5, False)},
        lambda b, V, K: b.expr_binary(BIN_EQ, b.expr_cast(
            b.expr_binary(BIN_ADD, V(0), V(1)), 3), K(5, 3)),
        lambda v: _u(v[0] + v[1], 3) == 5),
    # Unsigned to signed of the same width: the top half reads negative.
    "to_signed": (
        {0: (4, False)},
        lambda b, V, K: b.expr_binary(BIN_LT, b.expr_cast(V(0), 4, True),
                                      b.expr_const(0, True, 4)),
        lambda v: v[0] >= 8),
    # Signed to a wider unsigned: sign-extended, then read unsigned.
    "signed_to_wide_unsigned": (
        {0: (4, True)},
        lambda b, V, K: b.expr_binary(BIN_GT, b.expr_cast(V(0), 8), K(200, 8)),
        lambda v: _u(v[0], 8) > 200),
    # Signed truncation: the low 3 bits read signed.
    "truncate_to_signed": (
        {0: (6, False)},
        lambda b, V, K: b.expr_binary(BIN_EQ, b.expr_cast(V(0), 3, True),
                                      b.expr_const(-3, True, 3)),
        lambda v: _s(v[0], 3) == -3),
    # A cast's own type sizes its context: two 8-bit casts of 4-bit values
    # add at 8 bits, so 9 + 9 = 18 is not wrapped to 2.
    "cast_types_context": (
        {0: (4, False), 1: (4, False), 2: (8, False)},
        lambda b, V, K: b.expr_binary(BIN_EQ, V(2), b.expr_binary(
            BIN_ADD, b.expr_cast(V(0), 8), b.expr_cast(V(1), 8))),
        lambda v: v[0] + v[1] == v[2]),
    # A narrowing cast inside a wider context is extended by ITS signedness:
    # a 3-bit signed cast of x, compared with a signed 8-bit y.
    "narrow_signed_in_wide_context": (
        {0: (6, False), 1: (8, True)},
        lambda b, V, K: b.expr_binary(BIN_EQ, V(1), b.expr_cast(V(0), 3, True)),
        lambda v: v[1] == _s(v[0], 3)),
    # A cast in a Boolean position holds when nonzero.
    "boolean": (
        {0: (5, False)},
        lambda b, V, K: b.expr_cast(V(0), 2),
        lambda v: v[0] & 3 != 0),
}


def _domain(w, signed):
    return range(-(1 << (w - 1)), 1 << (w - 1)) if signed else range(1 << w)


@pytest.mark.parametrize("name", sorted(CASES))
def test_cast(name):
    vars_, build, oracle = CASES[name]
    b = SolveProblemBuilder()
    for vid, (w, s) in vars_.items():
        d = _domain(w, s)
        b.add_var(vid, w, s, d[0], d[-1])
    b.add_constraint(build(b, b.expr_var, lambda c, w: b.expr_const(c, width=w)))
    prob, _ = b.finalize()

    ids = sorted(vars_)
    sols = {vals for vals in product(*(_domain(*vars_[i]) for i in ids))
            if oracle(dict(zip(ids, vals)))}
    assert sols, "every case is satisfiable"
    ctx = SolveCtx(prob)
    seen = set()
    try:
        for seed in range(1, 41):
            cp = ctx.checkpoint()
            assert ctx.solve(seed=seed) == SOLVE_OK
            got = tuple(ctx.get_value(i) for i in ids)
            assert got in sols, (got, name)
            assert ctx.validate_model() == 0
            seen.add(got)
            ctx.restore(cp)
    finally:
        ctx.destroy()
    assert len(seen) > 1 or len(sols) == 1


def test_cast_contradiction_is_unsat():
    # 2-bit truncation can never be 4.
    b = SolveProblemBuilder()
    b.add_var(0, 6, False, 0, 63)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_cast(b.expr_var(0), 2),
                                   b.expr_const(4, width=8)))
    prob, _ = b.finalize()
    ctx = SolveCtx(prob)
    try:
        assert ctx.solve(seed=1) == SOLVE_UNSAT
    finally:
        ctx.destroy()


@pytest.mark.parametrize("shape", ["extract", "band", "extend", "cast"])
def test_a_value_as_a_constraint_holds_when_nonzero(shape):
    """A multi-bit value used as a whole constraint means `value != 0`. Only a
    bare variable compiled before; these shapes were refused as incomplete."""
    b = SolveProblemBuilder()
    b.add_var(0, 5, False, 0, 31)
    x = b.expr_var(0)
    e, holds = {
        "extract": (lambda: b.expr_extract(x, 1, 0), lambda v: v & 3 != 0),
        "band": (lambda: b.expr_binary(BIN_BAND, x, b.expr_const(4, width=5)),
                 lambda v: v & 4 != 0),
        "extend": (lambda: b.expr_extend(x, 5, 8), lambda v: v != 0),
        "cast": (lambda: b.expr_cast(x, 1), lambda v: v & 1 != 0),
    }[shape]
    b.add_constraint(e())
    prob, _ = b.finalize()
    ctx = SolveCtx(prob)
    seen = set()
    try:
        for seed in range(1, 41):
            cp = ctx.checkpoint()
            assert ctx.solve(seed=seed) == SOLVE_OK
            v = ctx.get_value(0)
            assert holds(v), v
            seen.add(v)
            ctx.restore(cp)
    finally:
        ctx.destroy()
    assert len(seen) > 4
