"""Run the ab_compare.py perf surfaces under alloc_trace.so.

    env -u LD_LIBRARY_PATH python tests/perf/alloc_trace/run_workloads.py \\
        --build build-alloc --shim alloc_trace.so --out DIR [--n 50] \\
        [--only swizzle,api,fixtures,field]

Writes DIR/<surface>.<pid> trace files and prints, per surface, the
number of workload operations (randomize calls, solves, fixture runs)
to pass to alloc_report.py --calls. The build should carry debug info
(-g) so frames symbolize.
"""
from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(_REPO), str(_REPO / "src")]

from tests.perf import ab_compare, vlt_protocol                     # noqa: E402
from tests.perf.rand_benches import Bench                           # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--shim", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=50)
    ap.add_argument("--only", default="swizzle,api,fixtures,field")
    args = ap.parse_args()
    build = Path(args.build).resolve()
    shim = str(Path(args.shim).resolve())
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    def env(surface):
        return {**os.environ, "LD_PRELOAD": shim, "ALLOC_TRACE_OUT": str(out / surface),
                "DVS_SOLVER_PATH": str(build / "lib")}

    only = args.only.split(",")
    if "swizzle" in only:
        ops = 0
        for name in ab_compare.BENCHES:
            b = Bench(name)
            e = env("swizzle")
            argv = ["env", "LD_PRELOAD=" + e["LD_PRELOAD"], "ALLOC_TRACE_OUT=" + e["ALLOC_TRACE_OUT"],
                    str(build / "dv-solve-smt2"), "--interactive", "--mode=verilator"]
            s = vlt_protocol.Session(argv, b.widths, b.smt_vlt())
            rng = random.Random(1)
            try:
                for _ in range(args.n):
                    s.randomize(rng)
                    ops += 1
            finally:
                s.close()
        print("swizzle ops", ops)
    if "api" in only:
        code = ab_compare._API.format(repo=str(_REPO), src=str(_REPO / "src"),
                                      names=ab_compare.BENCHES, n=args.n)
        p = subprocess.run([sys.executable, "-c", code], env=env("api"),
                           capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(p.stderr[-2000:])
        print("api ops", args.n * len(p.stdout.splitlines()))
    if "fixtures" in only:
        fixtures = sorted(f for s in ab_compare.FIXTURE_SETS
                          for f in (_REPO / "tests/formal/smt2" / s).glob("*.smt2"))
        for f in fixtures:
            subprocess.run([str(build / "dv-solve-smt2"), str(f)], env=env("fixtures"),
                           capture_output=True, timeout=300)
        print("fixtures ops", len(fixtures))
    if "field" in only:
        p = subprocess.run([sys.executable, str(_REPO / "tests/perf/field_bench.py"),
                            str(args.n)], env=env("field"), capture_output=True, text=True)
        if p.returncode:
            raise RuntimeError(p.stderr[-2000:])
        print("field shapes", len(ab_compare._FIELD.findall(p.stdout)), "x", args.n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
