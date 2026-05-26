"""dv-solve-smt2 subprocess wrappers.

Two engines are exposed:

  * ``DvSolveSMT2Solver`` — default CDCL engine.
  * ``DvSolveSMT2BBSolver`` — bitblast engine (DV_ENGINE=bitblast).

Same binary, different DV_ENGINE env. Separate wrapper classes so
cross-check tests can parametrize cleanly and so per-engine timing /
RSS numbers live in distinct FormalResults.
"""
from __future__ import annotations

from pathlib import Path

from .protocol import FormalResult
from ._subprocess_solver import find_binary, run_smt2_solver

_REPO_ROOT = Path(__file__).resolve().parents[3]
_BUILD_DIR = _REPO_ROOT / "build"


def _solve_with_engine(
    solver_name: str,
    smt2_path: Path,
    timeout_s: float,
    engine_env: dict[str, str] | None,
) -> FormalResult:
    exe = _BUILD_DIR / "dv-solve-smt2"
    if not exe.is_file():
        return FormalResult(
            solver=solver_name,
            benchmark=smt2_path.stem,
            result="error",
            solve_time_ms=0.0,
            peak_memory_kb=0,
            returncode=-1,
            stdout="",
            stderr="dv-solve-smt2 binary not found",
        )
    return run_smt2_solver(
        solver_name,
        [str(exe), str(smt2_path)],
        smt2_path,
        timeout_s=timeout_s,
        env_overrides=engine_env,
    )


class DvSolveSMT2Solver:
    """Default CDCL-engine dv-solve-smt2 solver."""

    @property
    def name(self) -> str:
        return "dv-solve-smt2"

    def is_available(self) -> bool:
        exe = _BUILD_DIR / "dv-solve-smt2"
        return exe.is_file() and exe.stat().st_mode & 0o111 != 0

    def solve(
        self, smt2_path: Path, *, timeout_s: float = 30.0
    ) -> FormalResult:
        return _solve_with_engine(self.name, smt2_path, timeout_s, None)


class DvSolveSMT2BBSolver:
    """Bitblast-engine dv-solve-smt2 solver (DV_ENGINE=bitblast).

    The bitblast engine routes the SMT problem through zsp_bbsolver
    (AIG + Tseitin + kissat) rather than the CDCL theory propagators.
    Substantially faster than CDCL on QF_UFBV / array-heavy BMC
    fixtures (measured: every CDCL-timing-out tier2/tier3 BMC fixture
    solves in <20ms here)."""

    @property
    def name(self) -> str:
        return "dv-solve-smt2-bb"

    def is_available(self) -> bool:
        exe = _BUILD_DIR / "dv-solve-smt2"
        return exe.is_file() and exe.stat().st_mode & 0o111 != 0

    def solve(
        self, smt2_path: Path, *, timeout_s: float = 30.0
    ) -> FormalResult:
        return _solve_with_engine(
            self.name, smt2_path, timeout_s, {"DV_ENGINE": "bitblast"}
        )
