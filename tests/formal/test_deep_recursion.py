"""Regression guard for B12 — deep S-expression nesting must never SIGSEGV.

The SMT2 translator (`_translate_tagged` <-> `_translate_list_tagged`) recurses one
C frame per nesting level, so a deeply-nested expression used to overflow the C
stack and crash (signal 11). Fixed 2026-07-21 by (a) a stack-sized depth guard that
bails to ``unknown`` past a safe depth and (b) running the solve on a 1 GiB-stack
pthread so legitimately-deep formulas solve instead of crashing.

Contract asserted here (soundness, not performance):
  * NO input, at any nesting depth, may crash (rc 139 / -11). Ever.
  * A moderately-deep solvable chain returns a real verdict (``unsat`` here), not a
    crash — proving the big worker stack is in effect.
  * A pathologically-deep chain returns ``unknown`` (the guard fired), never a crash
    and never a wrong verdict.

    direnv exec . pytest tests/formal/test_deep_recursion.py -v
"""
from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"


def _chain(depth: int, *, use_let: bool) -> str:
    lines = ["(set-logic QF_BV)", "(declare-fun a () (_ BitVec 8))"]
    if use_let:
        body = "(assert (let ((x0 (bvadd a (_ bv1 8)))) "
        body += "".join(
            f"(let ((x{i} (bvadd x{i - 1} (_ bv1 8)))) " for i in range(1, depth)
        )
        body += f"(= x{depth - 1} a)" + ")" * depth + ")"
    else:
        expr = "(bvadd " * depth + "a" + " (_ bv1 8))" * depth
        body = f"(assert (= {expr} a))"
    lines.append(body)
    lines.append("(check-sat)")
    return "\n".join(lines) + "\n"


def _run(smt2: str) -> tuple[str, int]:
    proc = subprocess.run(
        [str(_BIN)], input=smt2, capture_output=True, text=True, timeout=120
    )
    verdict = "none"
    for line in proc.stdout.splitlines():
        s = line.strip().lower()
        if s in ("sat", "unsat", "unknown"):
            verdict = s
    return verdict, proc.returncode


def _available() -> bool:
    return _BIN.is_file() and (_BIN.stat().st_mode & 0o111) != 0


pytestmark = pytest.mark.skipif(not _available(), reason="dv-solve-smt2 not built")


# Depths that straddle the old ~1490 crash point and go far past it. The key
# invariant for every one of these is "does not crash".
@pytest.mark.parametrize("use_let", [False, True], ids=["bvadd", "let"])
@pytest.mark.parametrize("depth", [2000, 8000, 60000, 200000])
def test_deep_nesting_never_crashes(depth: int, use_let: bool) -> None:
    verdict, rc = _run(_chain(depth, use_let=use_let))
    # SIGSEGV shows up as a negative returncode (-11) or 139 under a shell.
    assert rc not in (139, -11), (
        f"depth={depth} use_let={use_let} CRASHED (rc={rc}) — B12 regression"
    )
    assert verdict in ("sat", "unsat", "unknown"), (
        f"depth={depth} use_let={use_let}: no verdict (rc={rc})"
    )


@pytest.mark.parametrize("use_let", [False, True], ids=["bvadd", "let"])
def test_moderate_depth_solves(use_let: bool) -> None:
    """A 2000-deep chain (past the old crash point) must SOLVE, proving the
    large worker stack is active — not merely bail to unknown."""
    verdict, rc = _run(_chain(2000, use_let=use_let))
    assert rc not in (139, -11), f"CRASHED (rc={rc})"
    assert verdict == "unsat", f"expected unsat at depth 2000, got {verdict!r}"


def test_pathological_depth_defers_not_crashes() -> None:
    """An absurdly deep chain trips the guard -> unknown (sound), never a crash."""
    verdict, rc = _run(_chain(200000, use_let=False))
    assert rc not in (139, -11), f"CRASHED (rc={rc}) — guard failed to fire"
    assert verdict == "unknown", f"expected unknown from the depth guard, got {verdict!r}"
