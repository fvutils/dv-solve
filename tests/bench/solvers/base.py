"""Base types shared by all solver back-ends."""
from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Protocol

# Canonical results directory — one JSON file per (scenario, solver) pair.
RESULTS_DIR = Path(__file__).parent.parent / "results"


# ---------------------------------------------------------------------------
# Adaptive benchmark configuration
# ---------------------------------------------------------------------------

@dataclass
class BenchConfig:
    """Controls how long benchmarks run.

    Solvers run until *target_secs* of wall-clock time has elapsed **and**
    at least *min_solutions* have been produced.  If *timeout_secs* is
    exceeded first, the solver reports whatever it has (or skips).

    Defaults can be overridden via ``--bench-target-secs`` /
    ``--bench-timeout-secs`` pytest CLI options or the ``ZSP_BENCH_TARGET``
    / ``ZSP_BENCH_TIMEOUT`` environment variables.
    """
    target_secs:  float = 5.0
    timeout_secs: float = 10.0
    min_solutions: int  = 10


def get_bench_config() -> BenchConfig:
    """Return the active benchmark config, respecting env-var overrides."""
    return BenchConfig(
        target_secs=float(os.environ.get("ZSP_BENCH_TARGET", "5.0")),
        timeout_secs=float(os.environ.get("ZSP_BENCH_TIMEOUT", "10.0")),
        min_solutions=int(os.environ.get("ZSP_BENCH_MIN_SOL", "10")),
    )


@dataclass
class BenchResult:
    solver:      str
    scenario:    str
    n_solutions: int
    bench_ns:    int

    @property
    def ns_per_solution(self) -> float:
        return self.bench_ns / max(self.n_solutions, 1)

    @property
    def solutions_per_s(self) -> float:
        return self.n_solutions / (self.bench_ns / 1e9) if self.bench_ns > 0 else 0.0

    def summary(self) -> str:
        return (
            f"[{self.solver} / {self.scenario}]  {self.n_solutions} solutions  "
            f"{self.bench_ns/1e6:.1f} ms  "
            f"{self.ns_per_solution/1e3:.1f} µs/sol  "
            f"{self.solutions_per_s:.0f} sol/s"
        )

    def save(self, results_dir: Path = RESULTS_DIR) -> None:
        results_dir.mkdir(parents=True, exist_ok=True)
        path = results_dir / f"{self.scenario}_{self.solver}.json"
        path.write_text(json.dumps({
            "solver":          self.solver,
            "scenario":        self.scenario,
            "n_solutions":     self.n_solutions,
            "bench_ns":        self.bench_ns,
            "ns_per_solution": self.ns_per_solution,
            "solutions_per_s": self.solutions_per_s,
        }, indent=2))


class Solver(Protocol):
    def __str__(self) -> str:
        """Return the parametrize ID: 'python', 'native', 'sim-vlt', ..."""
        ...

    def bench(
        self,
        cls: type,
        validate: Callable[[dict], None],
        tmp_path: Path,
        n_solutions: int = 1000,
        min_bench_ns: int = 1_000_000_000,
    ) -> None:
        """
        Run the randomization benchmark for *cls*, assert correctness via
        *validate*, then print and persist throughput results.

        *validate* receives a plain dict[str, int] regardless of back-end.

        Call pytest.skip() if this solver is unavailable on the current host.
        """
        ...
