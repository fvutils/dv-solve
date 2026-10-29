"""The resample path skips the flip sweep after a pure-XOR affine model.

When `_fresh_base` takes its model from the affine check over a pure XOR
system (no opaque gate, no clause) and every variable input bit is in the
cone, the model is already a uniform draw, and the flip sweep is skipped
(construct.xor_parity: each single-bit flip breaks the parity). An opaque
constraint, or a variable outside the cone, must keep the sweep. A carry
chain (r = q + 1) must still draw from every solution. Runs the
api-cache pass of bench_vlt_replay and reads its per-field distinct counts.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_affine_exact.py
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "bench_vlt_replay"
_N = 200
_PAR = ("".join(["(bvxor "] * 31) + "((_ extract 0 0) data)"
        + "".join(f" ((_ extract {i} {i}) data))" for i in range(1, 32)))
_BASE = [f"(= p {_PAR})", "(= p #b1)", "(= lo ((_ extract 7 0) data))",
         "(= ((_ extract 0 0) lo) #b0)"]


def _session(extra: list, decls=(("data", 32), ("lo", 8), ("p", 1), ("q", 8))) -> str:
    out = []
    for i in range(_N):
        out.append("(reset)\n(set-option :produce-models true)\n"
                   f"(set-option :random-seed {1000 + i})\n(set-logic QF_ABV)\n"
                   "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n")
        out += [f"(declare-fun {v} () (_ BitVec {w}))\n"
                for v, w in decls]
        out += [f"(assert (= #b1 (__Vbv {a})))\n" for a in _BASE + extra]
        out.append(f"(check-sat)\n(get-value ({' '.join(v for v, _ in decls)}))\n")
    return "".join(out)


def _distinct(tmp_path: Path, extra: list, exact: str = "1", session: str = "",
              **envs) -> dict:
    f = tmp_path / "s.smt2"
    f.write_text(session or _session(extra))
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_SIM_CHECK="1", DV_AFFINE_EXACT=exact, **envs)
    p = subprocess.run([str(_BIN), str(f), "--reps", "1", "--passes", "api-cache",
                        "--quality"], capture_output=True, text=True, env=env, timeout=120)
    out = p.stdout + p.stderr
    assert "MISMATCH" not in out and " 0 invalid" in out, out[-2000:]
    return {m[0]: int(m[1]) for m in re.findall(r"field (\w+) n \d+ distinct (\d+)", out)}


@pytest.fixture(scope="module", autouse=True)
def _built():
    if not _BIN.exists():
        pytest.skip(f"{_BIN} not built")


# q < 100 is an opaque gate and a clause: the affine model is not uniform and
# the sweep must run. (q = q) or (p = 0) keeps q in the AIG but out of the
# cone: the affine model leaves it 0 and only the sweep fills it.
@pytest.mark.parametrize("extra", [[], ["(bvult q #x64)"], ["(or (= q q) (= p #b0))"]],
                         ids=["pure", "opaque", "off-cone"])
def test_skip_keeps_spread(tmp_path, extra):
    d = _distinct(tmp_path, extra)
    assert d["data"] >= _N - 2
    assert d["lo"] >= 80            # 128 even values, ~103 expected
    assert d["q"] >= 70, d          # >= 100 values, ~86+ expected
    assert d == _distinct(tmp_path, extra, exact="0") or extra == []


# r = q + 1 over the parity base: each carry gate takes a row's pivot, so the
# seeded model left the row unenforced, failed on the AIG, and the seed-0
# model (data 2, q 0) stood for every draw; no single-bit flip moves it.
# DV_AFFINE_REPIVOT=0 is the old elimination (checked to collapse).
def test_carry_repivot(tmp_path):
    s = _session(["(= r (bvadd q #x01))"],
                 (("data", 32), ("lo", 8), ("p", 1), ("q", 8), ("r", 8)))
    d = _distinct(tmp_path, [], session=s)
    assert d["data"] >= _N - 2
    assert d["q"] >= 110 and d["r"] == d["q"], d     # ~139 expected of 256
    assert _distinct(tmp_path, [], session=s, DV_AFFINE_REPIVOT="0")["q"] < 10
