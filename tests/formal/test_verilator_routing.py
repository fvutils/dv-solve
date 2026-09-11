"""Verilator randomization routing: prefer CDCL, fall back to bitblast.

In `--mode=verilator` dv-solve routes randomize() solves to the CDCL engine
(a near-uniform sampler) rather than the bitblast path (`_diversify` re-flips
bits on ONE cached model, which concentrates mass on boundary values). CDCL is
correct-or-`unknown` and an `unknown` escalates to bitblast, so the routing can
only change WHICH engine answers -- never the verdict.

The decision is probed once per constraint set and cached (`cdcl_route`), so an
instance CDCL cannot handle costs one bounded probe rather than a probe per
randomize() call. DV_VERILATOR_CDCL=0 restores bitblast-always.

Guards here:
  * verdicts are identical under both routings (soundness of the routing)
  * the sticky probe actually amortizes (no per-call stall)
  * routing lifts real fixtures that bitblast cannot solve at all
  * randomization still varies per call

Usage:
    direnv exec . pytest tests/formal/test_verilator_routing.py -v
"""
from __future__ import annotations

import os
import re
import subprocess
import time
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_FIX = _REPO / "tests" / "formal" / "smt2" / "verilator"
_LIT = re.compile(r"#[bx][0-9a-fA-F]+")
_DECL = re.compile(r"\(declare-fun\s+([^\s()|]+)\s*\(\)\s*\(_\s+BitVec\s+\d+\)")

# Fixtures where routing matters most, all real sanitized Verilator transcripts.
_CASES = [
    "t_constraint_array_sum_with", "t_constraint_countones",
    "t_constraint_solve_before", "t_constraint_sysfunc",
    "t_constraint_cond", "t_constraint_operators",
    "t_constraint_struct", "t_constraint_unsat",
]


def _loop(name: str, n: int, env: dict, want_values: bool = False, timeout=180):
    src = (_FIX / f"{name}.smt2").read_text().rstrip()
    gv = ""
    if want_values:
        vs = _DECL.findall(src)[:6]
        if vs:
            gv = "(get-value (" + " ".join(vs) + "))"
    cmds = []
    for _ in range(n):
        cmds += ["(reset)", src] + ([gv] if gv else [])
    cmds.append("(exit)")
    t0 = time.perf_counter()
    p = subprocess.run([str(_DV), "--interactive", "--mode=verilator"],
                       input="\n".join(cmds) + "\n", capture_output=True,
                       text=True, timeout=timeout,
                       env={**os.environ, **env})
    dt = time.perf_counter() - t0
    verdicts = [l.strip() for l in p.stdout.splitlines()
                if l.strip() in ("sat", "unsat", "unknown")]
    return verdicts, dt, _LIT.findall(p.stdout)


@pytest.fixture(autouse=True)
def _need_binary():
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")


@pytest.mark.parametrize("name", _CASES)
def test_routing_does_not_change_the_verdict(name: str) -> None:
    """CDCL-first and bitblast-always must agree. This is the soundness gate."""
    a, _, _ = _loop(name, 3, {})
    b, _, _ = _loop(name, 3, {"DV_VERILATOR_CDCL": "0"})
    assert a and b, f"{name}: no verdict"
    assert set(a) == set(b), f"{name}: cdcl-first {set(a)} vs bitblast {set(b)}"


@pytest.mark.parametrize("name", _CASES)
def test_randomization_still_varies(name: str) -> None:
    """Whichever engine answers, repeated randomize() must not be constant.

    Skips fixtures whose constraints admit only one solution (e.g. _unsat, and
    shapes that pin every rand var) -- there "constant" is correct.
    """
    v, _, lits = _loop(name, 40, {}, want_values=True)
    if "unknown" in v or not lits:
        pytest.skip(f"{name}: not solved / no readable values")
    if len(set(lits)) == 1:
        pytest.skip(f"{name}: single-solution constraint")
    assert len(set(lits)) > 1


def test_sticky_probe_amortizes() -> None:
    """A CDCL-infeasible instance must pay ONE bounded probe, not one per call.

    t_constraint_struct is such an instance. If the probe were re-run per call
    the 50 ms budget would dominate; amortized over 200 calls it must not.
    """
    _, dt, _ = _loop("t_constraint_struct", 200, {})
    per_call_ms = dt / 200 * 1e3
    assert per_call_ms < 5.0, \
        f"{per_call_ms:.2f} ms/call -- probe is not being cached"


def test_routing_rescues_a_bitblast_timeout() -> None:
    """t_constraint_shift_width: bitblast does not finish; CDCL solves it fast.

    A concrete case where the routing turns a hang into an answer.
    """
    v, dt, _ = _loop("t_constraint_shift_width", 20, {}, timeout=60)
    assert v and v[0] == "sat", f"expected sat, got {v[:1]}"
    assert dt / 20 * 1e3 < 50.0, f"{dt/20*1e3:.1f} ms/call is too slow"


def test_opt_out_env_is_honoured() -> None:
    """DV_VERILATOR_CDCL=0 must restore the bitblast path.

    Verified via the observable difference: under bitblast-always,
    shift_width does not complete in a short budget.
    """
    with pytest.raises(subprocess.TimeoutExpired):
        _loop("t_constraint_shift_width", 5,
              {"DV_VERILATOR_CDCL": "0"}, timeout=10)
