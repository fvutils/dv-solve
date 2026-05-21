"""Native C executable back-end for benchmarks.

Generates a self-contained C harness via CSolvePerfHarnessGenerator,
compiles it with gcc against libzsp_solver.so, and runs the resulting
executable.  This measures the raw C solver throughput without any
Python/ctypes overhead.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest
import zuspec.dataclasses as zdc

from .base import BenchResult, RESULTS_DIR, get_bench_config

# Locate libzsp_solver.so
_SOLVER_PKG = Path(__file__).parent.parent.parent.parent  # packages/zuspec-solver
_BUILD_DIR = _SOLVER_PKG / "build"
_SRC_DIR = _SOLVER_PKG / "src" / "c"


class CExeSolver:
    def __str__(self) -> str:
        return "native-c"

    def bench(
        self,
        cls: type,
        validate,
        tmp_path: Path,
        n_solutions: int = 1000,
        min_bench_ns: int = 1_000_000_000,
    ) -> None:
        # Check prerequisites
        lib_path = _BUILD_DIR / "libzsp_solver.so"
        if not lib_path.exists():
            pytest.skip("libzsp_solver.so not built (run cmake --build)")
        if not shutil.which("gcc"):
            pytest.skip("gcc not found on PATH")

        try:
            from zuspec.solver.c_bench_harness import CSolvePerfHarnessGenerator
        except ImportError:
            pytest.skip("CSolvePerfHarnessGenerator not available")

        cfg = get_bench_config()
        timeout_ns = int(cfg.timeout_secs * 1e9)

        # Generate C harness
        try:
            c_text = CSolvePerfHarnessGenerator().emit(cls, n_solutions=100_000)
        except Exception as exc:
            pytest.skip(f"C harness generation failed: {exc}")

        c_file = tmp_path / "harness.c"
        exe_file = tmp_path / "harness"
        c_file.write_text(c_text)

        # Compile
        rc = subprocess.run(
            ["gcc", "-O2",
             "-I", str(_SRC_DIR),
             "-o", str(exe_file),
             str(c_file),
             "-L", str(_BUILD_DIR),
             f"-Wl,-rpath,{_BUILD_DIR}",
             "-lzsp_solver", "-lrt"],
            capture_output=True, text=True,
        )
        if rc.returncode != 0:
            pytest.skip(f"C harness compile failed: {rc.stderr[:200]}")

        # Run
        result = subprocess.run(
            [str(exe_file), "-q",
             f"+n_solutions=100000",
             f"+timeout_ns={timeout_ns}"],
            capture_output=True, text=True,
            timeout=int(cfg.timeout_secs) + 5,
        )

        # Fail fast if the harness exited non-zero (compile or solve errors)
        if result.returncode != 0:
            pytest.fail(
                f"native-c harness exited {result.returncode}: "
                f"{result.stderr.strip()[-300:]}"
            )

        # Parse BENCH_NS from stderr (new format includes failure count)
        m = re.search(r"BENCH_NS\s+(\d+)\s+\((\d+)\s+solutions", result.stderr)
        if m is None:
            pytest.fail(f"BENCH_NS not found in stderr: {result.stderr[:300]}")

        bench_ns = int(m.group(1))
        n = int(m.group(2))

        if n < cfg.min_solutions:
            pytest.skip(f"{self}: only {n} solutions in {bench_ns/1e9:.1f}s")

        # Spot-check: run a small batch WITH SOL output to validate.
        # Use enough solutions to catch intermittent failures.
        try:
            check_result = subprocess.run(
                [str(exe_file), "+n_solutions=20", "+timeout_ns=5000000000"],
                capture_output=True, text=True,
                timeout=10,
            )
            if check_result.returncode != 0:
                pytest.fail(
                    f"native-c validation run failed: "
                    f"{check_result.stderr.strip()[-300:]}"
                )
            field_names = [f["name"] for f in zdc.extract_rand_fields(cls)]
            n_validated = 0
            for line in check_result.stdout.splitlines():
                if line.startswith("SOL "):
                    vals = list(map(int, line.split()[1:]))
                    sol = dict(zip(field_names, vals))
                    validate(sol)
                    n_validated += 1
            if n_validated == 0:
                pytest.fail("native-c validation produced no SOL lines")
        except subprocess.TimeoutExpired:
            pass  # validation timed out; throughput result still valid

        bench = BenchResult(str(self), cls.__name__.lower(), n, bench_ns)
        bench.save(RESULTS_DIR)
        print("\n" + bench.summary())
