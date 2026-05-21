"""FormalSolver protocol and FormalResult dataclass.

These types define the interface every solver wrapper must satisfy and
the data shape used by ResultsCollector.
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Protocol, runtime_checkable


@dataclass
class FormalResult:
    """Outcome of running a single SMT-LIB2 file through a solver."""

    solver: str             # "boolector", "bitwuzla", "z3", "cvc5"
    benchmark: str          # e.g. "axi4burst"
    result: str             # "sat" | "unsat" | "unknown" | "error" | "timeout"
    solve_time_ms: float    # wall-clock milliseconds
    peak_memory_kb: int     # peak RSS in KB (from resource module)
    returncode: int         # solver process exit code
    stdout: str             # raw solver output
    stderr: str             # raw solver stderr

    def summary(self) -> str:
        return (
            f"[{self.solver} / {self.benchmark}]  "
            f"{self.result}  {self.solve_time_ms:.1f} ms  "
            f"{self.peak_memory_kb} KB"
        )

    def to_dict(self) -> dict:
        return asdict(self)

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(self.to_dict(), indent=2))


@runtime_checkable
class FormalSolver(Protocol):
    """Protocol that all formal solver wrappers must satisfy."""

    @property
    def name(self) -> str:
        """Short identifier for this solver, e.g. 'boolector'."""
        ...

    def solve(
        self, smt2_path: Path, *, timeout_s: float = 30.0
    ) -> FormalResult:
        """Run the solver on an SMT-LIB2 file and return the result."""
        ...

    def is_available(self) -> bool:
        """Return True if the solver binary is found on this system."""
        ...
