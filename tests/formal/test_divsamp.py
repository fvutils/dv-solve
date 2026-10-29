"""The bit-blast engine's div/mul sampler (`_divsamp` in dvs_bbsolver.c).

`_divsamp` draws the free vars from their unary hull, computes the vars a
`v == e` leaf defines, and checks the rest. Of a cycle of definitions it must
keep the one that leaves the narrow vars free: in `p == a * b` with
`(bvudiv p b) == a`, defining `a := p / b` leaves `p` free over 2^64 and every
draw wraps, while `p := a * b` keeps `a` and `b` in range. Every model is
checked against the constraints as written; DV_SIM_CHECK=1 makes the engine
re-check every fast-path answer with SAT and abort on a mismatch.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_divsamp.py
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"
_M64 = (1 << 64) - 1


def _run(text: str, divsamp: bool = True):
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_ENGINE="bitblast", DV_SIM_CHECK="1", DV_BB_STATS="1",
               DV_DIVSAMP="1" if divsamp else "0")
    p = subprocess.run([str(_BIN)], input=text, capture_output=True,
                       text=True, env=env, timeout=120)
    assert "MISMATCH" not in p.stderr, p.stderr[-2000:]
    verdicts = re.findall(r"^(sat|unsat|unknown)$", p.stdout, re.M)
    models = []
    for blk in re.findall(r"^\(\(.*?\)\)$", p.stdout, re.M | re.S):
        vals = {}
        for name, lit in re.findall(r"\((\w+) (#[bx][0-9a-fA-F]+)\)", blk):
            vals[name] = int(lit[2:], 2 if lit[1] == "b" else 16)
        models.append(vals)
    return verdicts, models, p.stderr


def _mul_div(seed: int) -> str:
    # As Verilator writes it (arith.mul_div_64); the wrapping is what put the
    # udiv definition first.
    return f"""(reset)
(set-option :produce-models true)
(set-option :random-seed {seed})
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(declare-fun a () (_ BitVec 64))
(declare-fun b () (_ BitVec 64))
(declare-fun p () (_ BitVec 64))
(assert (= #b1 (bvand (__Vbv (bvuge a #x0000000000000001)) (__Vbv (bvule a #x00000000ffffffff)))))
(assert (= #b1 (bvand (__Vbv (bvuge b #x0000000000000001)) (__Vbv (bvule b #x000000000000ffff)))))
(assert (= #b1 (__Vbv (= p (bvmul a b)))))
(assert (= #b1 (__Vbv (= (bvudiv p b) a))))
(check-sat)
(get-value (a b p))
"""


@pytest.fixture(scope="module", autouse=True)
def _built():
    if not _BIN.exists():
        pytest.skip(f"{_BIN} not built")


@pytest.mark.parametrize("divsamp", [True, False])
def test_mul_div_models(divsamp):
    n = 40
    text = "".join(_mul_div(1000 + i) for i in range(n))
    verdicts, models, err = _run(text, divsamp)
    assert verdicts == ["sat"] * n
    assert len(models) == n
    for m in models:
        a, b, p = m["a"], m["b"], m["p"]
        assert 1 <= a <= 0xFFFFFFFF and 1 <= b <= 0xFFFF
        assert p == (a * b) & _M64 and p // b == a
    if divsamp:
        # The sampler answers each call on its first draw, and the draws vary.
        hits = re.findall(r"\[bb-divsamp\].*tries=(\d+) -> sat", err)
        assert len(hits) == n and all(t == "1" for t in hits), err[-2000:]
        assert len({m["a"] for m in models}) >= n - 2
        assert len({m["b"] for m in models}) >= n // 2
