#!/usr/bin/env python3
"""Uniformity harness: are dv-solve's randomize() answers LRM-uniform?

IEEE 1800 asks randomize() to pick uniformly over the solutions of the
constraints (dist aside, which Verilator resolves before the solver sees the
problem). This harness measures how close each dv-solve sampling path comes,
against an exact reference:

  1. A problem is the SMT-LIB of one randomize() call, as Verilator sends it
     (reset, seed, declarations, asserts, check-sat, get-value).
  2. z3 enumerates every solution over the get-value variables (ground truth
     never comes from dv-solve). Problems with more than --cap solutions are
     reported as not enumerable and do not gate.
  3. Each arm draws N models, one randomize() per seed, in the session shape
     the simulator uses. The arms are the paths a user can reach:
       default    dv-solve-smt2, default mode (seeded check-sat -> bitblast);
                  the vlt-dvs-solver simbench path
       cdcl       dv-solve-smt2 --engine=cdcl
       verilator  dv-solve-smt2 --mode=verilator (CDCL sampling, fingerprint cache)
       api-cache  bench_vlt_replay --passes api-cache (compiled-context reuse,
                  bitblast rediversify), the dvs-standalone simbench path
       uniform    a true RNG over the enumerated set: the noise floor
  4. Tests, Bonferroni-corrected at --alpha over the tests of one problem:
       joint      chi-square over the solutions (when N >= 5 per solution)
       marginals  chi-square of each variable's values against its exact
                  marginal, binned so every bin expects >= 5
       distinct   the number of distinct solutions hit, against its exact
                  law under uniform draws (one-sided: too few is the failure
                  of a sampler that stays near a few base models)
     plus descriptive numbers: coverage, total-variation distance, the
     max/min observed:expected ratio over solutions.
     A model outside the enumerated set is UNSOUND, whatever the statistics.

The harness only drives the two command-line tools, so it runs unchanged
against any build that has them (the 0.x vs 1.0 comparison):
    --dv-solve-smt2 <build>/dv-solve-smt2 --replay <build>/bench_vlt_replay

Usage:
    python3 tests/uniformity/uniformity.py [--source families|corpus|PATH...]
        [--arms default,verilator,api-cache,uniform] [--n N] [--json OUT]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parents[1]
sys.path.insert(0, str(_HERE))

ARMS_ALL = ("default", "cdcl", "verilator", "api-cache", "uniform")
_VAL = re.compile(r"\(\s*(\|[^|]*\||[^\s()]+|\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\))\s+(#b[01]+|#x[0-9a-fA-F]+|true|false)\s*\)")


# --------------------------------------------------------------------------
# Problems
# --------------------------------------------------------------------------

@dataclass
class Problem:
    name: str
    body: list[str]          # set-logic, define-fun, declare-fun, push, assert
    names: list[str]         # the get-value variables
    note: str = ""
    source: str = ""

    def digest(self) -> str:
        return hashlib.sha256("\n".join(self.body).encode()).hexdigest()[:12]


def _lit(v: str) -> int:
    if v == "true":
        return 1
    if v == "false":
        return 0
    return int(v[2:], 2) if v.startswith("#b") else int(v[2:], 16)


# --------------------------------------------------------------------------
# Ground truth (z3)
# --------------------------------------------------------------------------

def enumerate_solutions(p: Problem, cap: int) -> list[tuple] | None:
    """Every solution projected on p.names, or None if there are more than cap."""
    import z3
    text = "\n".join(l for l in p.body
                     if not l.lstrip().startswith(("(set-logic", "(push", "(pop",
                                                  "(set-option", "(check-sat")))
    s = z3.Solver()
    s.add(z3.parse_smt2_string(text))
    # Recover the declared constants by name from the parsed assertions.
    seen = {}
    def walk(e):
        stack = [e]
        while stack:
            x = stack.pop()
            if z3.is_const(x) and x.decl().kind() == z3.Z3_OP_UNINTERPRETED:
                seen[x.decl().name()] = x
            stack.extend(x.children())
    for a in s.assertions():
        walk(a)
    vars_ = []
    for n in p.names:
        if n.startswith("("):
            # A term such as (select a #x0): parse it in the problem's scope.
            t = z3.parse_smt2_string(text + f"\n(assert (= {n} {n}))")[-1]
            vars_.append(t.arg(0))
            continue
        key = n[1:-1] if n.startswith("|") else n
        if key not in seen:
            # Declared but unconstrained: every value is a solution.
            w = _declared_width(p.body, n)
            seen[key] = z3.BitVec(key, w) if w else z3.Bool(key)
        vars_.append(seen[key])
    s.set("timeout", 20000)      # per check, ms: a hard problem is just not enumerated
    out = []
    while True:
        r = s.check()
        if r == z3.unknown:
            return None
        if r != z3.sat:
            break
        m = s.model()
        vals = []
        for v in vars_:
            x = m.eval(v, model_completion=True)
            vals.append(1 if z3.is_true(x) else 0 if z3.is_false(x) else x.as_long())
        out.append(tuple(vals))
        if len(out) > cap:
            return None
        s.add(z3.Or([v != m.eval(v, model_completion=True) for v in vars_]))
    return out


def _declared_width(body: list[str], name: str) -> int:
    m = re.search(r"\(declare-fun\s+" + re.escape(name) + r"\s+\(\)\s+\(_ BitVec (\d+)\)",
                  "\n".join(body))
    return int(m.group(1)) if m else 0


# --------------------------------------------------------------------------
# Arms
# --------------------------------------------------------------------------

def seeds(n: int, base: int) -> list[int]:
    """Reproducible 31-bit seeds, spread like Verilator's (not 1, 2, 3...)."""
    out, x = [], base & 0xFFFFFFFFFFFFFFFF
    for _ in range(n):
        x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = x
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        out.append(((z ^ (z >> 31)) & 0x7FFFFFFF) or 1)
    return out


