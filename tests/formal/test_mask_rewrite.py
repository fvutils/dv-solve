"""Constant-mask comparisons compile to bit-field comparisons.

`(x & M) == V` gave the CDCL engine nothing about x: the bitwise-and
propagator only bounds its result, so the search drew x and rejected it, at a
cost that doubled with every masked bit (a 20-bit alignment mask on a 32-bit
address hit the time limit). Compile now rewrites such comparisons into
comparisons of x's bit fields (dvs_sv_rewrite_masks, run for every front end).
See docs/bitfield_linearization_research.md.

The rewrite must be an equivalence. These check it exhaustively on 4-bit
operands -- every mask, every constant, every comparison, both operand orders,
the operand forms it looks through (zero extension, extract, constant shift)
-- by pinning every value of the operand's variable and comparing sat/unsat
with the IR's own evaluator.
"""
from __future__ import annotations

import time

import pytest

from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import ev

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder            # noqa: E402
from dv_solve.ctx import (CompileIncompleteError, CompileUnsatError,  # noqa: E402
                          SOLVE_OK, SOLVE_UNSAT, SolveCtx)

WIDTHS = {"x": 4, "y": 4, "s": 2, "z": 6}
IDS = {n: i for i, n in enumerate(WIDTHS)}
CMPS = ["=", "distinct", "bvult", "bvule", "bvugt", "bvuge"]

X, Y = ("var", "x", 4), ("var", "y", 4)
OPERANDS = {
    "var": (X, ["x"]),
    "zext": (("zero_extend", 4, ("var", "s", 2)), ["s"]),
    "extract": (("extract", 4, 4, 1, ("var", "z", 6)), ["z"]),
    "lshr1": (("bvlshr", 4, X, ("const", 1, 4)), ["x"]),
    "lshr3": (("bvlshr", 4, X, ("const", 3, 4)), ["x"]),
}


def _assignments(names):
    out = [{}]
    for n in names:
        out = [dict(a, **{n: v}) for a in out for v in range(1 << WIDTHS[n])]
    return out


def _check(cons, names):
    """Every assignment of `names` is sat exactly when the constraints hold."""
    b = SolveProblemBuilder()
    for n, w in WIDTHS.items():
        b.add_var(IDS[n], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in cons:
        b.add_constraint(_lower(b, c, IDS))
    prob, _ = b.finalize()
    truth = {tuple(a[n] for n in names): all(ev(c, {**dict.fromkeys(WIDTHS, 0), **a})
                                             for c in cons)
             for a in _assignments(names)}
    try:
        ctx = SolveCtx(prob)
    except CompileUnsatError:
        assert not any(truth.values()), cons
        return
    except CompileIncompleteError:
        pytest.fail(f"not compiled: {cons}")
    with ctx:
        for key, want in truth.items():
            ctx.reset()
            cp = ctx.checkpoint()
            pinned = all(ctx.pin(IDS[n], v) for n, v in zip(names, key))
            rc = ctx.solve(seed=1) if pinned else SOLVE_UNSAT
            ctx.restore(cp)
            assert rc in (SOLVE_OK, SOLVE_UNSAT), (cons, key, rc)
            assert (rc == SOLVE_OK) == want, (cons, key, "sat" if rc == SOLVE_OK else "unsat")


@pytest.mark.parametrize("form", sorted(OPERANDS))
@pytest.mark.parametrize("op", ["bvand", "bvor"])
def test_masked_compare_is_exact(form, op):
    x, names = OPERANDS[form]
    for m in range(16):
        for v in range(16):
            for cmp in CMPS:
                a = (op, 4, x, ("const", m, 4))
                c = ("const", v, 4)
                _check([(cmp, "bool", a, c)], names)
                _check([(cmp, "bool", c, (op, 4, ("const", m, 4), x))], names)


def test_masked_pair_is_exact():
    for m in range(16):
        for cmp in ("=", "distinct"):
            _check([(cmp, "bool", ("bvand", 4, X, ("const", m, 4)),
                     ("bvand", 4, Y, ("const", m, 4)))], ["x", "y"])


def test_under_boolean_structure():
    """Inside not / or / and, and next to other constraints."""
    a = ("bvand", 4, X, ("const", 0b0110, 4))
    for v in range(16):
        k = ("const", v, 4)
        _check([("not", "bool", ("=", "bool", a, k))], ["x"])
        _check([("or", "bool", ("=", "bool", a, k), ("bvult", "bool", X, ("const", 3, 4)))],
               ["x"])
        _check([("=", "bool", a, ("const", 0b0100, 4)), ("bvuge", "bool", X, k)], ["x"])


def test_wide_alignment_is_fast():
    """32-bit `(a & (2^k - 1)) == 0`: about 7 ms a solve at k = 16 and the time
    limit at k = 20 before the rewrite; now microseconds, at any k."""
    for k in (12, 20, 28):
        b = SolveProblemBuilder()
        b.add_var(0, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
        from dv_solve.problem import BIN_BAND, BIN_EQ
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(
            BIN_BAND, b.expr_var(0), b.expr_const((1 << k) - 1, width=32)),
            b.expr_const(0, width=32)))
        prob, _ = b.finalize()
        with SolveCtx(prob) as ctx:
            t0 = time.perf_counter()
            for seed in range(1, 101):
                ctx.reset()
                assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
                assert ctx.get_value(0) & ((1 << k) - 1) == 0
            assert time.perf_counter() - t0 < 1.0, k


