"""Push/pop sessions hold the CDCL context's pool flat (dynamic pool plan, P0.3).

A pop used to keep everything its scope had allocated in the context pool --
the scope's propagators, its variables' wide bounds, the regrown reset
snapshot -- so a long push/assert/check-sat/pop session (yosys-smtbmc,
Verilator's randomize-with under (push 1)) filled the pool, overflowed, and
from then on answered on bitblast or `unknown`. The C side checks the pool
use exactly (tests/c/test_checkpoint_pool.c); this drives the same pattern
through dv-solve-smt2 and checks the session never overflows.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
_needs_exe = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")

N_SCOPES = 10000


def _script(n: int) -> str:
    s = ["(set-logic QF_BV)",
         "(declare-fun a () (_ BitVec 8))", "(declare-fun b () (_ BitVec 8))",
         "(declare-fun w () (_ BitVec 40))",
         "(assert (bvult a b))", "(assert (bvugt w #x00000003e8))", "(check-sat)"]
    for i in range(n):
        k = 2 + i % 200
        s += ["(push 1)",
              # a fresh 40-bit term per scope: wide bounds + aux variables
              f"(declare-fun t () (_ BitVec 40))",
              f"(assert (= t (bvadd w (_ bv{k} 40))))",
              f"(assert (bvult (bvadd a b) (_ bv{k + 10} 8)))",
              "(check-sat)", "(pop 1)"]
    s.append("(check-sat)")
    return "\n".join(s) + "\n"


@_needs_exe
def test_pushpop_session_does_not_overflow():
    r = subprocess.run([str(_EXE), "--stats"], input=_script(N_SCOPES),
                       capture_output=True, text=True, timeout=600)
    verdicts = [ln for ln in r.stdout.split() if ln in ("sat", "unsat", "unknown")]
    assert verdicts == ["sat"] * (N_SCOPES + 2), set(verdicts)
    assert "pool-overflow" not in r.stderr
    assert "flush-failed" not in r.stderr