def session(p: Problem, seed_list: list[int]) -> str:
    """One randomize() per seed, each starting with (reset), as Verilator does."""
    gv = "(get-value (" + " ".join(p.names) + "))"
    out = []
    for s in seed_list:
        out += ["(reset)", "(set-option :produce-models true)",
                f"(set-option :random-seed {s})"]
        out += p.body
        out += ["(check-sat)", gv]
    out.append("(exit)")
    return "\n".join(out) + "\n"


def _parse_models(text: str, names: list[str]) -> list[tuple | None]:
    """get-value answers in order; a call answered unsat/unknown gives None."""
    res, cur = [], {}
    for line in text.splitlines():
        t = line.strip()
        if t in ("unsat", "unknown"):
            res.append(None)
            continue
        for n, v in _VAL.findall(t):
            cur[" ".join(n.split())] = v
        if len(cur) == len(names) and all(n in cur for n in names):
            res.append(tuple(_lit(cur[n]) for n in names))
            cur = {}
    return res


@dataclass
class Tools:
    smt2: Path
    replay: Path
    env: dict = field(default_factory=dict)


def _run(cmd, inp=None, timeout=1800, env=None):
    e = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    e.update(env or {})
    return subprocess.run(cmd, input=inp, capture_output=True, text=True,
                          timeout=timeout, env=e)


def draw(arm: str, p: Problem, n: int, base: int, tools: Tools,
         space: list[tuple] | None) -> tuple[list[tuple | None], float]:
    sl = seeds(n, base)
    t0 = time.perf_counter()
    if arm == "uniform":
        if not space:
            return [], 0.0
        r = random.Random(base)
        return [space[r.randrange(len(space))] for _ in sl], 0.0
    if arm in ("default", "cdcl", "verilator"):
        flags = {"default": [], "cdcl": ["--engine=cdcl"],
                 "verilator": ["--mode=verilator"]}[arm]
        r = _run([str(tools.smt2), "--interactive", *flags], session(p, sl), env=tools.env)
        return _parse_models(r.stdout, p.names), time.perf_counter() - t0
    if arm == "api-cache":
        with tempfile.TemporaryDirectory() as d:
            cmds, dump = Path(d) / "s.smt2", Path(d) / "v.txt"
            cmds.write_text(session(p, sl))
            r = _run([str(tools.replay), str(cmds), "--passes", "api-cache",
                      "--reps", "1", "--dump-values", str(dump)], env=tools.env)
            el = time.perf_counter() - t0
            if r.returncode or not dump.exists():
                raise RuntimeError(f"bench_vlt_replay failed: {r.stderr.strip()[-300:]}")
            return _parse_dump(dump.read_text(), p.names, n), el
    raise ValueError(arm)