Z = ("var", "z", 6)


def _ex(h, l):
    return ("extract", h - l + 1, h, l, Z)


def test_negation_pushdown_is_exact():
    """`!(a || b)`, `!(a && b)` and nested negations over field comparisons
    are pushed to flipped comparisons (De Morgan). Before, `!(x[5:3] inside
    {..})` did not compile at all on the CDCL engine."""
    hi, lo = _ex(5, 3), _ex(2, 0)
    for v in range(8):
        a = ("=", "bool", hi, ("const", v, 3))
        b = ("bvult", "bool", lo, ("const", v, 3))
        c = ("bvuge", "bool", hi, ("const", (v * 5) % 8, 3))
        for f in (("not", "bool", ("or", "bool", a, b)),
                  ("not", "bool", ("and", "bool", a, b)),
                  ("not", "bool", ("not", "bool", a)),
                  ("not", "bool", ("or", "bool", ("and", "bool", a, c), ("not", "bool", b))),
                  ("not", "bool", ("or", "bool", a, ("or", "bool", c,
                                                      ("=", "bool", hi, ("const", 7 - v, 3))))),
                  ("or", "bool", ("not", "bool", ("and", "bool", a, b)), c)):
            _check([f], ["z"])


def test_set_on_masked_field_is_exact():
    """`(z[4:1] & M) inside {..}` and its negation: the mask is seen through
    the bit select."""
    f = ("extract", 4, 4, 1, Z)
    for m in (0b1110, 0b0110, 0b1001):
        ms = ("bvand", 4, f, ("const", m, 4))
        vals = sorted({(v * 3) & m for v in range(4)})
        s = ("or", "bool", *[("=", "bool", ms, ("const", v, 4)) for v in vals]) \
            if len(vals) > 1 else ("=", "bool", ms, ("const", vals[0], 4))
        _check([s], ["z"])
        _check([("not", "bool", s)], ["z"])


def test_widened_field_is_exact():
    """A bit select zero-extended into a wider masked compare: the bits above
    the select are zero, not the variable's next bits."""
    f = ("zero_extend", 6, ("extract", 3, 3, 1, Z))
    for m in (0b111111, 0b011100, 0b101010):
        for v in range(0, 64, 3):
            for cmp in ("=", "distinct", "bvult", "bvuge"):
                _check([(cmp, "bool", ("bvand", 6, f, ("const", m, 6)), ("const", v, 6))], ["z"])


def test_negated_truth_value_leaf():
    """A variable read as a truth value under `!`: `!(s || z[2:0] == v)`."""
    s = ("var", "s", 2)
    for v in range(8):
        a = ("=", "bool", _ex(2, 0), ("const", v, 3))
        _check([("not", "bool", ("or", "bool", s, a))], ["s", "z"])
        _check([("not", "bool", ("and", "bool", s, a))], ["s", "z"])
