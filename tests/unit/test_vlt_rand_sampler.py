"""--mode=verilator behaviour for Verilator's `+verilator+rand+sampler+solver`.

With that runtime option the solver samples: Verilator skips its own sampling
(UniGen2 cells, diversity pins) and takes the model dv-solve returns as the
random result, so
each randomize() is one seeded `check-sat`:

- **Driver seed.** Verilator sends `(set-option :random-seed N)` from the
  object's random state before each query. The same seed must give the same
  model, also within one session where dv-solve caches solved instances, and
  different seeds must give different models.
- **Large `inside` sets.** riscv-dv randomizes a 32-bit field under several
  `inside` sets of 60-500 values, which Verilator emits as `bvor` / `or` trees of
  reified equalities. Past 16 disjuncts these compiled to one guard per value
  and every solve took ~22 ms of trial and error; they now become one
  set-membership constraint. The model must still be a member of every set.
- **`inside` on a 32-bit unsigned variable.** The CDCL set propagator was chosen
  by the constants' width, and its 32-bit form reads inline bounds that only
  narrower variables keep, so it never rejected a non-member: every model
  failed validation and the query fell back to bit-blast.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest


_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
_needs_exe = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")

_PRELUDE = (
    "(set-option :produce-models true)\n"
    "(set-logic QF_ABV)\n"
    "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
    "(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))\n"
)
_VAL = re.compile(r"\(([\w.]+) #b([01]+)\)")

# Three membership sets and an exclusion over one 32-bit field, in the shapes
# riscv-dv's generated constraints take under Verilator.
_SET_A = list(range(0, 0x200, 3))                        # 171 values, bvor of __Vbv
_SET_B = list(range(0x18, 0x200, 7))                     # 70 values, under (=> (__Vbool #b1) ...)
_SET_C = list(range(0, 509))                             # Bool `or`, contiguous
_EXCL = [0x18 + 7 * k for k in range(0, 70, 9)]          # (not (inside ...)), small
_MEMBERS = (set(_SET_A) & set(_SET_B) & set(_SET_C)) - set(_EXCL)


def _bvor(xs):
    return "(bvor #b0 " + " ".join(f"(__Vbv (= #x{c:08x} x))" for c in xs) + ")"


def _membership_problem() -> str:
    s = "(declare-fun x () (_ BitVec 32))\n"
    s += f"(assert (= #b1 {_bvor(_SET_A)}))\n"
    s += f"(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool {_bvor(_SET_B)})))))\n"
    s += ("(assert (= #b1 (__Vbv (or "
          + " ".join(f"(= x (_ bv{c} 32))" for c in _SET_C) + "))))\n")
    s += f"(assert (= #b1 (__Vbv (not (__Vbool {_bvor(_EXCL)})))))\n"
    return s


def _session(problem: str, seeds) -> str:
    out = ""
    for seed in seeds:
        out += f"(set-option :random-seed {seed})\n" + _PRELUDE + problem
        out += "(check-sat)\n(get-value (x))\n(reset)\n"
    return out


def _run(script: str, tmp_path: Path):
    log = tmp_path / "dv.log"
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"], input=script,
                       capture_output=True, text=True, timeout=60,
                       env={"DV_LOG": str(log), "PATH": "/usr/bin:/bin"})
    verdicts = [l for l in r.stdout.split() if l in ("sat", "unsat", "unknown")]
    vals = [int(v, 2) for _n, v in _VAL.findall(r.stdout)]
    return verdicts, vals, log.read_text() if log.exists() else ""


@_needs_exe
def test_random_seed_reproduces_model(tmp_path):
    prob = ("(declare-fun x () (_ BitVec 16))\n(declare-fun y () (_ BitVec 16))\n"
            "(assert (bvult x #x1000))\n(assert (bvugt y x))\n")
    seeds = [7, 8, 7, 9, 8, 7]
    verdicts, vals, _ = _run(_session(prob, seeds), tmp_path)
    assert verdicts == ["sat"] * len(seeds)
    by_seed = {}
    for seed, v in zip(seeds, vals):
        assert by_seed.setdefault(seed, v) == v, f"seed {seed} gave {by_seed[seed]} then {v}"
    assert len(set(by_seed.values())) == 3, by_seed


@_needs_exe
def test_large_inside_sets_stay_on_cdcl(tmp_path):
    seeds = list(range(1, 41))
    verdicts, vals, log = _run(_session(_membership_problem(), seeds), tmp_path)
    assert verdicts == ["sat"] * len(seeds)
    assert all(v in _MEMBERS for v in vals), sorted(set(vals) - _MEMBERS)
    assert len(set(vals)) >= 10, "the seeds should spread over the members"
    assert "model-validation" not in log
    assert "bitblast" not in log, log[:2000]
    # As a guard per value this took ~22 ms a solve; as one set, ~0.2 ms.
    times = sorted(int(l.split()[1]) for l in log.splitlines() if l.startswith("vlt-time"))
    assert len(times) == len(seeds) and times[len(times) // 2] < 5000, times


@_needs_exe
def test_inside_on_unsigned_32bit_var(tmp_path):
    # Every constant fits a signed 32-bit int, which is what picked the 32-bit
    # propagator; the variable itself is unsigned 32-bit (64-bit bounds).
    members = [0x10000 * k + 3 for k in range(1, 24)]
    prob = "(declare-fun x () (_ BitVec 32))\n" + f"(assert (= #b1 {_bvor(members)}))\n"
    seeds = list(range(1, 21))
    verdicts, vals, log = _run(_session(prob, seeds), tmp_path)
    assert verdicts == ["sat"] * len(seeds)
    assert set(vals) <= set(members), sorted(set(vals) - set(members))
    assert "model-validation" not in log and "bitblast" not in log, log[:2000]
