"""Alignment to a random size compiles to slice implications.

AXI testbenches align an address to a random transfer size three ways:
`(addr & ((1 << size) - 1)) == 0`, `addr % (1 << size) == 0` and
`size == k -> addr[k-1:0] == 0`. The first two left the engine a mask it
could not propagate (or, once the address was split into fields, a check it
could only make after drawing them: ~120 us a solve against ~11 us for the
slice form). With size a narrow unsigned variable, compile rewrites both to
the third (dvs_sv_rewrite_masks, every front end).

The rewrite must be an equivalence: checked exhaustively over an 8-bit x, a
shift variable s and a 4-bit y it may share constraints with, by pinning
every assignment and comparing sat/unsat with the IR's evaluator.
"""
from __future__ import annotations

import pytest

from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import ev

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder            # noqa: E402
from dv_solve.ctx import (CompileIncompleteError, CompileUnsatError,  # noqa: E402
                          SOLVE_OK, SOLVE_UNSAT, SolveCtx)


def _check(cons, widths):
    names = sorted(widths)
    ids = {n: i for i, n in enumerate(names)}
    b = SolveProblemBuilder()
    for n in names:
        b.add_var(ids[n], width=widths[n], is_signed=False, lo=0, hi=(1 << widths[n]) - 1)
    for c in cons:
        b.add_constraint(_lower(b, c, ids))
    asg = [{}]
    for n in names:
        asg = [dict(a, **{n: v}) for a in asg for v in range(1 << widths[n])]
    truth = [(a, all(ev(c, a) for c in cons)) for a in asg]
    try:
        ctx = SolveCtx(b.finalize()[0])
    except CompileUnsatError:
        assert not any(t for _, t in truth), cons
        return
    except CompileIncompleteError:
        pytest.fail(f"declined: {cons}")
    with ctx:
        for a, want in truth:
            ctx.reset()
            cp = ctx.checkpoint()
            ok = all(ctx.pin(ids[n], v) for n, v in a.items())
            rc = ctx.solve(seed=1) if ok else SOLVE_UNSAT
            ctx.restore(cp)
            assert (rc == SOLVE_OK) == want, (cons, a, rc)


def _x(w):
    return ("var", "x", w)


def _shift(sw, w):
    s = ("var", "s", sw)
    return ("bvshl", w, ("const", 1, w), ("zero_extend", w, s) if sw < w else s)


def _mask(sw, w, swap=False):
    m = ("bvsub", w, _shift(sw, w), ("const", 1, w))
    return ("bvand", w, m, _x(w)) if swap else ("bvand", w, _x(w), m)


def _mod(sw, w):
    return ("bvurem", w, _x(w), _shift(sw, w))


FORMS = {"mask": lambda sw, w: _mask(sw, w), "mask-swapped": lambda sw, w: _mask(sw, w, True),
         "mod": _mod}


@pytest.mark.parametrize("form", sorted(FORMS))
@pytest.mark.parametrize("sw", [1, 2, 3, 4])     # 4: 1 << 15 leaves 8 bits, not rewritten
@pytest.mark.parametrize("cmp", ["=", "distinct"])
@pytest.mark.parametrize("zero_lhs", [False, True])
def test_alone(form, sw, cmp, zero_lhs):
    if form == "mod" and sw == 4:
        pytest.skip("1 << s reaches 0: x % 0 is x in SMT-LIB, unknown in SystemVerilog")
    a, z = FORMS[form](sw, 8), ("const", 0, 8)
    _check([(cmp, "bool", z, a) if zero_lhs else (cmp, "bool", a, z)], {"x": 8, "s": sw})


@pytest.mark.parametrize("form", sorted(FORMS))
@pytest.mark.parametrize("sw", [2, 3])
def test_with_fields(form, sw):
    """Next to the 4K-boundary shape: a field of x linked to y, a window."""
    a = FORMS[form](sw, 8)
    _check([("=", "bool", a, ("const", 0, 8)),
            ("bvule", "bool", ("bvadd", 8, ("zero_extend", 4, ("extract", 4, 3, 0, _x(8))),
                               ("zero_extend", 4, ("var", "y", 4))), ("const", 16, 8)),
            ("bvuge", "bool", _x(8), ("const", 0x30, 8))],
           {"x": 8, "s": sw, "y": 4})


@pytest.mark.parametrize("form", sorted(FORMS))
def test_under_structure(form):
    """Negated, and under an implication."""
    a = FORMS[form](3, 8)
    y = ("var", "y", 4)
    _check([("not", "bool", ("=", "bool", a, ("const", 0, 8))),
            ("=>", "bool", ("bvult", "bool", y, ("const", 5, 4)),
             ("=", "bool", FORMS[form](3, 8), ("const", 0, 8)))],
           {"x": 8, "s": 3, "y": 4})
    _check([("or", "bool", ("bvult", "bool", y, ("const", 3, 4)),
             ("distinct", "bool", a, ("const", 0, 8)))],
           {"x": 8, "s": 3, "y": 4})


def test_narrow_word():
    """x narrower than the shift: bits above x are zero."""
    x = ("var", "x", 4)
    s = ("var", "s", 3)
    a = ("bvand", 8, ("zero_extend", 8, x),
         ("bvsub", 8, ("bvshl", 8, ("const", 1, 8), ("zero_extend", 8, s)), ("const", 1, 8)))
    _check([("=", "bool", a, ("const", 0, 8))], {"x": 4, "s": 3})
