"""The bit-blast engine's concrete evaluator (`bb_eval` in dvs_bbsolver.c) on
unsigned div/mod.

With every var pinned, the pin fast path answers from bb_eval alone. Here every
4-bit (x, y, q) for `q == x op y` must get the verdict SMT-LIB gives (zero
divisor: bvudiv is all-ones, bvurem the dividend), and every answer must come
from the fast path. DV_SIM_CHECK=1 makes the engine re-check each fast-path
verdict with SAT and abort on a mismatch.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_bb_eval_divmod.py
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"

W = 4
M = (1 << W) - 1


def _ref(op: str, x: int, y: int) -> int:
    if op == "bvudiv":
        return x // y if y else M
    return x % y if y else x


@pytest.mark.parametrize("op", ["bvudiv", "bvurem"])
def test_divmod_exhaustive(op):
    s = ["(set-logic QF_BV)"]
    s += [f"(declare-fun {v} () (_ BitVec {W}))" for v in ("x", "y", "q")]
    expect = []
    for x in range(M + 1):
        for y in range(M + 1):
            t = _ref(op, x, y)
            for qv in range(M + 1):
                s.append(f"(push 1)(assert (= x #x{x:x}))(assert (= y #x{y:x}))"
                         f"(assert (= q #x{qv:x}))(assert (= q ({op} x y)))"
                         "(check-sat)(pop 1)")
                expect.append("sat" if qv == t else "unsat")
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_ENGINE="bitblast", DV_SIM_CHECK="1", DV_BB_STATS="1")
    p = subprocess.run([str(_BIN)], input="\n".join(s) + "\n",
                       capture_output=True, text=True, env=env, timeout=300)
    assert "MISMATCH" not in p.stderr, p.stderr[-2000:]
    got = re.findall(r"^(sat|unsat|unknown)$", p.stdout, re.M)
    assert got == expect
    # Every verdict came from bb_eval (pinfast 1 = sat, 2 = unsat; 0 = miss).
    pf = re.findall(r"\[bb-pinfast\] pinfast=(\d)", p.stderr)
    assert len(pf) == len(expect) and "0" not in pf
