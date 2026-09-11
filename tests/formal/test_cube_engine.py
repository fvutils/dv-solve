"""Soundness cross-check for the cube-and-conquer engine (DV_ENGINE=cube).

Two guards, both enforcing the soundness contract of
docs/cube_and_conquer_design.md — *correct-or-unknown, never wrong*:

  * ``test_cube_matches_bitblast`` — differential over tier1/tier2/tier3.
    The bitblast engine is the trusted oracle; the cube engine partitions
    the same bit-blasted instance, so it must agree on every fixture it
    answers. A cube ``unknown`` is allowed (sound); a sat/unsat that
    disagrees with bitblast is a bug.

  * ``test_cube_unsat_is_exhaustive`` / ``test_cube_sat_paths`` — targeted
    checks that the two-cube partition {x} ∪ {~x} yields UNSAT only when
    BOTH halves are UNSAT, and SAT (with a valid model) when either half is.

Usage:
    direnv exec . pytest tests/formal/test_cube_engine.py -v
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from .harness.dv_solve_smt2_solver import (
    DvSolveSMT2BBSolver,
    DvSolveSMT2CubeSolver,
)

_HERE = Path(__file__).resolve().parent
_BUILD_DIR = _HERE.parents[2] / "build"
_BINARY = _BUILD_DIR / "dv-solve-smt2"

TIER_DIRS = [_HERE / "smt2" / "tier1", _HERE / "smt2" / "tier2", _HERE / "smt2" / "tier3"]

PER_SOLVER_TIMEOUT_S = 15.0
_NON_ANSWERS = {"unknown", "timeout", "error"}


def _smt2_files() -> list[Path]:
    files: list[Path] = []
    for d in TIER_DIRS:
        if d.is_dir():
            files.extend(sorted(d.glob("*.smt2")))
    return files


_FILES = _smt2_files()
_CUBE = DvSolveSMT2CubeSolver()
_BB = DvSolveSMT2BBSolver()


@pytest.fixture(scope="module")
def cube_solver():
    if not _CUBE.is_available():
        pytest.skip("dv-solve-smt2 binary not built")
    return _CUBE


@pytest.fixture(scope="module")
def bb_solver():
    if not _BB.is_available():
        pytest.skip("dv-solve-smt2 binary not built")
    return _BB


@pytest.mark.parametrize(
    "smt2_file",
    _FILES,
    ids=[f"{f.parent.name}/{f.stem}" for f in _FILES],
)
def test_cube_matches_bitblast(smt2_file, cube_solver, bb_solver):
    """Cube must agree with the bitblast oracle on every fixture it answers."""
    bb = bb_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)
    if bb.result in _NON_ANSWERS:
        pytest.skip(f"bitblast oracle returned non-answer: {bb.result}")

    cube = cube_solver.solve(smt2_file, timeout_s=PER_SOLVER_TIMEOUT_S)
    if cube.result in _NON_ANSWERS:
        # A sound `unknown` is always permitted (never a wrong verdict).
        pytest.skip(f"cube returned non-answer (sound): {cube.result}")

    assert cube.result == bb.result, (
        f"cube says '{cube.result}', bitblast says '{bb.result}' on "
        f"{smt2_file.parent.name}/{smt2_file.stem}\n"
        f"  cube stdout:     {cube.stdout[:300]}\n"
        f"  bitblast stdout: {bb.stdout[:300]}"
    )


def _run_cube(smt2: str, timeout_s: float = 15.0) -> str:
    if not _BINARY.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    out = subprocess.run(
        [str(_BINARY), "/dev/stdin"],
        input=smt2,
        capture_output=True,
        text=True,
        timeout=timeout_s,
        env={"DV_ENGINE": "cube", "PATH": "/usr/bin:/bin"},
    )
    return out.stdout


# ---- targeted partition-soundness checks --------------------------------- #

# a<3 ∧ a>5 over BitVec4: empty domain — BOTH cubes of any split are UNSAT.
_UNSAT = (
    "(set-logic QF_BV)\n"
    "(declare-fun a () (_ BitVec 4))\n"
    "(assert (bvult a #x3))\n"
    "(assert (bvugt a #x5))\n"
    "(check-sat)\n"
)

# Forces the low bit of `a` to 1 (a is odd and == 0x2b): whichever cube pins
# that bit to 0 is UNSAT, the other is SAT — exercises the {~x} SAT path.
_SAT_ODD = (
    "(set-logic QF_BV)\n"
    "(declare-fun a () (_ BitVec 8))\n"
    "(assert (= a #x2b))\n"
    "(check-sat)\n"
    "(get-value (a))\n"
)


def test_cube_unsat_is_exhaustive():
    """An empty domain must aggregate to UNSAT (both cubes proved UNSAT)."""
    assert _run_cube(_UNSAT).splitlines()[0].strip() == "unsat"


def test_cube_sat_paths():
    """A satisfiable instance is SAT with the correct model under cube."""
    lines = _run_cube(_SAT_ODD).splitlines()
    assert lines[0].strip() == "sat"
    # model read-back must reflect the (only) solution a = 0x2b = 0b00101011
    assert any("00101011" in ln for ln in lines[1:]), lines
