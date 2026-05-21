"""Bitwuzla SMT-LIB2 back-end for benchmarks.

Emits the constrained-randomization problem as SMT-LIB2 once, then runs
bitwuzla N times with varying ``-s <seed>`` values.  Subprocess overhead is
intentionally included — it is part of the real cost of this approach.
"""
from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

import pytest
import zuspec.dataclasses as zdc

from .base import BenchResult, RESULTS_DIR, get_bench_config

# Prefer the bundled bitwuzla shipped with the verilator package.
_BUNDLED = (
    Path(__file__).parent.parent.parent.parent.parent.parent
    / "packages" / "verilator" / "bin" / "bitwuzla"
)

BATCH_SIZE = 50


def _find_bitwuzla() -> str | None:
    if _BUNDLED.is_file() and _BUNDLED.stat().st_mode & 0o111:
        return str(_BUNDLED)
    return shutil.which("bitwuzla")


class BitwuzlaSolver:
    def __str__(self) -> str:
        return "bitwuzla"

    def bench(
        self,
        cls: type,
        validate,
        tmp_path: Path,
        n_solutions: int = 1000,
        min_bench_ns: int = 1_000_000_000,
    ) -> None:
        bitwuzla = _find_bitwuzla()
        if bitwuzla is None:
            pytest.skip("bitwuzla not found on PATH")

        try:
            from zuspec.be.fv.smt2.rand_emitter import RandSMT2Emitter, parse_get_value
        except ImportError:
            pytest.skip("zuspec-be-fv not available (RandSMT2Emitter missing)")

        cfg = get_bench_config()
        target_ns  = int(cfg.target_secs * 1e9)
        timeout_ns = int(cfg.timeout_secs * 1e9)
        min_sol    = cfg.min_solutions

        smt2_text   = RandSMT2Emitter().emit(cls, seed=0)
        smt_file    = tmp_path / "problem.smt2"
        smt_file.write_text(smt2_text)
        field_names = [f["name"] for f in zdc.extract_rand_fields(cls)]

        iters    = 0
        failures = 0
        seed     = 0
        t_start  = time.perf_counter_ns()

        while True:
            for _ in range(BATCH_SIZE):
                out = subprocess.run(
                    [bitwuzla, "-s", str(seed), str(smt_file)],
                    capture_output=True, text=True,
                ).stdout
                if out.startswith("sat"):
                    sol = parse_get_value(out, field_names)
                    if iters % max(1, min_sol) == 0:
                        validate(sol)
                else:
                    failures += 1
                iters += 1
                seed  += 1

            elapsed = time.perf_counter_ns() - t_start

            if elapsed >= target_ns and iters >= min_sol:
                break

            if elapsed >= timeout_ns:
                if iters >= min_sol:
                    break
                pytest.skip(
                    f"bitwuzla: timed out after {elapsed/1e9:.1f}s "
                    f"with only {iters} solutions on {cls.__name__}"
                )

        assert failures == 0, f"{failures} unsatisfiable results out of {iters}"

        result = BenchResult("bitwuzla", cls.__name__.lower(), iters, elapsed)
        result.save(RESULTS_DIR)
        print("\n" + result.summary())
