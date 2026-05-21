"""SystemVerilog simulator back-end for benchmarks.

Uses :class:`~zuspec.solver.bench_harness.SolvePerfHarnessGenerator` to
auto-generate a complete SV harness from any ``@zdc.dataclass``, then compiles
and runs it via DV Flow.  No hand-written ``.sv`` files are required.
"""
from __future__ import annotations

import asyncio
import re
import shutil
from pathlib import Path

import pytest
import zuspec.dataclasses as zdc

from .base import BenchResult, RESULTS_DIR, get_bench_config

# sv_bench_time.c lives alongside the old SV corpus — a single canonical copy.
_C_DIR = Path(__file__).parent.parent / "sim" / "data"

_SIM_EXECUTABLES: dict[str, str] = {
    "verilator": "vlt",
    "vsim":      "mti",
    "vcs":       "vcs",
    "xmsim":     "xcm",
}

# Default n_solutions for the SV loop (large enough that the timeout
# controls the actual duration for fast solvers).
_DEFAULT_SIM_N = 100_000


def _dv_flow_run(tmp_path: Path, sim_id: str, sv_file: Path,
                 n_solutions: int, timeout_ns: int):
    """Compile *sv_file* with *sim_id* and run; return ``(bench_ns, solutions)``."""
    try:
        from dv_flow.mgr import (
            TaskSetRunner, PackageLoader, TaskListenerLog,
        )
        from dv_flow.mgr.task_graph_builder import TaskGraphBuilder
    except ImportError:
        pytest.skip("dv_flow not installed")

    rundir = str(tmp_path / "rundir")

    builder = TaskGraphBuilder(
        PackageLoader().load_rgy(["std", "hdlsim.%s" % sim_id]),
        rundir,
    )
    runner = TaskSetRunner(rundir)
    runner.builder = builder

    sv_files = builder.mkTaskNode(
        "std.FileSet",
        name="sv",
        type="systemVerilogSource",
        base=str(sv_file.parent),
        include=sv_file.name,
    )
    c_files = builder.mkTaskNode(
        "std.FileSet",
        name="c",
        type="cSource",
        base=str(_C_DIR),
        include="sv_bench_time.c",
    )
    sim_img = builder.mkTaskNode(
        "hdlsim.%s.SimImage" % sim_id,
        name="sim_img",
        needs=[sv_files, c_files],
        top=["harness"],
    )

    plusargs = [
        "n_solutions=%d" % n_solutions,
    ]
    if timeout_ns > 0:
        plusargs.append("timeout_ns=%d" % timeout_ns)

    sim_run = builder.mkTaskNode(
        "hdlsim.%s.SimRun" % sim_id,
        name="sim_run",
        needs=[sim_img],
        plusargs=plusargs,
    )

    runner.add_listener(TaskListenerLog().event)
    out_l = asyncio.run(runner.run([sim_run]))

    assert runner.status == 0, (
        "DV Flow run failed (sim=%s); check %s/sim_run/sim.log" % (sim_id, rundir)
    )

    rundir_fs = None
    for out in out_l:
        for fs in out.output:
            if getattr(fs, "filetype", None) == "simRunDir":
                rundir_fs = fs
                break

    assert rundir_fs is not None, "SimRun did not produce a simRunDir fileset"

    log_path = Path(rundir_fs.basedir) / "sim.log"
    content = log_path.read_text()

    m = re.search(r"BENCH_NS (\d+)", content)
    assert m is not None, "BENCH_NS marker not found in sim.log:\n" + content[:2000]
    bench_ns = int(m.group(1))

    raw_solutions = []
    for line in content.splitlines():
        # Simulators prefix $display output differently:
        #   MTI:     "# "
        #   VCS:     no prefix (or occasional timestamp)
        #   Xcelium: "xmsim: *W,RNQUIE: ..." warnings, but $display is clean
        # Strip common prefixes so SOL/BENCH_NS parsing is uniform.
        stripped = re.sub(r"^#\s*", "", line)
        if stripped.startswith("SOL "):
            raw_solutions.append(list(map(int, stripped.split()[1:])))
    return bench_ns, raw_solutions


class SimSolver:
    def __init__(self, sim_id: str, executable: str) -> None:
        self._sim_id = sim_id
        self._exe    = executable

    def __str__(self) -> str:
        return f"sim-{self._sim_id}"

    def bench(
        self,
        cls: type,
        validate,
        tmp_path: Path,
        n_solutions: int = 1000,
        min_bench_ns: int = 1_000_000_000,
    ) -> None:
        if not shutil.which(self._exe):
            pytest.skip(f"{self._exe} not found on PATH")

        try:
            from zuspec.solver.bench_harness import SolvePerfHarnessGenerator
        except ImportError:
            pytest.skip("zuspec.solver.bench_harness not available")

        cfg = get_bench_config()
        timeout_ns = int(cfg.timeout_secs * 1e9)

        try:
            sv_text = SolvePerfHarnessGenerator().emit(
                cls, n_solutions=_DEFAULT_SIM_N)
        except (ImportError, ModuleNotFoundError) as exc:
            pytest.skip(f"SV harness generator dependency missing: {exc}")
        sv_file = tmp_path / "harness.sv"
        sv_file.write_text(sv_text)

        bench_ns, raw_solutions = _dv_flow_run(
            tmp_path, self._sim_id, sv_file, _DEFAULT_SIM_N, timeout_ns)

        # Map raw integer lists to dicts using field-declaration order.
        field_names = [f["name"] for f in zdc.extract_rand_fields(cls)]
        solutions = [
            dict(zip(field_names, vals)) for vals in raw_solutions
        ]

        if len(solutions) < cfg.min_solutions:
            pytest.skip(
                f"{self}: only {len(solutions)} solutions in "
                f"{bench_ns/1e9:.1f}s on {cls.__name__}"
            )

        # Spot-check the last few solutions.
        for sol in solutions[-5:]:
            validate(sol)

        n = len(solutions)
        result = BenchResult(str(self), cls.__name__.lower(), n, bench_ns)
        result.save(RESULTS_DIR)
        print("\n" + result.summary())