def _parse_dump(text: str, names: list[str], n: int) -> list[tuple | None]:
    """Last model per call (a call may hold more than one check-sat)."""
    by_call: dict[int, tuple] = {}
    for line in text.splitlines():
        if not line.startswith("V "):
            continue
        parts = line.split()
        kv = dict(x.split("=", 1) for x in parts[3:])
        if all(k in kv for k in names):
            by_call[int(parts[1])] = tuple(_lit(kv[k]) for k in names)
    return [by_call.get(c) for c in range(1, n + 1)]


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------

def _chi2_sf(x: float, dof: int) -> float:
    from scipy.stats import chi2
    return float(chi2.sf(x, dof)) if dof > 0 else 1.0


def _binned_chi2(obs: Counter, exp: dict, n: int) -> tuple[float, int]:
    """Chi-square of obs against expected probabilities exp, merging adjacent
    values (in value order) until every bin expects >= 5. (p, bins)."""
    bins, cur_o, cur_e = [], 0, 0.0
    for v in sorted(exp):
        cur_o += obs.get(v, 0)
        cur_e += exp[v] * n
        if cur_e >= 5:
            bins.append((cur_o, cur_e))
            cur_o, cur_e = 0, 0.0
    if cur_e > 0:
        if bins:
            o, e = bins.pop()
            bins.append((o + cur_o, e + cur_e))
        else:
            bins.append((cur_o, cur_e))
    if len(bins) < 2:
        return 1.0, len(bins)
    x = sum((o - e) ** 2 / e for o, e in bins)
    return _chi2_sf(x, len(bins) - 1), len(bins)


def _distinct_p(d: int, k: int, n: int) -> float:
    """P(at most d distinct values) for n uniform draws from k, by the normal
    approximation with the exact mean and variance."""
    from scipy.stats import norm
    q1, q2 = (1 - 1 / k) ** n, (1 - 2 / k) ** n
    mean = k * (1 - q1)
    var = k * (k - 1) * q2 + k * q1 - k * k * q1 * q1
    if var <= 1e-12:
        return 1.0 if d >= mean - 0.5 else 0.0
    return float(norm.cdf((d + 0.5 - mean) / math.sqrt(var)))


def score(samples: list[tuple | None], space: list[tuple] | None,
          names: list[str], alpha: float) -> dict:
    got = [s for s in samples if s is not None]
    res = {"n": len(samples), "answered": len(got),
           "distinct": len(set(got))}
    if space is None:
        res["verdict"] = "NOENUM"
        return res
    K, N = len(space), len(got)
    idx = set(space)
    off = sum(1 for s in got if s not in idx)
    cnt = Counter(s for s in got if s in idx)
    res.update(solutions=K, offspace=off, coverage=len(cnt) / K if K else 0.0)
    if off or len(got) < len(samples):
        res["verdict"] = "UNSOUND" if off else "UNANSWERED"
        return res
    if K <= 1 or N == 0:
        res["verdict"] = "PASS"
        return res
    e = N / K
    res["tv"] = 0.5 * sum(abs(cnt.get(s, 0) / N - 1 / K) for s in space)
    res["max_ratio"] = max(cnt.values()) / e
    res["min_ratio"] = min(cnt.get(s, 0) for s in space) / e
    ntests = 2 + len(names)
    a = alpha / ntests
    fails = []
    if e >= 5:
        x = sum((cnt.get(s, 0) - e) ** 2 / e for s in space)
        res["joint_p"] = _chi2_sf(x, K - 1)
        if res["joint_p"] < a:
            fails.append("joint")
    else:
        res["joint_p"] = None      # too few samples per solution to test
    # Distinct solutions hit, against its exact law under uniform draws: a
    # sampler that stays near a few base models hits too few. One-sided.
    res["distinct_p"] = _distinct_p(len(cnt), K, N)
    if res["distinct_p"] < a:
        fails.append("distinct")
    mp = {}
    for i, nm in enumerate(names):
        exact = Counter(s[i] for s in space)
        obs = Counter(s[i] for s in got)
        p, b = _binned_chi2(obs, {v: c / K for v, c in exact.items()}, N)
        mp[nm] = {"p": p, "bins": b, "values": len(exact)}
        if p < a:
            fails.append(nm)
    res["marginals"] = mp
    res["failed_tests"] = fails
    res["verdict"] = "FAIL" if fails else "PASS"
    return res


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def build_info(tools: Tools) -> dict:
    r = _run([str(tools.smt2), "--build-info"])
    info = {"smt2": str(tools.smt2), "build_info": r.stdout.strip()}
    lib = re.search(r"^library\s+(\S+)", r.stdout, re.M)
    if lib and Path(lib.group(1)).exists():
        info["library_sha256"] = hashlib.sha256(Path(lib.group(1)).read_bytes()).hexdigest()
    v = _run([str(tools.smt2), "--version"])
    info["version"] = v.stdout.strip()
    return info


