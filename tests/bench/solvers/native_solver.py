"""Native C-solver back-end for benchmarks.

Identical measurement loop to PythonSolver but with ZSP_SOLVER_BACKEND=native.
Skips automatically when the native shared library is not built yet.
"""
from __future__ import annotations

import pytest

from .python_solver import PythonSolver, _solver_env, _zdc_bench


class NativeSolver(PythonSolver):
    _backend_name = "native"

    def bench(
        self,
        cls: type,
        validate,
        tmp_path: Path,
        n_solutions: int = 1000,
        min_bench_ns: int = 1_000_000_000,
    ) -> None:
        # Verify the native library is available before starting.
        try:
            from dv_solve.lib import _load_lib
            if _load_lib() is None:
                pytest.skip("native solver library not built")
        except ImportError:
            pytest.skip("zuspec.solver not installed")

        with _solver_env("native"):
            _zdc_bench(cls, validate, str(self), n_solutions, min_bench_ns)
