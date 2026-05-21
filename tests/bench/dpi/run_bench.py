"""Run the DPI vs std::randomize benchmark on available simulators."""
import os
import sys
import shutil
import asyncio
from pathlib import Path

# Add packages to path
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "src"))

from dv_flow.mgr import TaskGraphBuilder, PackageLoader, TaskSetRunner, TaskListenerLog

BENCH_DIR = Path(__file__).parent
PKG_DIR = Path(__file__).parent.parent.parent.parent  # zuspec-solver root
SV_PKG_DIR = PKG_DIR / "src" / "sv"
C_SRC_DIR = PKG_DIR / "src" / "c"

N = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
SIM = sys.argv[2] if len(sys.argv) > 2 else "mti"


def run_bench(sim, n, tmpdir):
    builder = TaskGraphBuilder(
        None, tmpdir,
        loader=PackageLoader().load_rgy(["std", f"hdlsim.{sim}"]))

    # SV packages
    sv_pkg = builder.mkTaskNode(
        "std.FileSet", name="sv_pkg",
        type="systemVerilogSource",
        base=str(SV_PKG_DIR),
        include="*.sv")

    # Benchmark SV + timing C
    bench_sv = builder.mkTaskNode(
        "std.FileSet", name="bench_sv",
        type="systemVerilogSource",
        base=str(BENCH_DIR),
        include="bench_dpi_vs_std.sv")

    bench_c = builder.mkTaskNode(
        "std.FileSet", name="bench_c",
        type="cSource",
        base=str(BENCH_DIR),
        include="bench_time.c")

    # Solver C sources
    solver_c = builder.mkTaskNode(
        "std.FileSet", name="solver_c",
        type="cSource",
        base=str(C_SRC_DIR),
        include="*.c")

    sim_img = builder.mkTaskNode(
        f"hdlsim.{sim}.SimImage",
        name="sim_img",
        needs=[sv_pkg, bench_sv, bench_c, solver_c],
        top=["bench_dpi_vs_std"])

    sim_run = builder.mkTaskNode(
        f"hdlsim.{sim}.SimRun",
        name="sim_run",
        needs=[sim_img],
        plusargs=[f"N={n}"])

    runner = TaskSetRunner(tmpdir, builder=builder)
    runner.add_listener(TaskListenerLog().event)
    asyncio.run(runner.run(sim_run))

    # Print sim.log
    log_path = os.path.join(tmpdir, "sim_run", "sim.log")
    if os.path.isfile(log_path):
        with open(log_path) as f:
            for line in f:
                if "BENCH" in line or "===" in line or "Error" in line:
                    print(line.rstrip().lstrip("# "))


if __name__ == "__main__":
    import tempfile
    with tempfile.TemporaryDirectory(prefix="dpi_bench_") as tmpdir:
        run_bench(SIM, N, tmpdir)
