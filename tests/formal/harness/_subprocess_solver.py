"""Shared base for subprocess-based SMT solver wrappers.

Each concrete solver just supplies a binary path and argument template;
the actual invocation, timing, memory capture, and result parsing live
here.
"""
from __future__ import annotations

import resource
import shutil
import subprocess
import time
from pathlib import Path
from typing import Sequence

from .protocol import FormalResult


def _parse_result(stdout: str) -> str:
    """Extract sat/unsat/unknown from solver stdout."""
    for line in stdout.splitlines():
        tok = line.strip()
        if not tok or tok.startswith(";"):
            continue
        if tok == "sat":
            return "sat"
        if tok == "unsat":
            return "unsat"
        if tok == "unknown":
            return "unknown"
    return "unknown"


def run_smt2_solver(
    solver_name: str,
    argv: Sequence[str],
    smt2_path: Path,
    *,
    timeout_s: float = 30.0,
    benchmark: str | None = None,
) -> FormalResult:
    """Run an SMT solver subprocess and return a FormalResult.

    *argv* should be the full command, e.g.
    ``["path/to/boolector", "--smt2", "file.smt2"]``.

    Peak memory is captured via ``resource.getrusage(RUSAGE_CHILDREN)``.
    """
    bench_name = benchmark or smt2_path.stem

    # Snapshot child RSS before the run so we can isolate this call
    pre_rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss

    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            list(argv),
            capture_output=True,
            text=True,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired:
        dt_ms = (time.monotonic() - t0) * 1000.0
        return FormalResult(
            solver=solver_name,
            benchmark=bench_name,
            result="timeout",
            solve_time_ms=dt_ms,
            peak_memory_kb=0,
            returncode=-1,
            stdout="",
            stderr=f"Timed out after {timeout_s}s",
        )
    dt_ms = (time.monotonic() - t0) * 1000.0

    post_rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    # On Linux ru_maxrss is in KB already
    peak_kb = max(post_rss - pre_rss, 0) or post_rss

    if proc.returncode != 0 and not proc.stdout.strip():
        result_str = "error"
    else:
        result_str = _parse_result(proc.stdout)

    return FormalResult(
        solver=solver_name,
        benchmark=bench_name,
        result=result_str,
        solve_time_ms=dt_ms,
        peak_memory_kb=peak_kb,
        returncode=proc.returncode,
        stdout=proc.stdout,
        stderr=proc.stderr,
    )


def find_binary(bundled: Path, name: str) -> str | None:
    """Return the solver binary path, preferring a bundled copy."""
    if bundled.is_file() and bundled.stat().st_mode & 0o111:
        return str(bundled)
    return shutil.which(name)
