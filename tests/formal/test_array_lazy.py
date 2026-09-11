"""Differential cross-check for the DV_ARRAY word-level lazy array engine vs z3.

Two suites, one invariant -- the lazy engine must NEVER disagree with z3
(sat-vs-unsat); ``unknown`` is an allowed honest punt:

1. The tier3 array-BMC fixtures (regfile / fifo / cache / dma / wide, d1-d8):
   the representative hardware/DV workload, exercising store chains + the
   yosys-style array-equality transition relation, incl. one SAT witness case.

2. A deterministic generative fuzzer (``fuzz_array_smt2.py``) stressing
   read-over-write, congruence, array equality between store terms, and
   structural sharing -- the constructs that surfaced (and now guard against)
   the soundness bugs found during bring-up: uninitialised read state, model
   reads corrupted by don't-care diversification, one-directional equality
   consistency, un-shared structurally-identical terms, and missing store-index
   instantiation for equality completeness.

Scale the fuzzer with ``ARRAY_FUZZ_N`` (default 200):
    ARRAY_FUZZ_N=2000 pytest tests/formal/test_array_lazy.py
"""
from __future__ import annotations

import os
from pathlib import Path

import pytest

from .fuzz_array_smt2 import generate_problem
from .harness.dv_solve_smt2_solver import DvSolveSMT2ArraySolver
from .harness.z3_solver import Z3Solver

_DV = DvSolveSMT2ArraySolver()
_Z3 = Z3Solver()
TIMEOUT_S = 15.0
_ANSWERS = {"sat", "unsat"}

_TIER3 = sorted((Path(__file__).parent / "smt2" / "tier3").glob("*.smt2"))
# tier4: large / wide-address memory BMC (the band the mux-forest can't touch;
# the DV_ARRAY lazy engine is the only option). Non-extensional store chains.
_TIER4 = sorted((Path(__file__).parent / "smt2" / "tier4").glob("*.smt2"))
_N = int(os.environ.get("ARRAY_FUZZ_N", "200"))
_SEEDS = list(range(_N))


@pytest.fixture(scope="module")
def z3_oracle():
    if not _Z3.is_available():
        pytest.skip("z3 not on PATH; cannot cross-check")
    return _Z3


def _check(smt2_path, z3):
    if not _DV.is_available():
        pytest.skip("dv-solve-smt2 binary not built")
    dv = _DV.solve(smt2_path, timeout_s=TIMEOUT_S).result
    z = z3.solve(smt2_path, timeout_s=TIMEOUT_S).result
    if dv in _ANSWERS and z in _ANSWERS:
        assert dv == z, (
            f"DV_ARRAY lazy engine disagrees with z3 on {smt2_path.name}: "
            f"dv={dv} z3={z}\n{smt2_path.read_text()}"
        )


@pytest.mark.skipif(not _TIER3, reason="tier3 fixtures not generated")
@pytest.mark.parametrize("fixture", _TIER3, ids=lambda p: p.stem)
def test_tier3_array_lazy(fixture, z3_oracle):
    _check(fixture, z3_oracle)


@pytest.mark.skipif(not _TIER4, reason="tier4 fixtures not generated "
                                       "(run tests/formal/gen_large_mem.py)")
@pytest.mark.parametrize("fixture", _TIER4, ids=lambda p: p.stem)
def test_tier4_large_mem(fixture, z3_oracle):
    _check(fixture, z3_oracle)


@pytest.mark.parametrize("seed", _SEEDS)
def test_fuzz_array_lazy(seed, z3_oracle, tmp_path):
    f = tmp_path / f"arr_{seed}.smt2"
    f.write_text(generate_problem(seed))
    _check(f, z3_oracle)
