"""Expected cost: how far is a shape class from the best case for its shape?

A model per domain (randomization, SMT-LIB2) predicts, from a class's
features in a stats file (its size, widths, operator kinds, motifs, how
constrained it is at the root), log2 of the **best-case** cost per solve of a
problem of that shape: the cheapest cost any arm we can run achieves on our
corpus (dv-solve's engines and settings, the reference solvers, enumeration).
A class's **delta** is its actual cost over that prediction.

The expectation is the best case, not our own typical cost: a model fitted
on our own performance learns our weaknesses as normal (fitted on the test
suite, it "expected" `countones`, `array` and `sum` constraints to cost 4x
more per doubling), and could only flag what is bad by our own standards.
docs/solve_stats_capture_plan.md, S5a.

- **Cost units** sum each engine's work, wasted work included, at that
  engine's measured nanoseconds per unit on the machine the model was fitted
  on. Work counts do not depend on the machine, so the check holds anywhere.
- **Features are about the problem only**, never the engine or route that
  answered it: a misrouted class must not be compared only with other
  misrouted ones.
- **The enumeration floor:** a class with few free bits at the root never
  costs more than trying each candidate.
- **Unexpected** = a delta of at least 4x, and beyond the best-case model's
  own cross-validated error. The threshold never comes from our own deltas:
  that would absorb a gap we have everywhere.
- **Anomalous** is a second, separate signal: a *self-model* fitted on our
  own cost over many more classes (the test suite) flags a class at least
  4x above what dv-solve usually spends on its shape, beyond the self-model's
  own spread. It finds regressions and one-off anomalies; it can never find
  a gap we have everywhere, which is what `unexpected` is for.

The model ships with dv-solve as perf_model.json. Predicting needs no
third-party package; fitting needs numpy.

    python -m tests.perf.perfmodel_corpus --build build -o train.json   # arms, frontier
    python -m dv_solve.perfmodel fit train.json -o src/dv_solve/perf_model.json
    python -m dv_solve.perfmodel check stats.*.json
    python -m dv_solve.perfmodel bias train.json      # where we lose, by motif
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

MODEL_SCHEMA = "dv-solve-perf-model/2"
TRAIN_SCHEMA = "dv-solve-perf-train/1"
FEATURE_VERSION = 4
ENGINES = ("cdcl", "bitblast", "cube")
# Randomization (what users' testbenches run) and SMT-LIB2 problems (mostly
# formal) are modelled apart: proof hardness is not a function of structure.
RAND_PATHS = ("api", "handle", "dpi", "verilator", "bvsat")
DELTA_MIN_LOG2 = 2.0            # unexpected: at least 4x the best case
MIN_CLASSES = 12                # fewer training classes than this: no model
FLOOR_MAX_BITS = 24             # the enumeration floor applies up to 2^24 candidates
_MODEL_PATH = Path(__file__).with_name("perf_model.json")

_SCALARS = ["vars", "constraints", "softs", "alldiffs", "dists", "ordered", "components",
            "max_component", "max_depth", "guard_depth", "nodes"]
_WIDTHS = ["1", "8", "16", "32", "64", "wide"]
_OPS = ["bit", "arith", "mul", "cmp", "eq", "bool", "other"]
_MOTIFS = ["var-cmp-const", "var-eq-const", "var-cmp-var", "var-eq-var", "range", "set",
           "ranges", "mod-eq", "mask-eq", "shift-eq", "linear", "nonlinear", "bitwise",
           "countones", "sum", "array", "guarded", "disjunction", "bool-var", "ite-value",
           "constant", "other"]
FEATURE_NAMES = ([f"f.{k}" for k in _SCALARS] + [f"w.{k}" for k in _WIDTHS]
                 + [f"o.{k}" for k in _OPS] + [f"m.{k}" for k in _MOTIFS]
                 + ["root.bits", "root.free_bits", "root.fixed", "root.known",
                    "unsat_share", "uniform_share"])


def _l(x) -> float:
    return math.log2(1.0 + max(float(x), 0.0))


def features(c: dict) -> Optional[List[float]]:
    """The feature vector of class record `c`, or None if it has no features.
    Only what the problem is and what was asked of it: never the engine or
    route that answered it."""
    f = c.get("features")
    if not f:
        return None
    v = [_l(f.get(k, 0)) for k in _SCALARS]
    v += [_l(f.get("widths", {}).get(k, 0)) for k in _WIDTHS]
    v += [_l(f.get("ops", {}).get(k, 0)) for k in _OPS]
    m = c.get("motifs", {})
    v += [_l(m.get(k, 0)) for k in _MOTIFS]
    r = c.get("root")
    if r and r.get("count"):
        n = r["count"]
        v += [_l(r["bits"] / n), _l(r["free_bits"] / n),
              r["fixed"] / max(r["vars"], 1), 1.0]
    else:
        v += [0.0, 0.0, 0.0, 0.0]
    n = max(c.get("solves", 0), 1)
    # A proof costs more than a model. (Not the unknown share: that would
    # explain away the searches that gave up.)
    v.append(c.get("results", {}).get("unsat", 0) / n)
    # Uniform sampling is asked for, and costs more by design.
    v.append(c.get("uniform", 0) / n)
    return v


def domain(c: dict) -> str:
    """"rand" if most of class `c`'s solves came through a randomization path,
    else "smt2"."""
    paths = c.get("paths", {})
    n_rand = sum(v.get("solves", 0) for k, v in paths.items() if k in RAND_PATHS)
    return "rand" if 2 * n_rand >= sum(v.get("solves", 0) for v in paths.values()) else "smt2"


def cost(c: dict, ns_per_work: Dict[str, float]) -> Optional[float]:
    """Class `c`'s cost per solve, in ns at the fitted machine's rates: every
    engine's work, wasted work included. None if it did no work or has no
    solves."""
    n = c.get("solves", 0)
    w = c.get("work", {})
    total = sum(ns_per_work.get(e, 0.0) * w.get(e, 0) for e in ENGINES)
    return total / n if n and total > 0 else None


def enum_floor(c: dict, ns_per_work: Dict[str, float]) -> Optional[float]:
    """The cost of trying every candidate left at the root, checking each
    constraint once per candidate, at CDCL's rate; None when the root is not
    known or leaves more than 2^FLOOR_MAX_BITS candidates."""
    r = c.get("root")
    if not r or not r.get("count") or "cdcl" not in ns_per_work:
        return None
    bits = r["free_bits"] / r["count"]
    if bits > FLOOR_MAX_BITS:
        return None
    cons = max(c.get("features", {}).get("constraints", 0), 1)
    return (2.0 ** bits) * cons * ns_per_work["cdcl"]


# ---------------------------------------------------------------------------
# Prediction (no third-party packages)
# ---------------------------------------------------------------------------

_LOADED: Dict[str, dict] = {}


def load_model(path: Optional[str] = None) -> Optional[dict]:
    """The shipped model (or the one at `path`); None if there is none."""
    p = Path(path) if path else _MODEL_PATH
    key = str(p)
    if key not in _LOADED:
        if not p.is_file():
            return None
        m = json.loads(p.read_text(encoding="utf-8"))
        if m.get("schema") != MODEL_SCHEMA or m.get("feature_version") != FEATURE_VERSION:
            return None
        _LOADED[key] = m
    return _LOADED[key]


def _linear(dm: dict, x: List[float]) -> float:
    return dm["intercept"] + sum(
        w * (xi - mu) / sd for w, xi, mu, sd in zip(dm["coef"], x, dm["mean"], dm["std"]))


def _self_delta(model: dict, c: dict, x: List[float], actual: float) -> Optional[float]:
    sm = model.get("self", {}).get(domain(c))
    return None if sm is None else actual - _linear(sm, x)


def predict(model: dict, c: dict) -> Optional[dict]:
    """For class `c`: {domain, actual, expected, delta, floored} in log2 ns
    per solve (expected: the best case for its shape, floored by
    enumeration), or None if it has no cost or features, or its domain has no
    model."""
    x = features(c)
    k = model["ns_per_work"]
    y = cost(c, k)
    d = domain(c)
    dm = model["domains"].get(d)
    if x is None or y is None or dm is None:
        return None
    yhat = _linear(dm, x)
    fl = enum_floor(c, k)
    floored = fl is not None and math.log2(fl) < yhat
    if floored:
        yhat = math.log2(fl)
    actual = math.log2(y)
    return {"domain": d, "actual": actual, "expected": yhat, "delta": actual - yhat,
            "floored": floored, "self_delta": _self_delta(model, c, x, actual)}


def threshold(model: dict, dom: str) -> float:
    """The delta (log2) at or above which a class of `dom` is unexpected: 4x,
    or the best-case model's own error if that is larger."""
    return max(DELTA_MIN_LOG2, model["domains"][dom]["error_q"]["p95"])


