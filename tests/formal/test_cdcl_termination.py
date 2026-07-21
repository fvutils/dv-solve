"""CDCL termination audit — pure CDCL must never hang.

Companion to test_cdcl_soundness.py. That test guards *correctness* (CDCL never
wrong); this one guards *termination* (CDCL always returns a verdict in bounded
time). The two together are the full "correct-or-unknown, never hang" invariant.

Motivation (B10, docs/solver_bug_backlog.md): the CDCL search bounds *work*
(conflict/restart counters) but historically not *time* — a hard shape could
churn a Luby-growing conflict budget for many minutes, and a non-progressing
propagation loop could spin without ever consulting the budget. Both read as a
hang. A hang violates correct-or-unknown just as badly as a wrong answer: the
caller can neither trust nor escalate a solve that never returns.

Each Verilator-derived fixture is run through pure CDCL (DV_NO_BITBLAST=1) with a
short wall-clock budget. We assert the process returns one of sat/unsat/unknown
*well within* a subprocess timeout set comfortably above that budget. If the
budget fails to fire (a true hang), the subprocess timeout trips and the test
fails, naming the fixture. This is a per-construct/shape no-hang guarantee.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_DVSOLVE = _HERE.parents[1] / "build" / "dv-solve-smt2"
_FIXDIR = _HERE / "smt2" / "verilator"
_FILES = sorted(_FIXDIR.glob("*.smt2")) if _FIXDIR.is_dir() else []

# CDCL wall-clock budget for the run, and the subprocess ceiling. The ceiling is
# well above the budget so a healthy solve (budget -> unknown) always finishes in
# time and only a genuine hang (budget never fires) trips the timeout.
_CDCL_BUDGET_SEC = "4"
_PROC_TIMEOUT_SEC = 15

pytestmark = pytest.mark.skipif(
    not (_DVSOLVE.exists() and _FILES),
    reason="dv-solve-smt2 / fixtures not available",
)


@pytest.mark.parametrize("fixture", _FILES, ids=[f.stem for f in _FILES])
def test_cdcl_always_terminates(fixture):
    env = dict(os.environ, DV_NO_BITBLAST="1", DV_CDCL_TIME_LIMIT=_CDCL_BUDGET_SEC)
    try:
        p = subprocess.run(
            [str(_DVSOLVE), str(fixture)],
            capture_output=True, text=True,
            timeout=_PROC_TIMEOUT_SEC, env=env,
        )
    except subprocess.TimeoutExpired:
        pytest.fail(
            f"CDCL HANG on {fixture.name}: no verdict within {_PROC_TIMEOUT_SEC}s "
            f"(budget {_CDCL_BUDGET_SEC}s). CDCL must degrade to `unknown` in "
            "bounded time, never hang (see B10 in solver_bug_backlog.md)."
        )
    verdict = (p.stdout.strip().split("\n") or [""])[0].strip()
    assert verdict in ("sat", "unsat", "unknown"), (
        f"CDCL produced no valid verdict on {fixture.name}: got {verdict!r}"
    )
