"""Generative differential cross-check: dv-solve (both engines) vs z3.

Runs a deterministic fleet of fuzzer-generated QF_BV problems (see
``fuzz_bv_smt2.py``) through both dv-solve engines and z3, enforcing the one
invariant that must always hold: **dv-solve never disagrees with z3**. A
dv-solve ``sat``/``unsat`` must match z3's; ``unknown``/``timeout`` is an
allowed honest punt. Any hard sat-vs-unsat split is a soundness bug — and the
generated SMT2 is dumped in the failure message for a one-command repro.

Coverage (how often dv-solve gives a definite answer) is logged, not asserted:
the point of the fuzzer is soundness across the op space, not a coverage floor.

Seeds are fixed, so this is reproducible and non-flaky. Scale with
``FUZZ_N`` (default 160):  ``FUZZ_N=1000 pytest tests/formal/test_fuzz_cross_check.py``
"""
from __future__ import annotations

import os

import pytest

from .fuzz_bv_smt2 import generate_problem
from .harness.dv_solve_smt2_solver import DvSolveSMT2Solver, DvSolveSMT2BBSolver
from .harness.z3_solver import Z3Solver

_N = int(os.environ.get("FUZZ_N", "160"))
_SEEDS = list(range(_N))
TIMEOUT_S = 10.0
_ANSWERS = {"sat", "unsat"}

_DV_CDCL = DvSolveSMT2Solver()
_DV_BB = DvSolveSMT2BBSolver()
_Z3 = Z3Solver()

# Module-level coverage tally (reported by the summary test at the end).
_cov = {"answered_cdcl": 0, "answered_bb": 0, "checked": 0}


@pytest.fixture(scope="module")
def z3_oracle():
    if not _Z3.is_available():
        pytest.skip("z3 not on PATH; cannot cross-check")
    return _Z3


@pytest.mark.parametrize("seed", _SEEDS)
def test_fuzz_cross_check(seed, z3_oracle, tmp_path):
    if not _DV_CDCL.is_available():
        pytest.skip("dv-solve-smt2 binary not built")

    smt2 = generate_problem(seed)
    f = tmp_path / f"fuzz_{seed}.smt2"
    f.write_text(smt2)

    z3 = z3_oracle.solve(f, timeout_s=TIMEOUT_S).result
    if z3 not in _ANSWERS:
        pytest.skip(f"z3 non-answer ({z3}) on seed {seed}")

    cdcl = _DV_CDCL.solve(f, timeout_s=TIMEOUT_S).result
    bb = _DV_BB.solve(f, timeout_s=TIMEOUT_S).result

    _cov["checked"] += 1
    if cdcl in _ANSWERS:
        _cov["answered_cdcl"] += 1
    if bb in _ANSWERS:
        _cov["answered_bb"] += 1

    for eng, res in (("cdcl", cdcl), ("bitblast", bb)):
        if res in _ANSWERS:
            assert res == z3, (
                f"SOUNDNESS: dv-solve ({eng}) says '{res}', z3 says '{z3}' "
                f"on fuzz seed {seed}\n--- repro ({f}) ---\n{smt2}"
            )


def test_zz_fuzz_coverage_report():
    """Not a pass/fail gate — just surfaces the answered-vs-punted ratio."""
    n = _cov["checked"]
    if n == 0:
        pytest.skip("no fuzz cases checked (ran in isolation?)")
    print(
        f"\n[fuzz coverage] checked={n}  "
        f"cdcl answered={_cov['answered_cdcl']} ({100*_cov['answered_cdcl']/n:.0f}%)  "
        f"bitblast answered={_cov['answered_bb']} ({100*_cov['answered_bb']/n:.0f}%)"
    )
