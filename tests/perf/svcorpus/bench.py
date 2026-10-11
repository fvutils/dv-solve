"""Time and spread of the corpus cases, per solver build, solver alone.

Each arm is a libdv_solve build (DVS_SOLVER_PATH), run in its own process:
compile the case's builder problem once, then N seeded solves, as a native
front end (zuspec, SV DPI) drives it -- reset, solve(seed), read the values.
Per case and arm:

  solve     median / mean / p90 time of solve() (us); compile time
  bad       models that break the case: checked against the Verilator
            capture (ir.ev, independent of the builder), and validate_model()
  rc        solves that did not return a model
  spread    per key (each variable; a wide one in 16 equal buckets), the
            Jensen-Shannon divergence from a reference sample of the
            constraints' SystemVerilog meaning, minus the same divergence for
            a second reference sample of the same size (the noise floor):
            0 is as good as an ideal sampler. The reference draws the
            `solve ... before` variables first, each feasible combination
            alike, then the rest uniformly, `dist` variables by their
            weights; by rejection, so a case whose solutions are too sparse
            in the declared domains has no reference ("-").

Usage:
  python -m tests.perf.svcorpus.bench [--n N] [--arm NAME=LIBDIR]... [CASE...]
With no --arm: build/lib as "now". Writes perf-out/svcorpus/<stamp>.json and
prints a table.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import random
import statistics
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[3]


# ---- keys and the reference ------------------------------------------------

def keys(case) -> dict:
    """name -> function(model dict) -> bin."""
    ks = {}
    for n, w, _ in case.VARS:
        if w <= 6:
            ks[n] = (lambda n: lambda m: m[n])(n)
        else:
            ks[n] = (lambda n, w: lambda m: m[n] >> (w - 4))(n, w)
    ks.update(getattr(case, "KEYS", {}))
    return ks


def _draw_var(rng, n, w, dist, box=None):
    if box and n in box:
        return rng.randint(*box[n])
    if n in dist:
        es = dist[n]
        tot = sum(wt * (hi - lo + 1 if pv else 1) for lo, hi, wt, pv in es)
        r = rng.random() * tot
        for lo, hi, wt, pv in es:
            r -= wt * (hi - lo + 1 if pv else 1)
            if r < 0:
                return rng.randint(lo, hi)
        return rng.randint(es[-1][0], es[-1][1])
    return rng.getrandbits(w)


def reference(case, cons, n: int, seed: int, tries: int = 4000, order: bool = True,
              seconds: float = 20.0):
    """n models drawn per the SV meaning, or None if too sparse to reject.
    order=False ignores `solve ... before`: uniform over the solutions (dist
    variables still by their weights)."""
    from tests.formal.soundness.ir import ev
    rng = random.Random(seed)
    dist = getattr(case, "DIST", {})
    widths = {nm: w for nm, w, _ in case.VARS}
    before = [v for b, _ in getattr(case, "SOLVE_BEFORE", []) for v in b
              if v in widths] if order else []
    box = getattr(case, "BOX", {})
    when = getattr(case, "DIST_WHEN", {})
    out, dead = [], set()
    stop = time.monotonic() + seconds  # past it, too sparse to reject
    while len(out) < n:
        if time.monotonic() > stop:
            return None
        fixed = {v: _draw_var(rng, v, widths[v], dist, box) for v in before}
        key = tuple(sorted(fixed.items()))
        if key in dead:
            continue
        for _ in range(tries):
            m = dict(fixed)
            for nm, w, _ in case.VARS:
                if nm not in m and nm not in when:
                    m[nm] = _draw_var(rng, nm, w, dist, box)
            for nm, cond in when.items():       # a dist in one branch: drawn last
                if nm not in m:
                    m[nm] = _draw_var(rng, nm, widths[nm], dist if cond(m) else {}, box)
            if all(ev(c, m) for c in cons):
                out.append(m)
                break
        else:
            dead.add(key)
            if len(dead) > 4096 or (not before and not out):
                return None
    return out


def _hist(models, f):
    h = {}
    for m in models:
        k = f(m)
        h[k] = h.get(k, 0) + 1
    return h


def jsd(p: dict, q: dict) -> float:
    sp, sq = sum(p.values()), sum(q.values())
    out = 0.0
    for k in set(p) | set(q):
        a, b = p.get(k, 0) / sp, q.get(k, 0) / sq
        mid = (a + b) / 2
        if a:
            out += a / 2 * math.log2(a / mid)
        if b:
            out += b / 2 * math.log2(b / mid)
    return out


# ---- one arm (runs in its own process) -------------------------------------

def worker(names, n, seed):
    from dv_solve.ctx import SOLVE_OK, SolveCtx
    from tests.formal.soundness.ir import ev
    from tests.perf.svcorpus import case as C
    res = {}
    for name in names:
        case = C.load(name)
        _, cons = C.capture(case)
        ids = C.ids(case)
        t0 = time.perf_counter()
        prob = C.problem(case)
        try:
            ctx = SolveCtx(prob)
        except Exception as e:                  # noqa: BLE001 -- report, go on
            res[name] = {"error": f"{type(e).__name__}: {e}"}
            continue
        t_compile = time.perf_counter() - t0
        rng = random.Random(seed)
        ts, models, bad, bad_v, rc_other = [], [], 0, 0, 0
        with ctx:
            for _ in range(n):
                ctx.reset()
                s = rng.getrandbits(62) + 1
                t = time.perf_counter()
                rc = ctx.solve(seed=s)
                ts.append(time.perf_counter() - t)
                if rc != SOLVE_OK:
                    rc_other += 1
                    continue
                m = {nm: ctx.get_value(ids[nm]) & ((1 << w) - 1) for nm, w, _ in case.VARS}
                if not all(ev(c, m) for c in cons):
                    bad += 1
                if ctx.validate_model():
                    bad_v += 1
                models.append(m)
        res[name] = {"t_compile": t_compile, "t_solve": ts, "models": models,
                     "bad": bad, "bad_validate": bad_v, "rc_other": rc_other}
    return res


def run_arm(lib, names, n, seed):
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)
    env["DVS_SOLVER_PATH"] = str(lib)
    env["PYTHONPATH"] = f"{REPO / 'src'}:{REPO}"
    r = subprocess.run([sys.executable, "-m", "tests.perf.svcorpus.bench", "--worker",
                        "--n", str(n), "--seed", str(seed), *names],
                       env=env, cwd=REPO, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"arm {lib}: {r.stderr[-2000:]}")
    return json.loads(r.stdout.strip().splitlines()[-1])


# ---- driver ----------------------------------------------------------------

def _us(x):
    return f"{x * 1e6:.1f}"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--arm", action="append", default=[])
    ap.add_argument("--worker", action="store_true")
    ap.add_argument("cases", nargs="*")
    a = ap.parse_args(argv)
    sys.path[:0] = [str(REPO / "src"), str(REPO)]
    from tests.perf.svcorpus import case as C
    names = a.cases or C.names()
    if a.worker:
        print(json.dumps(worker(names, a.n, a.seed)))
        return 0
    arms = [x.split("=", 1) for x in a.arm] or [["now", str(REPO / "build" / "lib")]]
    out = {"n": a.n, "seed": a.seed, "arms": dict(arms), "cases": {}}
    raw = {arm: run_arm(lib, names, a.n, a.seed) for arm, lib in arms}
    for name in names:
        case = C.load(name)
        _, cons = C.capture(case)
        ks = keys(case)
        refs = {}
        for tag, order in (("sv", True), ("uni", False)):
            if tag == "uni" and not getattr(case, "SOLVE_BEFORE", []):
                continue
            r1 = reference(case, cons, a.n, a.seed + 1, order=order)
            refs[tag] = (r1, reference(case, cons, a.n, a.seed + 2, order=order)) if r1 else None
        row = {"about": case.ABOUT, "arms": {}}
        print(f"\n{name}: {case.ABOUT}")
        print(f"  {'arm':10s} {'median':>8s} {'mean':>8s} {'p90':>8s} {'compile':>8s} "
              f"{'bad':>4s} {'rc':>4s}  spread: worst key's excess JSD vs SV meaning"
              + (" | vs uniform (no solve-before)" if "uni" in refs else ""))
        for arm, _ in arms:
            r = raw[arm][name]
            if "error" in r:
                print(f"  {arm:10s} {r['error']}")
                row["arms"][arm] = r
                continue
            ts = sorted(r["t_solve"])
            sps = {}
            for tag, rr in refs.items():
                sp = {}
                if rr and r["models"]:
                    ref, ref2 = rr
                    for k, f in ks.items():
                        floor = jsd(_hist(ref, f), _hist(ref2[:len(r["models"])], f))
                        sp[k] = jsd(_hist(r["models"], f), _hist(ref, f)) - floor
                sps[tag] = sp
            summ = {"median": statistics.median(ts), "mean": statistics.fmean(ts),
                    "p90": ts[int(0.9 * (len(ts) - 1))], "compile": r["t_compile"],
                    "bad": r["bad"], "bad_validate": r["bad_validate"],
                    "rc_other": r["rc_other"], "spread": sps}
            row["arms"][arm] = summ

            def worst(sp):
                if not sp:
                    return "-"
                k = max(sp, key=sp.get)
                return f"{sp[k]:+.3f} ({k})"
            spread = " | ".join(worst(sps.get(t)) for t in refs) or "-"
            print(f"  {arm:10s} {_us(summ['median']):>8s} {_us(summ['mean']):>8s} "
                  f"{_us(summ['p90']):>8s} {summ['compile'] * 1e3:7.1f}m "
                  f"{r['bad'] + r['bad_validate']:4d} {r['rc_other']:4d}  {spread}")
        out["cases"][name] = row
    d = REPO / "perf-out" / "svcorpus"
    d.mkdir(parents=True, exist_ok=True)
    path = d / time.strftime("%Y%m%d-%H%M%S.json")
    path.write_text(json.dumps(out, indent=1))
    print(f"\nwrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
