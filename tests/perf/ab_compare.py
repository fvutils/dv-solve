"""A/B wall time of two dv-solve builds on the quick perf surfaces.

Used as the perf gate of docs/dynamic_pool_plan.md: a change to the solver's
memory layout must stay within noise of the build before it. Each surface
runs both builds interleaved, so machine drift hits both alike, and keeps
the best of `--reps` runs per item.

  swizzle  every tests/perf/rand_benches benchmark through Verilator 5.046's
           randomize() protocol (vlt_protocol.Session), N calls per process
  api      the same benchmarks through SolveCtx: reset + solve, N times
  fixtures dv-solve-smt2 one-shot on the easy-band fixture sets
  field    tests/perf/field_bench.py shapes (mean per randomize)

    env -u LD_LIBRARY_PATH python tests/perf/ab_compare.py \\
        --a <base-build> --b build [--reps 5] [--n 200] [--only swizzle,api]

A build is a CMake binary directory holding dv-solve-smt2 and lib/.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(_REPO), str(_REPO / "src")]

from tests.perf import vlt_protocol                                 # noqa: E402
from tests.perf.rand_benches import Bench, ENUMERABLE, WIDE          # noqa: E402

FIXTURE_SETS = ("tier1", "verilator", "gaps")
BENCHES = sorted(ENUMERABLE) + sorted(WIDE)


def _swizzle(build: Path, b: Bench, n: int) -> float:
    rng = random.Random(1)
    s = vlt_protocol.Session([str(build / "dv-solve-smt2"), "--interactive",
                              "--mode=verilator"], b.widths, b.smt_vlt())
    t0 = time.perf_counter()
    try:
        for _ in range(n):
            s.randomize(rng)
    finally:
        dt = time.perf_counter() - t0
        s.close()
    return dt


_API = r"""
import random, sys, time
sys.path[:0] = [{repo!r}, {src!r}]
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK, CompileIncompleteError
from tests.formal.soundness.doors import _lower
from tests.perf.rand_benches import Bench
for name in {names!r}:
    b = Bench(name)
    ids = {{k: i for i, k in enumerate(b.names)}}
    bld = SolveProblemBuilder()
    for k in b.names:
        w = b.widths[k]
        bld.add_var(ids[k], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in b.cons:
        bld.add_constraint(_lower(bld, c, ids))
    prob, _ = bld.finalize()
    rng = random.Random(1)
    try:
        ctx = SolveCtx(prob)
    except CompileIncompleteError:
        continue                    # not native to the API; swizzle covers it
    with ctx:
        t0 = time.perf_counter()
        for _ in range({n}):
            ctx.reset()
            assert ctx.solve(seed=rng.getrandbits(63) + 1, fair_pick=True) == SOLVE_OK
        print(name, time.perf_counter() - t0)
"""


def _api(build: Path, n: int) -> dict:
    code = _API.format(repo=str(_REPO), src=str(_REPO / "src"), names=BENCHES, n=n)
    env = {**os.environ, "DVS_SOLVER_PATH": str(build / "lib")}
    p = subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"api arm failed on {build}:\n{p.stderr[-2000:]}")
    out = p.stdout
    return {ln.split()[0]: float(ln.split()[1]) for ln in out.splitlines()}


def _fixture(build: Path, f: Path) -> float:
    t0 = time.perf_counter()
    subprocess.run([str(build / "dv-solve-smt2"), str(f)], capture_output=True,
                   timeout=60)
    return time.perf_counter() - t0


_FIELD = re.compile(r"^(.*?)\s+mean=\s*([\d.]+)us", re.M)


def _field(build: Path, n: int) -> dict:
    env = {**os.environ, "DVS_SOLVER_PATH": str(build / "lib")}
    out = subprocess.run([sys.executable, str(_REPO / "tests/perf/field_bench.py"), str(n)],
                         env=env, check=True, capture_output=True, text=True).stdout
    return {k.strip(): float(v) * 1e-6 for k, v in _FIELD.findall(out)}


def _best(a: dict, run) -> None:
    for k, v in run.items():
        a[k] = min(a.get(k, v), v)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="baseline build dir")
    ap.add_argument("--b", required=True, help="candidate build dir")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--n", type=int, default=200, help="randomize calls per benchmark")
    ap.add_argument("--only", default="swizzle,api,fixtures,field")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    builds = {"a": Path(args.a).resolve(), "b": Path(args.b).resolve()}
    only = args.only.split(",")
    fixtures = sorted(f for s in FIXTURE_SETS
                      for f in (_REPO / "tests/formal/smt2" / s).glob("*.smt2"))
    best = {s: {"a": {}, "b": {}} for s in only}
    for rep in range(args.reps):
        order = ("a", "b") if rep % 2 == 0 else ("b", "a")
        for side in order:
            bd = builds[side]
            if "swizzle" in only:
                _best(best["swizzle"][side], {n: _swizzle(bd, Bench(n), args.n) for n in BENCHES})
            if "api" in only:
                _best(best["api"][side], _api(bd, args.n))
            if "fixtures" in only:
                _best(best["fixtures"][side], {f.name: _fixture(bd, f) for f in fixtures})
            if "field" in only:
                _best(best["field"][side], _field(bd, args.n))
    print(f"{'surface':10s} {'items':>5s} {'A total':>10s} {'B total':>10s} {'B/A':>6s}")
    for s in only:
        a, b = best[s]["a"], best[s]["b"]
        keys = sorted(set(a) & set(b))
        ta, tb = sum(a[k] for k in keys), sum(b[k] for k in keys)
        print(f"{s:10s} {len(keys):5d} {ta * 1e3:8.1f}ms {tb * 1e3:8.1f}ms {tb / ta:6.3f}")
        if args.verbose:
            for k in keys:
                print(f"   {k:40s} {a[k] * 1e3:9.3f} {b[k] * 1e3:9.3f} {b[k] / a[k]:6.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
