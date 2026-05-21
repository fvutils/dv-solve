"""DvSolveSMT2Solver -- subprocess wrapper for dv-solve-smt2."""
from __future__ import annotations

from pathlib import Path

from .protocol import FormalResult
from ._subprocess_solver import find_binary, run_smt2_solver

_REPO_ROOT = Path(__file__).resolve().parents[3]
_BUILD_DIR = _REPO_ROOT / "build"


class DvSolveSMT2Solver:
    """Non-incremental dv-solve-smt2 solver."""

    @property
    def name(self) -> str:
        return "dv-solve-smt2"

    def is_available(self) -> bool:
        exe = _BUILD_DIR / "dv-solve-smt2"
        return exe.is_file() and exe.stat().st_mode & 0o111 != 0

    def solve(
        self, smt2_path: Path, *, timeout_s: float = 30.0
    ) -> FormalResult:
        exe = _BUILD_DIR / "dv-solve-smt2"
        if not exe.is_file():
            return FormalResult(
                solver=self.name,
                benchmark=smt2_path.stem,
                result="error",
                solve_time_ms=0.0,
                peak_memory_kb=0,
                returncode=-1,
                stdout="",
                stderr="dv-solve-smt2 binary not found",
            )
        return run_smt2_solver(
            self.name,
            [str(exe), str(smt2_path)],
            smt2_path,
            timeout_s=timeout_s,
        )