def load_problems(sources: list[str]) -> list[Problem]:
    import families
    import corpus
    out = []
    for s in sources:
        if s == "families":
            out += families.all_problems()
        elif s == "corpus":
            out += corpus.load(_HERE / "corpus")
        else:
            out += corpus.load(Path(s))
    return out


def run_one(p, arms, tools, n, n_max, cap, alpha, base, n_noenum=200,
            budget=300.0) -> tuple[str, list[dict]]:
    t0 = time.perf_counter()
    try:
        space = enumerate_solutions(p, cap)
    except Exception as ex:      # z3 cannot parse it: still sample, do not gate
        space = None
    t_enum = time.perf_counter() - t0
    K = len(space) if space is not None else None
    # Nothing is tested without an enumeration: a few draws give distinct counts.
    nn = n_noenum if K is None else min(max(n, 5 * K), n_max)
    lines = [f"== {p.name}: {K if K is not None else '>' + str(cap)} solutions, "
             f"N={nn} (enum {t_enum:.1f}s)"]
    rows = []
    for arm in arms:
        try:
            na = nn
            if arm != "uniform" and nn > 200:
                # A slow arm (forced CDCL on a wide problem: ~0.5 s/call) gets
                # fewer draws, so one problem cannot hold the sweep for hours.
                _, el = draw(arm, p, 20, base, tools, space)
                na = max(200, min(nn, int(budget / max(el / 20, 1e-6))))
            samples, el = draw(arm, p, na, base, tools, space)
            r = score(samples, space, p.names, alpha)
            if na < nn:
                r["reduced_from"] = nn
        except Exception as ex:   # one bad problem must not stop the sweep
            r, el = {"verdict": "ERROR", "error": str(ex)[:300]}, 0.0
        r.update(problem=p.name, digest=p.digest(), arm=arm, seconds=round(el, 3),
                 source=p.source, note=p.note)
        rows.append(r)
        lines.append("   " + fmt_row(r))
    return "\n".join(lines), rows


def run(problems, arms, tools, n, n_max, cap, alpha, base, jobs=1, n_noenum=200,
        budget=300.0, log=print) -> list[dict]:
    args = (arms, tools, n, n_max, cap, alpha, base, n_noenum, budget)
    out: dict[int, list[dict]] = {}
    if jobs <= 1:
        for i, p in enumerate(problems):
            text, out[i] = run_one(p, *args)
            log(text)
    else:
        from concurrent.futures import ProcessPoolExecutor, as_completed
        with ProcessPoolExecutor(max_workers=jobs) as ex:
            futs = {ex.submit(run_one, p, *args): i for i, p in enumerate(problems)}
            for f in as_completed(futs):
                text, out[futs[f]] = f.result()
                log(text, flush=True)
    return [r for i in sorted(out) for r in out[i]]


def fmt_row(r: dict) -> str:
    def f(k, spec):
        v = r.get(k)
        return format(v, spec) if isinstance(v, (int, float)) else "-"
    mfail = ",".join(r.get("failed_tests", [])) or ""
    if r.get("reduced_from"):
        mfail += f" (N reduced from {r['reduced_from']})"
    return (f"{r['arm']:<10} {r['verdict']:<10} cov {f('coverage', '.3f')}  "
            f"tv {f('tv', '.3f')}  max {f('max_ratio', '5.2f')}  "
            f"min {f('min_ratio', '4.2f')}  joint_p {f('joint_p', '.2g')}  {mfail}")


