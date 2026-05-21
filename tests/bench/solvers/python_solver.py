"""Python-solver back-end for benchmarks.

Wraps ``zdc.randomize()`` directly with no extra environment variable set.
"""
from __future__ import annotations

import os
import signal
import time
from contextlib import contextmanager
from pathlib import Path

import zuspec.dataclasses as zdc

from .base import BenchResult, RESULTS_DIR, get_bench_config

WARMUP_ITERS = 50


class _BenchTimeout(Exception):
    """Raised by SIGALRM when the benchmark wall-clock budget is exhausted."""


def _alarm_handler(signum, frame):
    raise _BenchTimeout


@contextmanager
def _solver_env(backend_name: str):
    """Temporarily set ZSP_SOLVER_BACKEND to *backend_name*."""
    old = os.environ.get("ZSP_SOLVER_BACKEND")
    os.environ["ZSP_SOLVER_BACKEND"] = backend_name
    try:
        yield
    finally:
        if old is None:
            os.environ.pop("ZSP_SOLVER_BACKEND", None)
        else:
            os.environ["ZSP_SOLVER_BACKEND"] = old


def _zdc_bench(
    cls,
    validate,
    solver_name: str,
    n_solutions: int = 0,
    min_bench_ns: int = 0,
) -> None:
    """Adaptive timed loop for the Python and native back-ends.

    Runs randomizations until the wall-clock *target* is reached (and at
    least *min_solutions* produced).  A SIGALRM enforces the hard timeout
    even if a single randomize() call blocks.
    """
    import pytest
    from zuspec.dataclasses.constraint_parser import extract_rand_fields
    try:
        from zuspec.dataclasses.solver._core_solve import RandomizationError
    except ImportError:
        RandomizationError = Exception  # type: ignore

    cfg = get_bench_config()
    target_ns  = int(cfg.target_secs * 1e9)
    timeout_ns = int(cfg.timeout_secs * 1e9)
    min_sol    = cfg.min_solutions

    fields_info = extract_rand_fields(cls)
    fields      = [f["name"] for f in fields_info]
    init_kwargs = {f["name"]: f.get("default", 0) for f in fields_info}
    obj = cls(**init_kwargs)

    iters = 0
    failures = 0
    t_start = time.perf_counter_ns()  # captured before warm-up for alarm handler

    # Install SIGALRM so a hanging randomize() call doesn't block forever.
    prev_handler = signal.signal(signal.SIGALRM, _alarm_handler)
    # +2s grace beyond the configured timeout for warm-up overhead
    signal.alarm(int(cfg.timeout_secs) + 2)

    try:
        # Warm-up (excluded from timing)
        for _ in range(min(WARMUP_ITERS, max(min_sol, 1))):
            try:
                zdc.randomize(obj)
            except RandomizationError:
                pass

        t_start = time.perf_counter_ns()  # reset after warm-up

        while True:
            try:
                zdc.randomize(obj)
                iters += 1
            except RandomizationError:
                failures += 1

            elapsed = time.perf_counter_ns() - t_start
            total_attempts = iters + failures

            # Done: target time reached and enough solutions
            if elapsed >= target_ns and iters >= min_sol:
                break

            # Soft timeout check (for when individual calls are fast but many)
            if elapsed >= timeout_ns:
                if iters >= min_sol:
                    break
                pytest.skip(
                    f"{solver_name}: timed out after {elapsed/1e9:.1f}s "
                    f"with only {iters} solutions on {cls.__name__}"
                )

            # Give up if solver can't handle the problem (>90% UNSAT)
            if total_attempts >= 50 and failures / total_attempts > 0.90:
                pytest.skip(
                    f"{solver_name}: >90% UNSAT rate on {cls.__name__} "
                    f"({failures}/{total_attempts} failed)"
                )

    except _BenchTimeout:
        elapsed = time.perf_counter_ns() - t_start
        if iters >= min_sol:
            pass  # Got enough data, fall through to reporting
        else:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, prev_handler)
            pytest.skip(
                f"{solver_name}: timed out (alarm) after {cfg.timeout_secs}s "
                f"with {iters} solutions on {cls.__name__}"
            )
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, prev_handler)

    sol = {f: getattr(obj, f) for f in fields}
    validate(sol)

    if failures:
        print(f"\n  note: {failures}/{iters + failures} attempts returned UNSAT")

    result = BenchResult(solver_name, cls.__name__.lower(), iters, elapsed)
    result.save(RESULTS_DIR)
    print("\n" + result.summary())


class PythonSolver:
    _backend_name = "python"

    def __str__(self) -> str:
        return self._backend_name

    def bench(
        self,
        cls: type,
        validate,
        tmp_path: Path,
        n_solutions: int = 1000,
        min_bench_ns: int = 1_000_000_000,
    ) -> None:
        with _solver_env(self._backend_name):
            _zdc_bench(cls, validate, str(self), n_solutions, min_bench_ns)
