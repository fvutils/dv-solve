"""Exhaustive validation of dv-solve's signed bvsdiv / bvsrem lowering vs z3.

The SMT2 frontend lowers `bvsdiv`/`bvsrem` to the SMT-LIB signed-division
definition (ite over unsigned bvudiv/bvurem with sign muxing) and force-routes
such problems to the bitblast engine (the CDCL bounds engine leaves the composed
result unpinned -> wrong model). This test pins every (a, b) at a small width and
checks dv-solve's model value against z3's, including division/remainder by zero
(SMT-LIB: bvudiv-by-0 = all-ones, bvurem-by-0 = dividend).

Guards against both a lowering regression and the CDCL-routing regression: if
signed div/rem ever stops force-routing to bitblast, CDCL returns wrong values
and this fails.

Note: bvsmod is deliberately NOT lowered (an earlier attempt was only 219/256
correct); it stays at honest `unknown`. If bvsmod support is added, extend this.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DVSOLVE = _REPO / "build" / "dv-solve-smt2"
_Z3 = _REPO / "packages" / "python" / "bin" / "z3"
_WIDTH = 4  # exhaustive over all 16x16 (a,b) pairs


def _val(out: str):
    m = re.search(r"\(r (#[bx][0-9a-fA-F]+|\(_ bv\d+ \d+\))", out)
    if not m:
        return None
    s = m.group(1)
    if s.startswith("#b"):
        return int(s[2:], 2)
    if s.startswith("#x"):
        return int(s[2:], 16)
    return int(re.search(r"bv(\d+)", s).group(1))


def _solve(exe_args, smt, use_z3=False):
    proc = subprocess.run(exe_args, input=smt, capture_output=True, text=True, timeout=15)
    return proc.stdout


pytestmark = pytest.mark.skipif(
    not (_DVSOLVE.exists() and _Z3.exists()),
    reason="dv-solve-smt2 or z3 not built",
)


@pytest.mark.parametrize("op", ["bvsdiv", "bvsrem", "bvsmod"])
def test_signed_divrem_matches_z3(op):
    w = _WIDTH
    mism = []
    for a in range(1 << w):
        for b in range(1 << w):
            smt = (f"(set-logic QF_BV)(declare-fun r () (_ BitVec {w}))"
                   f"(assert (= r ({op} (_ bv{a} {w}) (_ bv{b} {w}))))"
                   f"(check-sat)(get-value (r))")
            zv = _val(_solve([str(_Z3), "-smt2", "-in"], smt))
            dv = _val(_solve([str(_DVSOLVE), "/dev/stdin"], smt))
            if zv is None:
                continue  # z3 gave no model (shouldn't happen)
            if dv != zv:
                mism.append((a, b, zv, dv))
    assert not mism, (
        f"{op}: {len(mism)} mismatches vs z3 (first 8: {mism[:8]}). "
        "Either the lowering broke or signed div/rem stopped routing to bitblast."
    )