def summary_md(rows: list[dict], info: dict) -> str:
    arms = list(dict.fromkeys(r["arm"] for r in rows))
    probs = list(dict.fromkeys(r["problem"] for r in rows))
    by = {(r["problem"], r["arm"]): r for r in rows}
    out = [f"# Uniformity report", "", f"- solver: `{info.get('version', '')}`",
           f"- library sha256: `{info.get('library_sha256', '?')[:16]}`", "",
           "| problem | solutions | " + " | ".join(arms) + " |",
           "|---|---:|" + "---|" * len(arms)]
    for p in probs:
        k = next((by[(p, a)].get("solutions") for a in arms if (p, a) in by), None)
        cells = []
        for a in arms:
            r = by.get((p, a), {})
            v = r.get("verdict", "")
            tv = r.get("tv")
            cells.append(f"{v} {tv:.2f}" if isinstance(tv, float) else v)
        out.append(f"| {p} | {k if k is not None else '-'} | " + " | ".join(cells) + " |")
    out += ["", "Cell: verdict and total-variation distance from uniform "
            "(the `uniform` arm is the noise floor at that N).", ""]
    for a in arms:
        rs = [by[(p, a)] for p in probs if (p, a) in by]
        c = Counter(r["verdict"] for r in rs)
        out.append(f"- {a}: " + ", ".join(f"{k} {v}" for k, v in sorted(c.items())))
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--source", action="append",
                    help="families, corpus, or a corpus file/dir (repeatable; "
                         "default families + corpus)")
    ap.add_argument("--only", help="comma-separated problem-name substrings")
    ap.add_argument("--arms", default="default,verilator,api-cache,uniform")
    ap.add_argument("--build", default=str(_REPO / "build"),
                    help="build dir holding dv-solve-smt2 and bench_vlt_replay")
    ap.add_argument("--dv-solve-smt2")
    ap.add_argument("--replay")
    ap.add_argument("--n", type=int, default=2000, help="minimum draws per arm")
    ap.add_argument("--n-max", type=int, default=20000,
                    help="draws per arm are max(n, 5 x solutions) up to this")
    ap.add_argument("--cap", type=int, default=4096,
                    help="largest solution set to enumerate")
    ap.add_argument("--alpha", type=float, default=1e-3,
                    help="family-wise significance per problem and arm")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=1, help="problems in parallel")
    ap.add_argument("--n-noenum", type=int, default=200,
                    help="draws per arm for a problem that is not enumerated")
    ap.add_argument("--budget", type=float, default=300.0,
                    help="seconds per arm and problem; a slower arm draws fewer (>= 200)")
    ap.add_argument("--json", help="write rows (JSON lines) here")
    ap.add_argument("--md", help="write a Markdown summary here")
    a = ap.parse_args(argv)

    b = Path(a.build)
    tools = Tools(Path(a.dv_solve_smt2 or b / "dv-solve-smt2"),
                  Path(a.replay or b / "bench_vlt_replay"))
    arms = a.arms.split(",")
    for x in arms:
        if x not in ARMS_ALL:
            ap.error(f"unknown arm {x}; choose from {','.join(ARMS_ALL)}")
    probs = load_problems(a.source or ["families", "corpus"])
    if a.only:
        keys = a.only.split(",")
        probs = [p for p in probs if any(k in p.name for k in keys)]
    info = build_info(tools)
    print(f"# {info.get('version', '')}  lib {info.get('library_sha256', '?')[:16]}  "
          f"{len(probs)} problems, arms {','.join(arms)}")
    rows = run(probs, arms, tools, a.n, a.n_max, a.cap, a.alpha, a.seed, a.jobs, a.n_noenum,
               a.budget)
    meta = {"info": info, "args": vars(a), "date": time.strftime("%Y-%m-%d %H:%M:%S")}
    if a.json:
        with open(a.json, "w") as f:
            f.write(json.dumps({"meta": meta}) + "\n")
            for r in rows:
                f.write(json.dumps(r) + "\n")
    md = summary_md(rows, info)
    if a.md:
        Path(a.md).write_text(md)
    print()
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
