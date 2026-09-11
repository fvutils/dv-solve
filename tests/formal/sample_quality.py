#!/usr/bin/env python3
"""Randomization quality harness: how uniform are dv-solve's models?

Implements the metric set from docs/randomness_benchmark_plan.md so numbers are
comparable with Verilator PR#8042 (UniGen2):

  coverage   -- distinct solutions hit / |solution space|      (higher better)
  JSD        -- Jensen-Shannon divergence vs uniform, in BITS  (lower better)
               reported both normalized ([0,1]) and x100, since PR#8042's table
               quotes values >1 and so must use a scaled estimator.
  chi2 p     -- goodness-of-fit p-value vs uniform             (want > 0.01)
  thin       -- observed/expected frequency of the RAREST branch of the space

Ground truth comes from z3 (independent oracle), never from dv-solve.

A `uniform` arm -- a true RNG drawing from the enumerated space -- is always
included.  Its scores are the BIAS FLOOR at that sample count: a sampler that
matches it is perfect, and "better" than it is noise, not quality.

Usage:
    python3 tests/formal/sample_quality.py [--budgets 1,5] [--bench countones]
"""
from __future__ import annotations

import argparse
import math
import os
import random
import re
import shutil
import subprocess
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_VAL = re.compile(r"\(\s*([A-Za-z_][\w]*)\s+(#b[01]+|#x[0-9a-fA-F]+)\s*\)")

# --------------------------------------------------------------------------
# Benchmarks -- QF_BV so the CDCL engine is genuinely exercised.
# Each: (name, decls+asserts, var names, thin-branch predicate or None)
# --------------------------------------------------------------------------


def _countones() -> tuple:
    bits = [f"((_ zero_extend 7) ((_ extract {i} {i}) x))" for i in range(8)]
    acc = bits[0]
    for b in bits[1:]:
        acc = f"(bvadd {acc} {b})"
    return ("countones",
            ["(declare-fun x () (_ BitVec 8))", f"(assert (= {acc} #b00000100))"],
            ["x"], None)


def _ot_aon_wkup() -> tuple:
    """OpenTitan aon_timer shape: thold==0 is a 1-solution branch of 1003."""
    return ("ot_aon_wkup",
            ["(declare-fun thold () (_ BitVec 8))",
             "(declare-fun cnt () (_ BitVec 8))",
             "(assert (or (= thold #x00) "
             "(and (bvuge thold #x05) (bvule thold #xab))))",
             "(assert (ite (= thold #x00) (= cnt #x00) "
             "(and (bvuge cnt (bvsub thold #x05)) (bvule cnt thold))))"],
            ["thold", "cnt"],
            lambda m: m["thold"] == 0)


def _range_1000() -> tuple:
    """Control: a plain contiguous range. Every sampler should do well here."""
    return ("range_1000",
            ["(declare-fun r () (_ BitVec 16))",
             "(assert (bvult r #x03e8))"],
            ["r"], None)


def _disjoint_ranges() -> tuple:
    """`x inside {[10:20],[100:150]}` -- a domain with a HOLE. The shape that
    exposed B18: a phase-saved value in the gap passes the coarse bounds check.
    Thin branch = the small lower band (11 of 62 solutions)."""
    return ("disjoint_ranges",
            ["(declare-fun x () (_ BitVec 8))",
             "(assert (or (and (bvuge x #x0a) (bvule x #x14)) "
             "(and (bvuge x #x64) (bvule x #x96))))"],
            ["x"], lambda m: m["x"] <= 0x14)


def _sum_eq_const() -> tuple:
    """a + b == 100 over 8-bit vars: 256 solutions, tightly coupled pair."""
    return ("sum_eq_const",
            ["(declare-fun a () (_ BitVec 8))", "(declare-fun b () (_ BitVec 8))",
             "(assert (= (bvadd a b) #x64))"],
            ["a", "b"], None)


def _a_lt_b() -> tuple:
    """a < b over 6-bit vars: 2016 solutions, an ordering coupling. This is the
    shape _select_unassigned's fair_pick note calls out as marginal-skewing."""
    return ("a_lt_b",
            ["(declare-fun a () (_ BitVec 6))", "(declare-fun b () (_ BitVec 6))",
             "(assert (bvult a b))"],
            ["a", "b"], lambda m: m["a"] == 0)


def _dep_range() -> tuple:
    """b in [a : a+10], a <= 80 -- a value-dependent range (the ot_aon shape
    without the disjunction, so CDCL can actually compile it)."""
    return ("dep_range",
            ["(declare-fun a () (_ BitVec 8))", "(declare-fun b () (_ BitVec 8))",
             "(assert (bvule a #x50))", "(assert (bvuge b a))",
             "(assert (bvule b (bvadd a #x0a)))"],
            ["a", "b"], lambda m: m["a"] == 0)


