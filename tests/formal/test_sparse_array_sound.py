"""Soundness guard for wide (sparse) arrays.

An array whose index width M exceeds SMT2_MAX_ARRAY_ADDR_BITS (10) is too large to
expand densely (2^M element vars), so the frontend registers it as a *sparse*
array: element vars are created lazily per concrete index. This must be SOUND —
dv-solve may answer ``unknown`` (e.g. on stores or symbolic indices it does not
model precisely), but it must NEVER disagree with z3 (sat-vs-unsat).

This is the contract that replaced the old "reject too-wide arrays" behavior
(see tests/c/test_smt2_frontend.c::test_declare_array_addr_too_wide). Here we lock
in the soundness of the replacement differentially against z3.

    direnv exec . pytest tests/formal/test_sparse_array_sound.py -v
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from .harness._subprocess_solver import find_binary

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"
# The standalone-checkout venv first, then PATH: inside a larger workspace
# dv-solve has no packages/ of its own and z3 comes from the environment.
_Z3 = Path(find_binary(_ROOT / "packages" / "python" / "bin" / "z3", "z3") or _ROOT / "packages" / "python" / "bin" / "z3")

# Each case: (id, smt2). Widths are all M=12 > 10 so they take the sparse path.
_HDR = "(set-logic QF_AUFBV)\n(declare-const a (Array (_ BitVec 12) (_ BitVec 8)))\n"
_CASES = [
    ("concrete_select_sat",
     _HDR + "(assert (= (select a (_ bv1 12)) (_ bv3 8)))\n"
            "(assert (= (select a (_ bv2 12)) (_ bv4 8)))\n(check-sat)\n"),
    ("same_idx_conflict_unsat",
     _HDR + "(assert (= (select a (_ bv9 12)) (_ bv3 8)))\n"
            "(assert (= (select a (_ bv9 12)) (_ bv4 8)))\n(check-sat)\n"),
    ("store_select_roundtrip_sat",
     _HDR + "(assert (= (select (store a (_ bv5 12) (_ bv7 8)) (_ bv5 12)) (_ bv7 8)))\n"
            "(check-sat)\n"),
    ("store_select_contradiction_unsat",
     _HDR + "(assert (not (= (select (store a (_ bv5 12) (_ bv7 8)) (_ bv5 12)) (_ bv7 8))))\n"
            "(check-sat)\n"),
    ("symbolic_index_sat",
     _HDR + "(declare-const i (_ BitVec 12))\n"
            "(assert (= (select a i) (_ bv3 8)))\n"
            "(assert (= (select a (_ bv0 12)) (_ bv5 8)))\n(check-sat)\n"),
    ("many_concrete_selects_unsat",
     _HDR + "(assert (= (select a (_ bv100 12)) (_ bv1 8)))\n"
            "(assert (= (select a (_ bv200 12)) (_ bv1 8)))\n"
            "(assert (not (= (select a (_ bv100 12)) (select a (_ bv200 12)))))\n"
            "(check-sat)\n"),
]


def _run(binary: Path, smt2: str, *extra: str) -> str:
    proc = subprocess.run(
        [str(binary), *extra], input=smt2, capture_output=True, text=True, timeout=60
    )
    for line in proc.stdout.splitlines():
        s = line.strip().lower()
        if s in ("sat", "unsat", "unknown"):
            return s
    return "none"


def _available() -> bool:
    return _BIN.is_file() and (_BIN.stat().st_mode & 0o111) != 0


@pytest.mark.skipif(not _available(), reason="dv-solve-smt2 not built")
@pytest.mark.skipif(not _Z3.is_file(), reason="z3 not available")
@pytest.mark.parametrize("case_id,smt2", _CASES, ids=[c[0] for c in _CASES])
def test_sparse_array_agrees_with_z3(case_id: str, smt2: str) -> None:
    dv = _run(_BIN, smt2)
    z3 = _run(_Z3, smt2, "-in")
    # dv may be honest-unknown, but must never contradict z3 (sat<->unsat).
    if dv in ("sat", "unsat"):
        assert dv == z3, f"{case_id}: UNSOUND dv={dv} z3={z3}"
    else:
        assert dv == "unknown", f"{case_id}: unexpected dv={dv!r}"
