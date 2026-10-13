"""Variables defined by an equality are decided last (_is_defined in
src/c/dvs_search.c).

A front-end lifts each operand into a declared variable, `t == <expr>`. The
search used to decide those like any other variable, and they have the
smallest domains, so it decided them first. Two of them defined from the same
input could then be given values no input yields, and when propagation cannot
invert the definition (`x & mask`) the search found out only by enumerating
the input: pyvsc's `abs(s - (b & 0x7F)) == 3` took ~1 s and up to 6.2M
conflicts per solve (pyvsc doc/notes/vdc_grammar_deferred_issues.md, F5).
Each test solves under a conflict budget that the old order exhausted on
the first seed.
"""
from __future__ import annotations

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import (BIN_ADD, BIN_BAND, BIN_EQ, BIN_LT, BIN_SUB)
from dv_solve.ctx import SolveCtx, SOLVE_OK

# Restart unit 100, at most 5 restarts: a few hundred conflicts.
BUDGET = dict(max_conflicts=100, max_restarts=5)


def _solve_all(build, n, check, **opts):
    b = SolveProblemBuilder()
    try:
        vids = build(b)
        buf, _sz = b.finalize()
    finally:
        b.destroy()
    ctx = SolveCtx(buf)
    out = []
    try:
        for s in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=s, fair_pick=True, **opts) == SOLVE_OK, s
            v = tuple(ctx.get_value(i) for i in vids)
            assert check(*v), (s, v)
            out.append(v)
    finally:
        ctx.destroy()
    return out


def _f5(b):
    """pyvsc's problem for `abs(s - (b & 0x7F)) == 3`, s: s8, b: u8. The
    expression is unsigned (SV), so `abs` is `ite(x <u 0, 0 - x, x)` with x
    lifted three times: v2..v12 are the front-end's temporaries."""
    b.add_var(0, 8, True, -128, 127)
    b.add_var(1, 8, False, 0, 255)
    for v in range(2, 13):
        b.add_var(v, 32, False, 0, (1 << 32) - 1)
    var = b.expr_var
    c32 = lambda k: b.expr_const(k, width=32)

    def eq(v, e):
        b.add_constraint(b.expr_binary(BIN_EQ, var(v), e))

    s32 = lambda: b.expr_extend(var(0), 8, 32, False)
    m32 = lambda: b.expr_binary(BIN_BAND, b.expr_extend(var(1), 8, 32, False), c32(127))
    eq(12, c32(3))
    eq(12, b.expr_ite(b.expr_binary(BIN_LT, var(4), c32(0)), var(8), var(11)))
    eq(11, b.expr_binary(BIN_SUB, var(10), var(9)))
    eq(10, s32())
    eq(9, m32())
    eq(8, b.expr_binary(BIN_SUB, c32(0), var(7)))
    eq(7, b.expr_binary(BIN_SUB, var(6), var(5)))
    eq(6, s32())
    eq(5, m32())
    eq(4, b.expr_binary(BIN_SUB, var(3), var(2)))
    eq(3, s32())
    eq(2, m32())
    return [0, 1]


def test_abs_of_masked_difference():
    draws = _solve_all(_f5, 50, lambda s, b: (s & 0xFF) - (b & 0x7F) == 3, **BUDGET)
    assert len(set(draws)) > 30


def _mask_eq(b):
    """pyvsc's problem for (a & 0xFF) + 1 == (b & 0xFF) and
    (a & 0xFF00) == (b & 0xFF00), a, b: u16; v2..v4 are its temporaries."""
    b.add_var(0, 16, False, 0, 0xFFFF)
    b.add_var(1, 16, False, 0, 0xFFFF)
    for v in range(2, 5):
        b.add_var(v, 32, False, 0, (1 << 32) - 1)
    var = b.expr_var
    c32 = lambda k: b.expr_const(k, width=32)
    mask = lambda v, k: b.expr_binary(BIN_BAND, b.expr_extend(var(v), 16, 32, False), c32(k))

    def eq(lhs, rhs):
        b.add_constraint(b.expr_binary(BIN_EQ, lhs, rhs))

    eq(mask(0, 0xFF00), var(4))
    eq(var(4), mask(1, 0xFF00))
    eq(b.expr_binary(BIN_ADD, var(2), c32(1)), var(3))
    eq(var(3), mask(1, 0xFF))
    eq(var(2), mask(0, 0xFF))
    return [0, 1]


def test_masked_fields_related():
    # Deciding a still leaves b to enumerate: a pinned `b & mask` does not
    # narrow b, so this needs a larger budget than F5 (F5 item 3 in the
    # pyvsc notes). The old order fails even at 200 restarts.
    _solve_all(_mask_eq, 50,
               lambda a, b: (a & 0xFF) + 1 == (b & 0xFF)
               and (a & 0xFF00) == (b & 0xFF00),
               max_conflicts=100, max_restarts=200)


def test_defined_variable_in_a_solve_order_is_decided_first():
    # z == x + y over x, y in [0:3]. Unordered, z follows the uniform (x, y):
    # z == 0 has 1/16 of the draws. With `solve z before x, y`, z is chosen
    # first and is uniform over its 7 feasible values: 1/7 each.
    def build(ordered):
        def f(b):
            b.add_var(0, 8, False, 0, 3)
            b.add_var(1, 8, False, 0, 3)
            b.add_var(2, 8, False, 0, 255)
            b.add_constraint(b.expr_binary(
                BIN_EQ, b.expr_var(2),
                b.expr_binary(BIN_ADD, b.expr_var(0), b.expr_var(1))))
            if ordered:
                b.set_var_order(2, 1)
            return [0, 1, 2]
        return f

    check = lambda x, y, z: z == x + y
    n = 2800
    zero = lambda draws: sum(1 for d in draws if d[2] == 0) / n
    assert abs(zero(_solve_all(build(True), n, check)) - 1 / 7) < 0.03
    assert abs(zero(_solve_all(build(False), n, check)) - 1 / 16) < 0.03


def _copies(rel):
    """t1 == a * b and t2 == a * b (a, b: u8; t1, t2: u16), with `rel` over
    t1, t2: the shape a front-end writes when it lifts each mention of a*b."""
    from dv_solve.problem import BIN_MUL

    def build(b):
        b.add_var(0, 8, False, 0, 255)
        b.add_var(1, 8, False, 0, 255)
        b.add_var(2, 16, False, 0, 0xFFFF)
        b.add_var(3, 16, False, 0, 0xFFFF)
        prod = lambda: b.expr_binary(BIN_MUL, b.expr_extend(b.expr_var(0), 8, 16, False),
                                     b.expr_extend(b.expr_var(1), 8, 16, False))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(2), prod()))
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(3), prod()))
        b.add_constraint(b.expr_binary(rel, b.expr_var(2), b.expr_var(3)))
    return build


def test_duplicate_definitions_are_one_variable():
    # Merged, t1 != t2 is t1 != t1: unsatisfiable at once. Unmerged, the
    # search had to try a and b to find that out.
    from dv_solve.problem import BIN_NEQ, BIN_LT
    from dv_solve.ctx import CompileUnsatError, SOLVE_UNSAT
    for rel in (BIN_NEQ, BIN_LT):
        b = SolveProblemBuilder()
        try:
            _copies(rel)(b)
            buf, _sz = b.finalize()
        finally:
            b.destroy()
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            continue
        try:
            assert ctx.solve(seed=1, fair_pick=True, **BUDGET) == SOLVE_UNSAT
        finally:
            ctx.destroy()
