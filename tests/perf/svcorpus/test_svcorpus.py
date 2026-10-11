"""Every corpus case is the class Verilator captured, and solves to valid models.

equiv: z3 proves the builder problem and Verilator's capture have the same
solutions (a hand translation that drifts from the source fails here).
draws: seeded solves return models that satisfy the capture (ir.ev, not the
builder's own evaluator) and validate_model().
"""
from __future__ import annotations

import random
import shutil

import pytest

from tests.perf.svcorpus import case as C

dv = pytest.importorskip("dv_solve")


@pytest.mark.parametrize("name", C.names())
def test_equivalent_to_capture(name):
    from tests.formal.soundness.ir import _z3
    if not _z3() and not shutil.which("z3"):
        pytest.skip("no z3")
    from tests.perf.svcorpus.equiv import check
    ok, why = check(name)
    assert ok, why


def _draw_params():
    out = []
    for n in C.names():
        known = getattr(C.load(n), "KNOWN_ISSUE", None)
        out.append(pytest.param(n, marks=pytest.mark.xfail(reason=known, strict=True))
                   if known else n)
    return out


@pytest.mark.parametrize("name", _draw_params())
def test_draws_are_valid(name):
    from dv_solve.ctx import SOLVE_OK, SolveCtx
    from tests.formal.soundness.ir import ev
    case = C.load(name)
    _, cons = C.capture(case)
    ids = C.ids(case)
    rng = random.Random(1)
    with SolveCtx(C.problem(case)) as ctx:
        for _ in range(50):
            ctx.reset()
            assert ctx.solve(seed=rng.getrandbits(62) + 1, time_limit_ms=2000) == SOLVE_OK
            m = {n: ctx.get_value(ids[n]) & ((1 << w) - 1) for n, w, _ in case.VARS}
            assert all(ev(c, m) for c in cons), m
            assert ctx.validate_model() == 0, m