def _bit_mask() -> tuple:
    """(x & 0x0f) == 5: 16 solutions scattered by high bits -- a bitwise shape."""
    return ("bit_mask",
            ["(declare-fun x () (_ BitVec 8))",
             "(assert (= (bvand x #x0f) #x05))"],
            ["x"], None)


BENCHES = {b[0]: b for b in (_countones(), _ot_aon_wkup(), _range_1000(),
                             _disjoint_ranges(), _sum_eq_const(), _a_lt_b(),
                             _dep_range(), _bit_mask())}
HDR = "(set-logic QF_BV)"


def _as_int(lit: str) -> int:
    return int(lit[2:], 2) if lit.startswith("#b") else int(lit[2:], 16)


# --------------------------------------------------------------------------
# Ground truth: exhaustive enumeration with z3 + blocking clauses
# --------------------------------------------------------------------------

def enumerate_space(decls: list[str], names: list[str], cap: int = 200000) -> list[tuple]:
    if not shutil.which("z3"):
        sys.exit("z3 is required for ground truth")
    p = subprocess.Popen(["z3", "-in"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True, bufsize=1)

    def send(s):
        p.stdin.write(s + "\n")
        p.stdin.flush()

    for d in [HDR] + decls:
        send(d)
    def verdict() -> str:
        """Read to the next sat/unsat/unknown.

        The get-value reader below stops on the balanced ')' and leaves the
        trailing newline in the buffer, so a bare readline() here would return
        "" and look like the end of the enumeration after exactly one model.
        """
        while True:
            line = p.stdout.readline()
            if not line:
                return "eof"
            if line.strip():
                return line.strip()

    out = []
    while len(out) < cap:
        send("(check-sat)")
        if verdict() != "sat":
            break
        send("(get-value (" + " ".join(names) + "))")
        buf, depth = "", 0
        while True:
            ch = p.stdout.read(1)
            if not ch:
                break
            buf += ch
            depth += (ch == "(") - (ch == ")")
            if depth == 0 and buf.strip():
                break
        m = dict(_VAL.findall(buf))
        if len(m) != len(names):
            break
        out.append(tuple(_as_int(m[n]) for n in names))
        send("(assert (or " + " ".join(
            f"(not (= {n} {m[n]}))" for n in names) + "))"
            if len(names) > 1 else
            f"(assert (not (= {names[0]} {m[names[0]]})))")
    p.stdin.close()
    p.kill()
    return out


# --------------------------------------------------------------------------
# Samplers
# --------------------------------------------------------------------------

def _dv_once(args) -> tuple | None:
    """One solve in a FRESH process (CDCL re-randomizes only on first solve)."""
    decls, names, engine, seed, env = args
    script = "\n".join([HDR] + decls + [f"(set-option :seed {seed})",
                                        "(check-sat)",
                                        "(get-value (" + " ".join(names) + "))",
                                        "(exit)"]) + "\n"
    try:
        p = subprocess.run([str(_DV), "--interactive", f"--engine={engine}"],
                           input=script, capture_output=True, text=True,
                           timeout=120, env={**os.environ, **(env or {})})
    except subprocess.TimeoutExpired:
        return None
    m = dict(_VAL.findall(p.stdout))
    if len(m) != len(names):
        return None
    return tuple(_as_int(m[n]) for n in names)


def sample_dv_fresh(decls, names, engine, n, workers=8, env=None) -> list[tuple]:
    work = [(decls, names, engine, s + 1, env) for s in range(n)]
    with ProcessPoolExecutor(max_workers=workers) as ex:
        return [r for r in ex.map(_dv_once, work, chunksize=16) if r]


def sample_dv_session(decls, names, engine, n, env=None) -> list[tuple]:
    """All n draws in ONE process -- the shape a live randomize() loop uses."""
    cmds = [HDR] + decls
    for s in range(n):
        cmds += [f"(set-option :seed {s + 1})", "(check-sat)",
                 "(get-value (" + " ".join(names) + "))"]
    cmds.append("(exit)")
    p = subprocess.run([str(_DV), "--interactive", f"--engine={engine}"],
                       input="\n".join(cmds) + "\n", capture_output=True,
                       text=True, timeout=3600, env={**os.environ, **(env or {})})
    vals, cur = [], {}
    for name, lit in _VAL.findall(p.stdout):
        cur[name] = lit
        if len(cur) == len(names):
            vals.append(tuple(_as_int(cur[n]) for n in names))
            cur = {}
    return vals


# --------------------------------------------------------------------------
# Metrics
# --------------------------------------------------------------------------

def metrics(samples: list[tuple], space: list[tuple], thin) -> dict:
    K, N = len(space), len(samples)
    idx = {s: i for i, s in enumerate(space)}
    cnt = Counter(s for s in samples if s in idx)
    off = sum(1 for s in samples if s not in idx)   # must be 0: unsound model

    p = [cnt.get(s, 0) / N for s in space] if N else [0.0] * K
    q = [1.0 / K] * K
    m = [(pi + qi) / 2 for pi, qi in zip(p, q)]

    def kl(a, b):
        return sum(ai * math.log2(ai / bi) for ai, bi in zip(a, b) if ai > 0)

    jsd = 0.5 * kl(p, m) + 0.5 * kl(q, m)          # in bits, 0..1

    exp = N / K
    chi2 = sum((cnt.get(s, 0) - exp) ** 2 / exp for s in space) if exp > 0 else 0.0
    try:
        from scipy.stats import chi2 as _c2
        pval = float(_c2.sf(chi2, K - 1))
    except Exception:
        pval = float("nan")

    res = {"n": N, "missing": 0, "coverage": len(cnt) / K, "jsd": jsd, "jsd_x100": jsd * 100,
           "chi2_p": pval, "offspace": off}
    if thin:
        tset = [s for s in space if thin(dict(zip(NAMES, s)))]
        got = sum(cnt.get(s, 0) for s in tset)
        res["thin_ratio"] = (got / N) / (len(tset) / K) if N and tset else float("nan")
    return res


NAMES: list[str] = []


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--budgets", default="1,5")
    ap.add_argument("--bench", default="disjoint_ranges,sum_eq_const,a_lt_b,dep_range,bit_mask,range_1000")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--cdcl-limit", type=int, default=2,
                    help="DV_CDCL_TIME_LIMIT seconds for the pure-CDCL arms")
    a = ap.parse_args()
    budgets = [int(x) for x in a.budgets.split(",")]

    global NAMES
    for bname in a.bench.split(","):
        name, decls, names, thin = BENCHES[bname]
        NAMES = names
        space = enumerate_space(decls, names)
        K = len(space)
        print(f"\n=== {name}: |solution space| = {K} "
              f"(ground truth: z3 exhaustive) ===")
        print(f"{'arm':<22}{'budget':>7}{'N':>8}{'coverage':>10}"
              f"{'JSD(bits)':>11}{'JSDx100':>9}{'chi2 p':>9}"
              f"{'thin':>7}{'miss':>7}")
        for mult in budgets:
            N = K * mult
            # DV_NO_BITBLAST=1 on the CDCL arms: without it an `unknown` from
            # CDCL silently escalates to bitblast, so the arm would measure
            # bitblast's distribution while wearing a "cdcl" label.
            PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": str(a.cdcl_limit)}
            arms = [
                ("uniform (bias floor)",
                 lambda: [random.choice(space) for _ in range(N)]),
                ("dv cdcl (fresh proc)",
                 lambda: sample_dv_fresh(decls, names, "cdcl", N, a.workers, PURE)),
                ("dv cdcl (session)",
                 lambda: sample_dv_session(decls, names, "cdcl", N, PURE)),
                ("dv cdcl+fairpick (sess)",
                 lambda: sample_dv_session(decls, names, "cdcl", N,
                                           {**PURE, "DV_FAIR_PICK": "1"})),
                ("dv bitblast (fresh)",
                 lambda: sample_dv_fresh(decls, names, "bitblast", N, a.workers)),
                ("dv bitblast (session)",
                 lambda: sample_dv_session(decls, names, "bitblast", N)),
            ]
            for label, fn in arms:
                s = fn()
                r = metrics(s, space, thin)
                r["missing"] = N - r["n"]
                thin_s = (f"{r['thin_ratio']:.2f}" if "thin_ratio" in r else "-")
                if r["n"] == 0:
                    print(f"{label:<22}{mult:>6}x{0:>8}"
                          f"{'  NO MODELS (unknown/timeout)':>46}")
                    continue
                print(f"{label:<22}{mult:>6}x{r['n']:>8}"
                      f"{r['coverage']*100:>9.1f}%{r['jsd']:>11.4f}"
                      f"{r['jsd_x100']:>9.2f}{r['chi2_p']:>9.3f}"
                      f"{thin_s:>7}{r['missing']:>7}")


if __name__ == "__main__":
    main()
