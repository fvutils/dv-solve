"""A multi-bit variable read as a truth value inside Boolean structure (B98).

`!s || a`, with s two bits wide and `a` a shape the OR flattener does not take,
compiles through Boolean guards. The guard for `s` was s itself, so s = 2 made
`!s` (encoded as 1 - g) -1 and the solve wrong-unsat. A wider variable now
gets a guard `g <-> s != 0`. At the root, `s` and `!s` compiled to `s >= 1`
and `s <= 0`, wrong for a signed s, which is true when negative.

Exhaustive over the variables' values: each assignment is pinned and the
sat/unsat answer compared with the IR evaluator.
"""
from __future__ import annotations

import pytest

from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import ev

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder            # noqa: E402
from dv_solve.ctx import (CompileIncompleteError, CompileUnsatError,  # noqa: E402
                          SOLVE_OK, SOLVE_UNSAT, SolveCtx)

Z = ("var", "z", 4)


def _check(cons, s_width, s_signed):
    widths = {"s": s_width, "z": 4}
    ids = {"s": 0, "z": 1}
    b = SolveProblemBuilder()
    lo, hi = (-(1 << (s_width - 1)), (1 << (s_width - 1)) - 1) if s_signed \
        else (0, (1 << s_width) - 1)
    b.add_var(0, width=s_width, is_signed=s_signed, lo=lo, hi=hi)
    b.add_var(1, width=4, is_signed=False, lo=0, hi=15)
    for c in cons:
        b.add_constraint(_lower(b, c, ids))
    prob, _ = b.finalize()
    m = (1 << s_width) - 1
    truth = {(sv, zv): all(ev(c, {"s": sv & m, "z": zv}) for c in cons)
             for sv in range(lo, hi + 1) for zv in range(16)}
    try:
        ctx = SolveCtx(prob)
    except CompileUnsatError:
        assert not any(truth.values()), cons
        return
    except CompileIncompleteError:
        return                      # declined: not a wrong answer
    with ctx:
        for (sv, zv), want in truth.items():
            ctx.reset()
            cp = ctx.checkpoint()
            pinned = ctx.pin(0, sv) and ctx.pin(1, zv)
            rc = ctx.solve(seed=1) if pinned else SOLVE_UNSAT
            ctx.restore(cp)
            assert rc in (SOLVE_OK, SOLVE_UNSAT), (cons, sv, zv, rc)
            assert (rc == SOLVE_OK) == want, \
                (cons, (sv, zv), "sat" if rc == SOLVE_OK else "unsat")


F = ("=", "bool", ("extract", 2, 2, 1, Z), ("const", 2, 2))      # not flattened
G = ("bvult", "bool", Z, ("const", 9, 4))                        # var-const


FORMS = {
    "!s || f": lambda s, f, g: ("or", "bool", ("not", "bool", s), f),
    "!s || g": lambda s, f, g: ("or", "bool", ("not", "bool", s), g),
    "s || f": lambda s, f, g: ("or", "bool", s, f),
    "!(s && f)": lambda s, f, g: ("not", "bool", ("and", "bool", s, f)),
    "!(s || f)": lambda s, f, g: ("not", "bool", ("or", "bool", s, f)),
    "s -> f": lambda s, f, g: ("=>", "bool", s, f),
    "f -> !s": lambda s, f, g: ("=>", "bool", f, ("not", "bool", s)),
    "(s && g) || f": lambda s, f, g: ("or", "bool", ("and", "bool", s, g), f),
    "s": lambda s, f, g: s,
    "!s": lambda s, f, g: ("not", "bool", s),
    "f -> s": lambda s, f, g: ("=>", "bool", f, s),
    "!(!s)": lambda s, f, g: ("not", "bool", ("not", "bool", s)),
}


@pytest.mark.parametrize("form", sorted(FORMS))
@pytest.mark.parametrize("s_width,s_signed", [(1, False), (2, False), (3, True), (8, False)])
def test_truth_value_guard(form, s_width, s_signed):
    _check([FORMS[form](("var", "s", s_width), F, G)], s_width, s_signed)
