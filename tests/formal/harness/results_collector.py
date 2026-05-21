"""Collect FormalResult instances and write CSV / JSON / Markdown summaries."""
from __future__ import annotations

import csv
import json
from io import StringIO
from pathlib import Path
from typing import List

from .protocol import FormalResult

_RESULTS_DIR = Path(__file__).resolve().parents[1] / "results"

CSV_COLUMNS = [
    "benchmark",
    "solver",
    "result",
    "solve_time_ms",
    "peak_memory_kb",
    "depth",
]


class ResultsCollector:
    """Accumulates FormalResult objects and writes summary files."""

    def __init__(self, results_dir: Path | None = None) -> None:
        self.results_dir = results_dir or _RESULTS_DIR
        self._results: List[FormalResult] = []

    def add(self, result: FormalResult) -> None:
        self._results.append(result)

    @property
    def results(self) -> List[FormalResult]:
        return list(self._results)

    # -- Per-run JSON -------------------------------------------------------

    def save_individual(self, result: FormalResult) -> Path:
        """Write a single per-(benchmark, solver) JSON file."""
        path = self.results_dir / f"{result.benchmark}_{result.solver}.json"
        result.save(path)
        return path

    # -- Summary CSV --------------------------------------------------------

    def write_csv(self, path: Path | None = None) -> Path:
        path = path or (self.results_dir / "tier1_baseline.csv")
        path.parent.mkdir(parents=True, exist_ok=True)

        rows = sorted(self._results, key=lambda r: (r.benchmark, r.solver))
        with open(path, "w", newline="") as fh:
            writer = csv.writer(fh)
            writer.writerow(CSV_COLUMNS)
            for r in rows:
                writer.writerow([
                    r.benchmark,
                    r.solver,
                    r.result,
                    f"{r.solve_time_ms:.1f}",
                    r.peak_memory_kb,
                    "",  # depth -- N/A for Tier 1
                ])
        return path

    # -- Summary Markdown ---------------------------------------------------

    def write_markdown(self, path: Path | None = None) -> Path:
        path = path or (self.results_dir / "tier1_baseline.md")
        path.parent.mkdir(parents=True, exist_ok=True)

        # Pivot: one row per benchmark, columns per solver
        solvers = sorted({r.solver for r in self._results})
        benchmarks = sorted({r.benchmark for r in self._results})
        lookup = {(r.benchmark, r.solver): r for r in self._results}

        lines: list[str] = []
        lines.append("# Tier 1 Baseline Results\n")

        # Header
        hdr = "| Benchmark |"
        sep = "|-----------|"
        for s in solvers:
            hdr += f" {s} (ms) | {s} result | {s} mem (KB) |"
            sep += "----------:|:----------:|------------:|"
        lines.append(hdr)
        lines.append(sep)

        for b in benchmarks:
            row = f"| {b} |"
            for s in solvers:
                r = lookup.get((b, s))
                if r:
                    row += f" {r.solve_time_ms:.1f} | {r.result} | {r.peak_memory_kb} |"
                else:
                    row += " -- | -- | -- |"
            lines.append(row)

        lines.append("")
        path.write_text("\n".join(lines))
        return path

    # -- Convenience --------------------------------------------------------

    def write_summary(self) -> tuple[Path, Path]:
        """Write both CSV and Markdown summaries, return their paths."""
        csv_path = self.write_csv()
        md_path = self.write_markdown()
        return csv_path, md_path
