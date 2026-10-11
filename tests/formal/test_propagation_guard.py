"""A propagation that cannot converge must end in an answer, not exhaust memory.

B61: `x < x`, `(x+y) > (x+y)` (identical sub-terms get separate auxiliary
variables) and `x > (x | y)` (B30) make the bound propagators climb one
value per round. At 31 bits or more that is 2^31+ rounds with no deadline
check reached, and the trail grew until the process was killed for memory --
on a 63-bit `x < x`. Same-variable compares now conflict at once; anything
else that climbs is stopped by a deadline and trail cap inside propagation,
which make the solve a timeout (never unsat), so SMT-LIB2 escalates to
bitblast. Found by the soundness campaign's wide mode.
"""
from __future__ import annotations

import resource
import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"


def _limit():
    resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))   # 2 GiB


@pytest.mark.parametrize("width", [31, 32, 63, 64])
@pytest.mark.parametrize("assertion,expect", [
    ("(bvult x x)", "unsat"),
    ("(distinct x x)", "unsat"),
    ("(bvugt (bvadd x y) (bvadd x y))", "unsat"),
    ("(bvugt x (bvor x y))", "unsat"),
    ("(bvult y (bvor y (_ bv1 {w})))", "sat"),
])
def test_climbing_compares_answer_within_bounds(width, assertion, expect):
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    a = assertion.format(w=width)
    script = (f"(set-logic QF_BV)(declare-const x (_ BitVec {width}))"
              f"(declare-const y (_ BitVec {width}))(assert {a})(check-sat)")
    r = subprocess.run([str(_EXE)], input=script, capture_output=True, text=True,
                       timeout=60, preexec_fn=_limit)
    assert r.stdout.split()[:1] == [expect], (r.returncode, r.stderr[-500:])


# B103: a pin that sets a climb going. `ite(c, x, y) < v <= y` with x pinned
# high drives y up one value a round (the ite's hull takes y's lower bound);
# the pin hit the trail cap, reported the abandoned propagation as a conflict,
# and a satisfiable pin read as unsat. The abort flag then failed every pin
# after it until the next solve. Found by the wide campaign's protocol door,
# as `ite(c, x, y) < y` -- which compile now rewrites to `c && x < y`
# (test_ite_self.py), so the climb goes through v here.

def _ite_climb(width):
    from tests.formal.soundness.doors import _lower
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.ctx import SolveCtx
    x, y, z, v = (("var", n, width) for n in "xyzv")
    r = ("ite", width, ("bvult", "bool", ("const", 0, width), z), x, y)
    b = SolveProblemBuilder()
    for i in range(4):
        b.add_var(i, width=width, is_signed=False, lo=0, hi=(1 << width) - 1)
    for c in (("bvult", "bool", r, v), ("bvule", "bool", v, y)):
        b.add_constraint(_lower(b, c, {"x": 0, "y": 1, "z": 2, "v": 3}))
    return SolveCtx(b.finalize()[0])


@pytest.mark.parametrize("width", [32, 33, 64])
def test_pin_that_climbs_is_not_a_conflict(width):
    pytest.importorskip("dv_solve")
    from dv_solve.ctx import SOLVE_OK, SOLVE_UNSAT
    big = (1 << (width - 1)) + 12345
    with _ite_climb(width) as ctx:
        for v in (big, 0x414c343c, 5):
            cp = ctx.checkpoint()
            assert ctx.pin(0, v), hex(v)
            rc = ctx.solve(seed=1, max_restarts=5)
            assert rc != SOLVE_UNSAT, hex(v)          # sat or a timeout, never unsat
            if rc == SOLVE_OK:
                assert ctx.get_value(0) & ((1 << width) - 1) == v
            ctx.restore(cp)
    with _ite_climb(width) as ctx:
        assert ctx.pin(0, big)
        assert ctx.propagate_only() == 0               # not refuted


def test_abort_does_not_poison_later_pins():
    pytest.importorskip("dv_solve")
    with _ite_climb(32) as ctx:
        cp = ctx.checkpoint()
        assert ctx.pin(0, 0x80000000)
        ctx.restore(cp)
        assert ctx.pin(0, 5)
