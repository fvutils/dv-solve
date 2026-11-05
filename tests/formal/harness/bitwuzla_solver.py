"""BitwuzlaSolver -- subprocess wrapper for bitwuzla."""
from __future__ import annotations

from pathlib import Path

from .protocol import FormalResult
from ._subprocess_solver import find_binary, run_smt2_solver

_REPO_ROOT = Path(__file__).resolve().parents[3]
_BUNDLED = _REPO_ROOT / "packages" / "verilator" / "bin" / "bitwuzla"


class BitwuzlaSolver:
    """Non-incremental bitwuzla via ``bitwuzla <file>``."""

    @property
    def name(self) -> str:
        return "bitwuzla"

    def is_available(self) -> bool:
        return find_binary(_BUNDLED, "bitwuzla") is not None

    def solve(
        self, smt2_path: Path, *, timeout_s: float = 30.0
    ) -> FormalResult:
        exe = find_binary(_BUNDLED, "bitwuzla")
        if exe is None:
            return FormalResult(
                solver=self.name,
                benchmark=smt2_path.stem,
                result="error",
                solve_time_ms=0.0,
                peak_memory_kb=0,
                returncode=-1,
                stdout="",
                stderr="bitwuzla binary not found",
            )
        return run_smt2_solver(
            self.name,
            [exe, str(smt2_path)],
            smt2_path,
            timeout_s=timeout_s,
        )
