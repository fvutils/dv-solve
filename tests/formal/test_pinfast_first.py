"""The one-shot check and the reused-context draw try the pinned-constant path first.

A dist pick pins a variable to a constant (dist_weight_div: a == 13, k == 3).
When every assert holds whatever the unforced bits are, `_pinfast` fills them
from the seed: a uniform draw over every solution, with no division sampler.
`dvs_bbsolver_check` (a new api-cache context) and `_fresh_base` (a reused
one) now call it before `_divsamp`. A conflicting pin is unsat. Runs the
api-cache pass of bench_vlt_replay with DV_SIM_CHECK=1 and reads its per-field
distinct counts and the `[bb-pinfast]` lines. DV_PINFAST_FIRST=0 = old order.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_pinfast_first.py
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
_DECLS = (("a", 8), ("b", 8), ("k", 4))


def _session(asserts) -> str:
    """asserts(i) -> the asserts of randomize() call i."""
    out = []
    for i in range(_N):
        out.append("(reset)\n(set-option :produce-models true)\n"
                   f"(set-option :random-seed {1000 + i})\n(set-logic QF_ABV)\n"
                   "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n")
        out += [f"(declare-fun {v} () (_ BitVec {w}))\n" for v, w in _DECLS]
        out += [f"(assert (= #b1 (__Vbv {a})))\n" for a in asserts(i)]
        out.append(f"(check-sat)\n(get-value ({' '.join(v for v, _ in _DECLS)}))\n")
    return "".join(out)


def _run(tmp_path: Path, session: str, **envs) -> tuple:
    f = tmp_path / "s.smt2"
    f.write_text(session)
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_SIM_CHECK="1", DV_BB_STATS="1", **envs)
    p = subprocess.run([str(_BIN), str(f), "--reps", "1", "--passes", "api-cache",
                        "--quality"], capture_output=True, text=True, env=env, timeout=120)
    out = p.stdout + p.stderr
    assert "MISMATCH" not in out and " 0 invalid" in out, out[-2000:]
    d = {m[0]: int(m[1]) for m in re.findall(r"field (\w+) n \d+ distinct (\d+)", out)}
    return d, len(re.findall(r"\[bb-pinfast\] pinfast=1", out)), out


@pytest.fixture(scope="module", autouse=True)
def _built():
    if not _BIN.exists():
        pytest.skip(f"{_BIN} not built")


# The dist4 shape: a and k pinned through an ite on a constant, b half-forced.
def _pins(i):
    return [f"(ite (bvule #x02 #x04) (= a (_ bv{13 + i % 3} 8)) (= a #xc6))",
            "(or (= k #x3) (= k #xf))", "(= k #x3)",
            "(= ((_ extract 3 0) b) #x5)"]


@pytest.mark.parametrize("vary", [True, False], ids=["new", "reused"])
def test_pinned_spread(tmp_path, vary):
    d, hits, out = _run(tmp_path, _session(_pins if vary else (lambda i: _pins(0))))
    assert d["k"] == 1 and d["a"] == (3 if vary else 1), d
    assert d["b"] >= 14, d          # 16 values (b[3:0] = 5), ~16 expected of 200
    assert hits >= _N - 1, hits     # every draw from the pinned path
    d0, hits0, _ = _run(tmp_path, _session(_pins if vary else (lambda i: _pins(0))),
                        DV_PINFAST_FIRST="0")
    assert d0["k"] == 1 and hits0 < hits


# A pin that contradicts a range, a forced bit or another pin: the pinned path
# says unsat on every call (bitblast engine), in either order.
@pytest.mark.parametrize("pins", [
    ["(= a #x0d)", "(bvugt a #x20)"],
    ["(= ((_ extract 3 0) b) #x5)", "(= ((_ extract 0 0) b) #b0)"],
    ["(ite (bvule #x02 #x04) (= a #x0d) (= a #xc6))", "(= ((_ extract 3 0) a) #x5)"]],
    ids=["range", "bit", "ite"])
@pytest.mark.parametrize("first", ["1", "0"])
def test_pinned_unsat(tmp_path, pins, first):
    _, _, out = _run(tmp_path, _session(lambda i: pins), DV_ENGINE="bitblast",
                     DV_PINFAST_FIRST=first)
    m = re.search(r"(\d+) unsat\)", out)
    assert m and int(m.group(1)) == _N, out[-2000:]
    assert ("pinfast=2" in out) == (first == "1")    # old order: never reached