def check(data: dict, model: Optional[dict] = None) -> List[dict]:
    """Every class of stats file `data` with a prediction, most recoverable
    time first. `ratio` is its delta (actual over best-case cost);
    `unexpected` marks the classes over the threshold; `recoverable_ns` is
    the solver time it would save at the best case."""
    model = model or load_model()
    if model is None:
        return []
    out = []
    for c in data["snapshot"]["classes"]:
        p = predict(model, c)
        if p is None:
            continue
        p["class"] = c["class"]
        p["solves"] = c["solves"]
        p["ratio"] = 2.0 ** p["delta"]
        p["unexpected"] = p["delta"] >= threshold(model, p["domain"])
        sm = model.get("self", {}).get(p["domain"])
        p["anomalous"] = bool(sm and p["self_delta"] is not None and p["self_delta"] >= max(
            DELTA_MIN_LOG2, sm["error_q"]["p95"]))
        p["recoverable_ns"] = int(c["wall_ns"] * max(0.0, 1.0 - 1.0 / p["ratio"]))
        out.append(p)
    out.sort(key=lambda p: (not p["unexpected"], not p["anomalous"], -p["recoverable_ns"]))
    return out


# ---------------------------------------------------------------------------
# Fitting (numpy)
# ---------------------------------------------------------------------------

