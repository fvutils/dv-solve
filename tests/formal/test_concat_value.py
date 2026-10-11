"""A concatenation used as a value, not only as `var == {hi, lo}`.

`{i[31], i[7], i[30:25], i[11:8]} != 0` -- a RISC-V branch offset -- and
`{y, x[3:0]} < k` were CompileIncomplete on the CDCL path: only `var ==
concat` compiled. A concat value is now an aux variable tied to its parts by
the concat propagator.

Seeded random concats over an 8-bit x and a 3-bit y -- slices, constants,
nested concats, under arithmetic, an ite or Boolean structure -- are checked
exhaustively against the IR's evaluator (see test_field_split._check).
"""
from __future__ import annotations

import random

import pytest

from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import ev
from tests.formal.test_field_split import X, Y, _c, _check, _ex

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder            # noqa: E402
from dv_solve.ctx import (CompileIncompleteError, CompileUnsatError,  # noqa: E402
                          SOLVE_OK, SOLVE_UNSAT, SolveCtx)


def _w(t):
    if t[0] == "var":
        return t[2]
    if t[0] == "const":
        return t[2]
    return t[1]


def _part(rng, depth):
    r = rng.randrange(6 if depth < 2 else 4)
    if r == 0:
        return Y
    if r == 1:
        lo = rng.randrange(8)
        return _ex(rng.randrange(lo, 8), lo)
    if r == 2:
        w = rng.randint(1, 3)
        return _c(rng.randrange(1 << w), w)
    if r == 3:
        return X
    return _cat(rng, depth + 1)


def _cat(rng, depth=0):
    for _ in range(20):
        h, l = _part(rng, depth), _part(rng, depth)
        if _w(h) + _w(l) <= 16:
            return ("concat", _w(h) + _w(l), h, l)
    return ("concat", 4, _ex(1, 0), _ex(7, 6))


def _con(rng):
    c = _cat(rng)
    w = _w(c)
    r = rng.randrange(6)
    if r == 1:                                  # under arithmetic
        c = ("bvadd", w, c, _c(rng.randrange(1 << w), w))
    elif r == 2:                                # inside an ite
        e = _c(rng.randrange(1 << w), w)
        c = ("ite", w, ("bvult", "bool", Y, _c(rng.randrange(8), 3)), c, e)
    elif r == 3 and w >= 8:                     # against a variable
        return (rng.choice(["=", "distinct", "bvult", "bvuge"]), "bool", c,
                ("zero_extend", w, X) if w > 8 else X)
    op = rng.choice(["=", "distinct", "bvult", "bvule", "bvugt", "bvuge"])
    k = rng.randrange(1 << w)
    return (op, "bool", c, _c(k, w))


def _case(seed):
    rng = random.Random(seed)
    cons = [_con(rng) for _ in range(rng.randint(1, 3))]
    if rng.random() < 0.3:
        cons = [("or", "bool", cons[0], ("not", "bool", cons[-1]))] + cons[1:-1]
    return cons


@pytest.mark.parametrize("seed", range(150))
def test_random_concat(seed):
    _check(_case(seed), declines=False)


def test_branch_offset():
    """B-type immediate, scaled: {x[7], x[0], x[6:4], x[3:1]} != 0, next to
    x[3:1] < 6 and the high bit under y."""
    imm = ("concat", 8, ("concat", 2, _ex(7, 7), _ex(0, 0)),
           ("concat", 6, _ex(6, 4), _ex(3, 1)))
    _check([("distinct", "bool", imm, _c(0, 8)),
            ("bvult", "bool", _ex(3, 1), _c(6, 3)),
            ("=>", "bool", ("bvult", "bool", Y, _c(2, 3)), ("=", "bool", _ex(7, 7), _c(1, 1)))])


S = ("var", "s", 3)


@pytest.mark.parametrize("k", range(0, 64, 3))
def test_signed_part(k):
    """A signed part is its bits: s = -1 is 3'b111 in {s, x[2:0]}."""
    cons = [("bvuge", "bool", ("concat", 6, S, _ex(2, 0)), _c(k, 6)),
            ("bvult", "bool", _ex(7, 3), _c(3, 5))]
    b = SolveProblemBuilder()
    b.add_var(0, width=8, is_signed=False, lo=0, hi=255)
    b.add_var(1, width=3, is_signed=True, lo=-4, hi=3)
    ids = {"x": 0, "s": 1}
    for c in cons:
        b.add_constraint(_lower(b, c, ids))
    truth = {(xv, sv): all(ev(c, {"x": xv, "s": sv & 7}) for c in cons)
             for xv in range(256) for sv in range(-4, 4)}
    try:
        ctx = SolveCtx(b.finalize()[0])
    except CompileUnsatError:
        assert not any(truth.values())
        return
    except CompileIncompleteError:
        return
    with ctx:
        for (xv, sv), want in truth.items():
            ctx.reset()
            cp = ctx.checkpoint()
            rc = ctx.solve(seed=1) if ctx.pin(0, xv) and ctx.pin(1, sv) else SOLVE_UNSAT
            ctx.restore(cp)
            assert (rc == SOLVE_OK) == want, (k, xv, sv, rc)
