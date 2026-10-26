"""Distribution accuracy of the accuracy benchmarks, per arm (docs/accuracy_closure_plan.md).

    python3 -m tests.perf.accuracy_check --build build [--n 4000] [--arms capi,api,seed,swizzle]

`--build` is a CMake build directory holding `dv-solve-smt2` and `lib/`. For
each benchmark and arm it prints excess JSD (the arm's JSD minus that of an
exact uniform draw of the same N) and a chi-square p-value with the bins whose
expected count is below 5 pooled, which is the plan's uniformity gate.
"""
from __future__ import annotations

import argparse
import math
import os
from collections import Counter
from pathlib import Path

from tests.formal.soundness.ir import solutions
from tests.perf.rand_benches import Bench
from tests.perf.run_rand import Arm, _key, truth, _api, _capi, _seeded, _swizzle, _uniform, _seed

ACCURACY = ["cond_weight", "order_chain_a0", "order_chain_a5", "order_chain_a9",
            "sum_unique", "linear_sys", "sum3_const"]


def pooled_chi2p(b: Bench, models: list) -> float:
    from scipy.stats import chi2
    tr = truth(b)
    n = len(models)
    cnt = Counter(_key(b, m) for m in models)
    stat, df, po, pe = 0.0, -1, 0, 0.0
    for k, p in tr.items():
        if n * p >= 5:
            stat += (cnt.get(k, 0) - n * p) ** 2 / (n * p)
            df += 1
        else:
            po += cnt.get(k, 0)
            pe += n * p
    if pe > 0:
        stat += (po - pe) ** 2 / pe
        df += 1
    return float(chi2.sf(stat, max(df, 1)))


def jsd(b: Bench, models: list) -> float:
    tr = truth(b)
    n = len(models)
    cnt = Counter(_key(b, m) for m in models)
    keys = set(tr) | set(cnt)
    p = {k: cnt.get(k, 0) / n for k in keys}
    out = 0.0
    for k in keys:
        q = tr.get(k, 0.0)
        mid = (p[k] + q) / 2
        if p[k] > 0:
            out += 0.5 * p[k] * math.log2(p[k] / mid)
        if q > 0:
            out += 0.5 * q * math.log2(q / mid)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--n", type=int, default=4000)
    ap.add_argument("--arms", default="capi,api,seed,swizzle")
    ap.add_argument("--only", nargs="*", default=ACCURACY)
    a = ap.parse_args()
    build = Path(a.build).resolve()
    # Every arm (and the builder) must load this build's library, before any
    # import of dv_solve loads another.
    os.environ["DVS_SOLVER_PATH"] = str(build / "lib")
    smt2 = str(build / "dv-solve-smt2")
    arms = {"api": Arm("api", "api", env={"DVS_SOLVER_PATH": str(build / "lib")}),
            "capi": Arm("capi", "capi", [str(build / "bench_rand_capi")],
                        env={"DVS_SOLVER_PATH": str(build / "lib")}),
            "seed": Arm("seed", "seed", [smt2]),
            "swizzle": Arm("swizzle", "swizzle", [smt2, "--interactive", "--mode=verilator"])}
    sel = [arms[x] for x in a.arms.split(",")]
    print(f"{'bench':16} {'arm':8} {'excess JSD':>10} {'chi2 p':>9} {'distinct':>8} {'p50 us':>8}")
    for name in a.only:
        b = Bench(name)
        space = None if b.bins is not None else \
            [tuple(m[k] for k in b.names) for m in solutions(b.widths, b.cons)]
        ref = _uniform(b, a.n, _seed(name, "uniform"), space)["models"]
        j0 = jsd(b, ref)
        for arm in sel:
            seed = _seed(name, arm.name)
            try:
                r = (_api(b, a.n, seed, arm.env) if arm.kind == "api" else
                     _capi(b, arm, a.n, seed) if arm.kind == "capi" else
                     _seeded(b, arm, a.n, seed) if arm.kind == "seed" else
                     _swizzle(b, arm, a.n, seed))
            except Exception as e:                      # report, keep going
                print(f"{name:16} {arm.name:8} error: {str(e)[:80]}")
                continue
            m = r["models"]
            w = sorted(r["wall_s"]) if r["wall_s"] else [0]
            print(f"{name:16} {arm.name:8} {jsd(b, m) - j0:10.4f} {pooled_chi2p(b, m):9.2g} "
                  f"{len(set(m)) / len(m):8.3f} {w[len(w) // 2] * 1e6:8.1f}", flush=True)


if __name__ == "__main__":
    main()
