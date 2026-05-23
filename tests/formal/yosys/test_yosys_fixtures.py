"""End-to-end test for the yosys-shaped fixtures landed in Phase 6.5.

These exercise the (declare-datatypes ((NAME 0)) ((...))) translation to
opaque-sort + per-field sort-fun, the (set-logic ALL) and (reset-assertions)
plumbing, and the --smt2 / --no-incremental CLI shim.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
DV = Path(__file__).resolve().parents[3] / "build" / "dv-solve-smt2"

_FIXTURES = sorted(HERE.glob("*.smt2"))

_EXPECTED = {
    "regfile_dt": "unsat",
}


@pytest.mark.parametrize("fixture", _FIXTURES, ids=[f.stem for f in _FIXTURES])
def test_yosys_fixture(fixture: Path):
    if not DV.exists():
        pytest.skip(f"dv-solve-smt2 not built at {DV}")
    expected = _EXPECTED.get(fixture.stem)
    if expected is None:
        pytest.skip(f"no expected result for {fixture.stem}")
    out = subprocess.run(
        [str(DV), "--smt2", str(fixture)],
        capture_output=True, text=True, timeout=30,
    )
    first = out.stdout.strip().splitlines()[0] if out.stdout.strip() else ""
    assert first == expected, (
        f"{fixture.stem}: expected {expected!r}, got {first!r}\n"
        f"stdout: {out.stdout[:300]}\nstderr: {out.stderr[:300]}"
    )
