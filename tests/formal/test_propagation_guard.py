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
