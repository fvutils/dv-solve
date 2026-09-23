"""CDCL soundness audit — bitblast disabled.

Invariant we hold the CDCL engine to: it must NEVER return a wrong answer. It may
return `unknown` (sound but incomplete — bitblast then handles it as a pure
performance path), but a definitive CDCL `sat`/`unsat` that disagrees with the z3
oracle is a soundness bug.

This runs every Verilator-derived fixture through dv-solve with `DV_NO_BITBLAST=1`
(forces CDCL, disables both the auto-route to bitblast and the unknown->bitblast
escalation) and compares to z3:

  * cdcl sat/unsat == z3            -> pass  (CDCL is correct)
  * cdcl unknown / error / timeout  -> skip  (sound-but-incomplete; OK)
  * z3 unknown                      -> skip  (no oracle)
  * cdcl definitive != z3           -> FAIL  (CDCL soundness bug)

Rationale: routing to bitblast is a performance optimization, not a correctness
crutch — otherwise a latent CDCL bug surfaces exactly when bitblast isn't the
fast choice. See docs/solver_bug_backlog.md.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

from .harness._subprocess_solver import find_binary

_HERE = Path(__file__).resolve().parent
_DVSOLVE = _HERE.parents[1] / "build" / "dv-solve-smt2"
# The standalone-checkout venv first, then PATH: inside a larger workspace
# dv-solve has no packages/ of its own and z3 comes from the environment.
_Z3 = Path(find_binary(_HERE.parents[1] / "packages" / "python" / "bin" / "z3", "z3") or _HERE.parents[1] / "packages" / "python" / "bin" / "z3")
_FIXDIR = _HERE / "smt2" / "verilator"
_FILES = sorted(_FIXDIR.glob("*.smt2")) if _FIXDIR.is_dir() else []

# Known CDCL soundness bugs (wrong on pure CDCL, correct in production via the
# validation + bitblast-escalation net). Tracked in docs/solver_bug_backlog.md.
# xfail(strict) so a FIX flips them red and forces removal from this list.
_KNOWN_CDCL_BUGS = set()  # empty: no known wrong-answer CDCL bugs remain


def _verdict(argv, smt_path, env=None):
    try:
        p = subprocess.run(argv + [str(smt_path)], capture_output=True, text=True,
                           timeout=25, env=env)
    except subprocess.TimeoutExpired:
        return "timeout"
    out = p.stdout.strip().split("\n")
    return out[0].strip() if out and out[0].strip() else "error"


pytestmark = pytest.mark.skipif(
    not (_DVSOLVE.exists() and _Z3.exists() and _FILES),
    reason="dv-solve-smt2 / z3 / fixtures not available",
)


@pytest.mark.parametrize("fixture", _FILES, ids=[f.stem for f in _FILES])
def test_cdcl_never_wrong(fixture):
    if fixture.stem in _KNOWN_CDCL_BUGS:
        pytest.xfail("known pure-CDCL soundness bug (see solver_bug_backlog.md)")

    env = dict(os.environ, DV_NO_BITBLAST="1")
    cdcl = _verdict([str(_DVSOLVE)], fixture, env=env)
    if cdcl not in ("sat", "unsat"):
        pytest.skip(f"CDCL non-answer (sound-but-incomplete): {cdcl}")
    z3 = _verdict([str(_Z3), "-smt2"], fixture)
    if z3 not in ("sat", "unsat"):
        pytest.skip(f"z3 non-answer: {z3}")

    assert cdcl == z3, (
        f"CDCL SOUNDNESS BUG on {fixture.name}: cdcl={cdcl} z3={z3}. "
        "CDCL must be correct-or-unknown, never wrong (bitblast is perf-only)."
    )
