"""The division sampler checks randc exclusions `v != c` by lookup.

randc_basic adds one `(not (= b K))` per value already drawn, up to ~40.
`_divsamp` used to evaluate every such leaf for every candidate value of b
while it enumerated b's domain; `_ds_excl` now recognises them, the loops
skip them and reject x by binary search in the sorted constants. The domain,
and so every draw, is the same as before. Runs the api-cache pass of
bench_vlt_replay with DV_SIM_CHECK=1 and reads its per-field distinct counts
and the `[bb-divsamp] ... excl=` lines. DV_DS_EXCL=0 = old evaluation.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_ds_excl.py
"""
from __future__ import annotations

import os
import random
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "bench_vlt_replay"
_N = 200
_DECLS = (("a", 4), ("b", 6), ("c", 8))
_BASE = ["(= #b1 (__Vbv (bvult b #b101000)))",
         "(= #b1 (__Vbv (bvugt c ((_ repeat 1) (concat #x0 a)))))"]
# The spellings of b != k a frontend emits.
_FORMS = ["(not (= b (_ bv{k} 6)))", "(not (= (_ bv{k} 6) b))",
          "(= #b1 (__Vbv (not (= b (_ bv{k} 6)))))"]


def _session(asserts) -> str:
    """asserts(i) -> the asserts of randomize() call i."""
    out = []
    for i in range(_N):
        out.append("(reset)\n(set-option :produce-models true)\n"
                   f"(set-option :random-seed {1000 + i})\n(set-logic QF_ABV)\n"
                   "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n")
        out += [f"(declare-fun {v} () (_ BitVec {w}))\n" for v, w in _DECLS]
        out += [f"(assert {a})\n" for a in _BASE + asserts(i)]
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
    ex = sum(int(m) for m in re.findall(r"\[bb-divsamp\].* excl=(\d+)", out))
    m = re.search(r"(\d+) unsat\)", out)
    return d, ex, int(m.group(1)) if m else -1, out


@pytest.fixture(scope="module", autouse=True)
def _built():
    if not _BIN.exists():
        pytest.skip(f"{_BIN} not built")


def _randc(i):
    """The values drawn so far in a randc cycle: a growing random subset."""
    r = random.Random(i)
    ks = r.sample(range(40), i % 39)
    return [_FORMS[j % len(_FORMS)].format(k=k) for j, k in enumerate(ks)] + \
        [f"(not (= a (_ bv{k} 4)))" for k in r.sample(range(16), i % 15)]


def test_excl_same_draws(tmp_path):
    d, ex, unsat, _ = _run(tmp_path, _session(_randc))
    d0, ex0, unsat0, _ = _run(tmp_path, _session(_randc), DV_DS_EXCL="0")
    assert ex > 0 and ex0 == 0, (ex, ex0)
    assert unsat == unsat0 == 0
    assert d == d0, (d, d0)                 # same domains, same seeded draws
    assert d["b"] >= 35 and d["a"] >= 14, d


# One value left: every draw is it. None left: unsat on every call.
@pytest.mark.parametrize("excl", ["1", "0"])
def test_excl_one_left(tmp_path, excl):
    keep = lambda i: [_FORMS[k % 3].format(k=k) for k in range(40) if k != 17]
    d, ex, unsat, out = _run(tmp_path, _session(keep), DV_DS_EXCL=excl)
    assert unsat == 0 and d["b"] == 1, d
    assert (ex > 0) == (excl == "1")


@pytest.mark.parametrize("excl", ["1", "0"])
def test_excl_none_left(tmp_path, excl):
    none = lambda i: [_FORMS[k % 3].format(k=k) for k in range(40)]
    _, _, unsat, _ = _run(tmp_path, _session(none), DV_DS_EXCL=excl)
    assert unsat == _N