_LAMBDAS = [0.1, 0.3, 1.0, 3.0, 10.0, 30.0, 100.0]


def _ridge(np, X, y, lam):
    mu = X.mean(axis=0)
    sd = X.std(axis=0)
    sd[sd == 0] = 1.0
    Z = (X - mu) / sd
    b0 = y.mean()
    A = Z.T @ Z + lam * np.eye(Z.shape[1])
    w = np.linalg.solve(A, Z.T @ (y - b0))
    return mu, sd, w, b0


def _cv_residuals(np, X, y, lam, folds, seed):
    idx = list(range(len(y)))
    random.Random(seed).shuffle(idx)
    res = np.zeros(len(y))
    for k in range(folds):
        test = idx[k::folds]
        held = set(test)
        train = [i for i in idx if i not in held]
        mu, sd, w, b0 = _ridge(np, X[train], y[train], lam)
        res[test] = y[test] - (b0 + ((X[test] - mu) / sd) @ w)
    return res


def _best(np, X, y, folds, seed):
    """(lambda, cross-validated residuals) with the least squared error."""
    best = None
    for lam in _LAMBDAS:
        res = _cv_residuals(np, X, y, lam, min(folds, len(y)), seed)
        mse = float((res ** 2).mean())
        if best is None or mse < best[0]:
            best = (mse, lam, res)
    return best[1], best[2]


