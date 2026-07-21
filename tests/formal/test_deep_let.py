"""Regression guard for deeply-nested ``let`` (B11 + the expanding-init soundness fix).

Two bugs fixed 2026-07-20, both only visible past the 64-entry substitution-stack
realloc boundary (``SMT2_MAX_SUBST``), so every case here nests well beyond 64:

  * **B11 — silent ``unknown``:** ``SMT2_MAX_SUBST`` was a hard 64 with a fixed
    ``subst_stack``; a ``let`` nest deeper than 64 active bindings overflowed and the
    whole assert silently tainted to ``unknown``. Now the stack is heap-grown.
  * **expanding-init soundness:** after the stack went heap-backed, the ``let`` push
    forgot to clear ``Smt2Subst.expanding`` (realloc'd memory is uninitialised, unlike
    the old zero-init fixed array). A garbage ``expanding`` byte made ``_subst_lookup``
    *skip* a binding — which with shadowed names (reused ``x``) could resolve to the
    WRONG outer binding → wrong verdict, not just ``unknown``.

Strategy: build deep ``let`` chains (unique-name, shadowed, multi-binding) with both
sat and unsat targets, and require dv-solve to (a) not answer ``unknown`` on these
solvable formulas, and (b) agree with z3.

    direnv exec . pytest tests/formal/test_deep_let.py -v
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"
_Z3 = _ROOT / "packages" / "python" / "bin" / "z3"

# Depths chosen to straddle the 64 realloc boundary and go well past the old
# ~113 chained-let failure point.
_DEPTHS = [65, 120, 300]


def _chain(depth: int, target: str, *, shadow: bool) -> str:
    """A depth-deep let chain; each binding depends on the previous.

    shadow=True reuses the name ``x`` at every level (exercises _subst_lookup
    walking past shadowed entries — the expanding-skip soundness case).
    """
    lines = ["(set-logic QF_BV)", "(declare-fun a () (_ BitVec 8))"]
    body = "(assert "
    if shadow:
        body += "(let ((x (bvadd a (_ bv1 8)))) "
        body += "".join("(let ((x (bvadd x (_ bv1 8)))) " for _ in range(2, depth + 1))
        body += f"(= x {target})"
    else:
        body += "(let ((x1 (bvadd a (_ bv1 8)))) "
        body += "".join(
            f"(let ((x{i} (bvadd x{i - 1} (_ bv1 8)))) " for i in range(2, depth + 1)
        )
        body += f"(= x{depth} {target})"
    body += ")" * depth + ")"
    lines.append(body)
    lines.append("(check-sat)")
    return "\n".join(lines) + "\n"


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


# target -> forces a satisfiable (a free) or unsatisfiable (x == a, impossible after
# depth increments) equality.  "(_ bv0 8)" is satisfiable (pick a = -depth).
_CASES = [
    ("(_ bv0 8)", "sat"),          # solvable: a can be chosen so x wraps to 0
    ("(bvadd a (_ bv0 8))", "unsat"),  # x = a + depth == a is impossible for depth%256 != 0
]


@pytest.mark.skipif(not _available(), reason="dv-solve-smt2 not built")
@pytest.mark.skipif(not _Z3.is_file(), reason="z3 not available")
@pytest.mark.parametrize("shadow", [False, True], ids=["unique", "shadowed"])
@pytest.mark.parametrize("depth", _DEPTHS)
@pytest.mark.parametrize("target,_hint", _CASES)
def test_deep_let_matches_z3(depth: int, shadow: bool, target: str, _hint: str) -> None:
    smt2 = _chain(depth, target, shadow=shadow)
    dv = _run(_BIN, smt2, "--engine=bitblast")
    z3 = _run(_Z3, smt2, "-in")
    # B11: a solvable deep-let formula must not silently degrade to unknown.
    assert dv != "unknown", (
        f"deep let depth={depth} shadow={shadow} target={target} -> unknown "
        f"(B11 subst-stack cap / silent-taint regression)"
    )
    # Soundness: must agree with z3 (the shadowed cases exercise expanding-skip).
    assert dv == z3, (
        f"deep let depth={depth} shadow={shadow} target={target}: dv={dv} z3={z3}"
    )
