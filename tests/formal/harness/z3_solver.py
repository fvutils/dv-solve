"""Z3Solver -- subprocess wrapper for z3 (optional)."""
from __future__ import annotations

from pathlib import Path

from .protocol import FormalResult
from ._subprocess_solver import find_binary, run_smt2_solver


class Z3Solver:
    """Non-incremental z3 via ``z3 -smt2 <file>``."""

    @property
    def name(self) -> str:
        return "z3"

    def is_available(self) -> bool:
        return find_binary(Path("/nonexistent"), "z3") is not None

    def solve(
        self, smt2_path: Path, *, timeout_s: float = 30.0
    ) -> FormalResult:
        exe = find_binary(Path("/nonexistent"), "z3")
        if exe is None:
            return FormalResult(
                solver=self.name,
                benchmark=smt2_path.stem,
                result="error",
                solve_time_ms=0.0,
                peak_memory_kb=0,
                returncode=-1,
                stdout="",
                stderr="z3 binary not found",
            )
        return run_smt2_solver(
            self.name,
            [exe, "-smt2", str(smt2_path)],
            smt2_path,
            timeout_s=timeout_s,
        )