def ns_per_work(classes: List[dict]) -> Dict[str, float]:
    """Each engine's ns per work unit on this machine: the median over the
    classes in which only that engine ran."""
    out = {}
    for e in ENGINES:
        r = sorted(c["wall_ns"] / c["work"][e] for c in classes
                   if c.get("work", {}).get(e) and c.get("engine_ran", {}).get(e)
                   and all(not c.get("engine_ran", {}).get(o) for o in ENGINES if o != e))
        if r:
            out[e] = r[len(r) // 2]
    return out


def fit(train: dict, folds: int = 5, seed: int = 1, self_model: bool = False) -> dict:
    """A model fitted on training file `train` (tests/perf/perfmodel_corpus):
    its stats file and, per class, the best arm's time over ours (`frontier`).
    The target is a class's best-case cost, our cost times that ratio; with
    `self_model` it is our own cost (for the bias report only: a self-model
    learns our weaknesses as normal)."""
    import numpy as np
    classes = train["stats"]["snapshot"]["classes"]
    frontier = train.get("frontier", {})
    k = train.get("ns_per_work") or ns_per_work(classes)
    rows: Dict[str, List[Tuple[List[float], float, str]]] = {}
    for c in classes:
        x, y = features(c), cost(c, k)
        if x is None or y is None:
            continue
        if not self_model:
            if c["class"] not in frontier:
                continue
            y *= min(1.0, frontier[c["class"]])
        rows.setdefault(domain(c), []).append((x, math.log2(y), c["class"]))
    model = {"schema": MODEL_SCHEMA, "feature_version": FEATURE_VERSION,
             "features": FEATURE_NAMES, "target": "self" if self_model else "best-case",
             "dv_solve": train["stats"].get("dv_solve"), "commit": train["stats"].get("commit"),
             "ns_per_work": k, "domains": {}}
    for d, rs in sorted(rows.items()):
        if len(rs) < MIN_CLASSES:
            continue
        X = np.array([r[0] for r in rs])
        y = np.array([r[1] for r in rs])
        lam, res = _best(np, X, y, folds, seed)
        mu, sd, w, b0 = _ridge(np, X, y, lam)
        ss = float(((y - y.mean()) ** 2).sum()) or 1.0
        model["domains"][d] = {
            "classes": len(y), "lambda": lam, "intercept": float(b0),
            "mean": mu.tolist(), "std": sd.tolist(), "coef": w.tolist(),
            "cv_r2": 1.0 - float((res ** 2).sum()) / ss,
            "cv_rmse": math.sqrt(float((res ** 2).mean())),
            # The model's own error at predicting the target (not our delta).
            "error_q": {f"p{p}": float(np.percentile(res, p)) for p in (50, 90, 95, 99)},
        }
    return model


def fit_self(stats_data: dict, folds: int = 5, seed: int = 1,
             k: Optional[Dict[str, float]] = None) -> dict:
    """The self-model: our own cost per domain, fitted robustly in one round
    (the classes far above their cross-validated prediction are set aside and
    the rest refitted), so that its spread is that of ordinary classes."""
    import numpy as np
    classes = stats_data["snapshot"]["classes"]
    k = k or ns_per_work(classes)
    rows: Dict[str, List[Tuple[List[float], float]]] = {}
    for c in classes:
        x, y = features(c), cost(c, k)
        if x is not None and y is not None:
            rows.setdefault(domain(c), []).append((x, math.log2(y)))
    out = {}
    for d, rs in sorted(rows.items()):
        if len(rs) < MIN_CLASSES:
            continue
        X = np.array([r[0] for r in rs])
        y = np.array([r[1] for r in rs])
        _, res0 = _best(np, X, y, folds, seed)
        keep = res0 < max(DELTA_MIN_LOG2, float(np.percentile(res0, 95)))
        lam, res = _best(np, X[keep], y[keep], folds, seed)
        mu, sd, w, b0 = _ridge(np, X[keep], y[keep], lam)
        yk = y[keep]
        ss = float(((yk - yk.mean()) ** 2).sum()) or 1.0
        out[d] = {"classes": len(y), "fitted_on": int(keep.sum()), "lambda": lam,
                  "intercept": float(b0), "mean": mu.tolist(), "std": sd.tolist(),
                  "coef": w.tolist(), "cv_r2": 1.0 - float((res ** 2).sum()) / ss,
                  "cv_rmse": math.sqrt(float((res ** 2).mean())),
                  "error_q": {f"p{p}": float(np.percentile(res, p)) for p in (50, 90, 95, 99)}}
    return out


def bias(train: dict) -> List[dict]:
    """Where we lose to the best arm, by motif: per domain and motif, the
    training classes holding it and our cost over the best case's, as a
    geometric mean, largest first. Measured, not modelled: the ranked list of
    weaknesses a self-model would take as normal."""
    frontier = train.get("frontier", {})
    acc: Dict[Tuple[str, str], List[float]] = {}
    for c in train["stats"]["snapshot"]["classes"]:
        ratio = frontier.get(c["class"])
        if not ratio or c.get("features") is None:
            continue
        gap = -math.log2(min(1.0, ratio))       # log2 of ours over the best
        for m, n in c.get("motifs", {}).items():
            if n:
                acc.setdefault((domain(c), m), []).append(gap)
    out = [{"domain": d, "motif": m, "classes": len(g), "gap": 2.0 ** (sum(g) / len(g))}
           for (d, m), g in acc.items()]
    return sorted(out, key=lambda r: -r["gap"])


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def _fmt_check(rows: List[dict], top: int) -> str:
    flagged = [r for r in rows if r["unexpected"] or r["anomalous"]]
    lines = [f"{len(rows)} classes predicted: {sum(r['unexpected'] for r in rows)} far from the "
             f"best case for their shape (unexpected), {sum(r['anomalous'] for r in rows)} slow "
             f"for dv-solve's own habits (anomalous)"]
    for r in flagged[:top]:
        tags = "+".join(t for t in ("unexpected", "anomalous") if r[t])
        lines.append(f"  {r['class'][:12]}  {r['domain']:5s} {r['ratio']:8.1f}x the best case"
                     f"{' (enumeration floor)' if r['floored'] else ''}  [{tags}]  "
                     f"{r['solves']} solves, {r['recoverable_ns'] / 1e6:.1f} ms recoverable")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m dv_solve.perfmodel", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fit", help="fit the model on a training file (needs numpy)")
    p.add_argument("train")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--self-model", action="store_true",
                   help="fit our own cost, not the best case (for the bias report)")
    p.add_argument("--self-from", nargs="*", default=[],
                   help="stats files (the test suite) for the self-model, with the "
                        "training file's own classes")
    p = sub.add_parser("check", help="list classes far from the best case")
    p.add_argument("files", nargs="+")
    p.add_argument("--model", help="a model file other than the shipped one")
    p.add_argument("--top", type=int, default=20)
    p = sub.add_parser("bias", help="where we lose to the best arm, by motif")
    p.add_argument("train")
    p.add_argument("--top", type=int, default=12)
    a = ap.parse_args(argv)
    from dv_solve import stats
    if a.cmd == "fit":
        tr = json.loads(Path(a.train).read_text(encoding="utf-8"))
        m = fit(tr, self_model=a.self_model)
        if not a.self_model:
            data = stats.merge([tr["stats"]] + [stats.load(f) for f in a.self_from])
            m["self"] = fit_self(data, k=m["ns_per_work"])
            for d, sm in m["self"].items():
                print(f"self-model {d}: {sm['classes']} classes ({sm['fitted_on']} fitted), "
                      f"cv r2 {sm['cv_r2']:.2f}, cv rmse {sm['cv_rmse']:.2f} log2")
        Path(a.out).write_text(json.dumps(m, indent=1) + "\n", encoding="utf-8")
        print("ns per work unit: " + ", ".join(f"{e} {v:.2f}" for e, v in m["ns_per_work"].items()))
        for d, dm in m["domains"].items():
            print(f"{d}: {dm['classes']} classes, lambda {dm['lambda']}, "
                  f"cv r2 {dm['cv_r2']:.2f}, cv rmse {dm['cv_rmse']:.2f} log2, "
                  f"p95 error {dm['error_q']['p95']:.2f}")
        return 0
    if a.cmd == "bias":
        tr = json.loads(Path(a.train).read_text(encoding="utf-8"))
        print("where we lose to the best arm, by motif (our cost over the best case, "
              "geometric mean over the training classes holding it):")
        for r in bias(tr)[:a.top]:
            print(f"  {r['domain']:5s} {r['motif']:16s} {r['gap']:7.1f}x  ({r['classes']} classes)")
        return 0
    model = load_model(a.model)
    if model is None:
        print("no model", file=sys.stderr)
        return 1
    data = stats.merge(stats.load(f) for f in a.files)
    print(_fmt_check(check(data, model), a.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
