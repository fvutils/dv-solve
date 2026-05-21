"""sby + yosys toolchain smoke tests.

Runs sby on a hand-written counter_assert.sv example with BMC and cover
tasks to verify the formal verification flow works end-to-end.

Usage:
    direnv exec . pytest tests/formal/test_sby_smoke.py -v
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

_FORMAL_DIR = Path(__file__).resolve().parent
SBY_FILE = _FORMAL_DIR / "sby" / "counter_assert.sby"
SV_FILE = _FORMAL_DIR / "sv" / "counter_assert.sv"


def _find_sby() -> str | None:
    """Find the sby executable on PATH."""
    return shutil.which("sby")


def _run_sby(task: str, workdir: Path) -> subprocess.CompletedProcess:
    """Run sby for a specific task in the given working directory."""
    sby = _find_sby()
    if sby is None:
        pytest.skip("sby not found on PATH")

    # Copy SV source into workdir so sby can find it
    shutil.copy2(SV_FILE, workdir / "counter_assert.sv")
    sby_copy = workdir / "counter_assert.sby"
    shutil.copy2(SBY_FILE, sby_copy)

    return subprocess.run(
        [sby, "-f", str(sby_copy), task],
        capture_output=True,
        text=True,
        timeout=120,
        cwd=str(workdir),
    )


@pytest.mark.parametrize("task", ["bmc", "cover"])
def test_sby_task(task, tmp_path):
    """Run an sby task and verify it passes."""
    proc = _run_sby(task, tmp_path)
    stdout_tail = proc.stdout[-500:] if proc.stdout else ""
    stderr_tail = proc.stderr[-500:] if proc.stderr else ""

    assert proc.returncode == 0, (
        f"sby {task} failed (rc={proc.returncode}):\n"
        f"--- stdout (last 500 chars) ---\n{stdout_tail}\n"
        f"--- stderr (last 500 chars) ---\n{stderr_tail}"
    )

    combined = proc.stdout + proc.stderr
    assert "PASS" in combined.upper() or "DONE" in combined.upper(), (
        f"sby {task}: no PASS/DONE in output:\n{stdout_tail}"
    )
