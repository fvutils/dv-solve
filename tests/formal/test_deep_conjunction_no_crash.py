"""Regression guard for B10: deep conjunction spines must not overflow the stack.

Large plain-QF_BV instances (asp graph-colouring / edge-matching / sudoku) build
deeply-nested ``(and c1 (and c2 (and c3 ...)))`` expression trees. Three separate
recursive walks used to descend one C-stack frame per conjunct and SIGSEGV:

  * ``bb_binary``           (bitblast walk, zsp_bbsolver.c)
  * ``_compile_constraint`` (CDCL compile, zsp_compile.c)
  * ``collect_substs_from`` (bitblast equality-subst pre-pass, zsp_bbsolver.c)

All three were flattened to iterative heap worklists (fix 2026-07-20). These
fixtures crash the pre-fix binary within ~1.5 s on BOTH engines, so a short
timeout is enough to catch a regression: the process must NOT die from SIGSEGV.
A clean verdict or a timeout (hard instance) both count as "did not crash".

    direnv exec . pytest tests/formal/test_deep_conjunction_no_crash.py -v
"""
from __future__ import annotations

import signal
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"
_CORPUS = _ROOT / "tests" / "formal" / "corpus" / "smtcomp2025_parallel" / "smt2" / "QF_BV"

# Instances that crashed the pre-fix binary within a couple of seconds.
_FIXTURES = [
    "asp/GraphColouring/graph-colouring-nodes=130-density=0.1-instance=3.smt2",
    "asp/GraphColouring/graph-colouring-nodes=140-density=0.1-instance=2.smt2",
]
_ENGINES = ["bitblast", "cdcl"]

# The pre-fix binary crashes within ~1.5 s; an 8 s window is a >5x margin while
# keeping the guard fast (the fixed binary just keeps solving until the cap).
_TIMEOUT_S = 8


def _available() -> bool:
    return _BIN.is_file() and (_BIN.stat().st_mode & 0o111) != 0


@pytest.mark.skipif(not _available(), reason="dv-solve-smt2 not built")
@pytest.mark.parametrize("fixture", _FIXTURES)
@pytest.mark.parametrize("engine", _ENGINES)
def test_deep_conjunction_does_not_segfault(engine: str, fixture: str) -> None:
    path = _CORPUS / fixture
    if not path.is_file():
        pytest.skip(f"corpus fixture missing: {fixture}")
    try:
        proc = subprocess.run(
            [str(_BIN), f"--engine={engine}", str(path)],
            capture_output=True,
            timeout=_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        # Hard instance that keeps solving past the cap — the point is it did
        # NOT crash. Pass.
        return
    # A negative returncode is death-by-signal; SIGSEGV is the B10 regression.
    assert proc.returncode != -signal.SIGSEGV, (
        f"{engine} SIGSEGV on {fixture} (B10 deep-conjunction stack overflow "
        f"regressed); stderr tail: {proc.stderr[-400:]!r}"
    )
    # Any other signal death is also a regression worth failing on.
    assert proc.returncode >= 0, (
        f"{engine} died from signal {-proc.returncode} on {fixture}"
    )
