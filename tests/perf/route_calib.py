"""Calibrate the deterministic engine-routing costs (docs/deterministic_routing_plan.md).

Verilator-mode routing used to decide on wall-clock time. It now decides on
work counts the solver reports in its DV_LOG `vlt-time` lines: CDCL
propagator firings (`ticks=`), conflicts, and the problem size (`vars=`,
`props=`, `psz=` pool bytes);
bit-blast clauses and SAT work by backend (`bbclk=`/`bbkw=` kissat clauses
and ticks, `bbclc=`/`bbcw=` CaDiCaL clauses and polls), and AIG node visits
outside the SAT solver (`bbax=`). This replays a set of Verilator-shaped workloads with each
engine forced, takes the fastest of a few runs per check-sat, and fits
microseconds per unit of each count, so the routing thresholds can be stated
in work while meaning what the old microsecond thresholds meant.

    PYTHONPATH=src python3 -m tests.perf.route_calib [--exe build/dv-solve-smt2] [--csv out.csv]
"""
from __future__ import annotations

import argparse
import csv
import os
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from tests.perf import vlt_protocol
from tests.perf.rand_benches import ENUMERABLE, WIDE, Bench

_REPO = Path(__file__).resolve().parents[2]
_HANDSHAKE = "(set-logic QF_ABV)\n(check-sat)\n(reset)\n"
_LINE = re.compile(r"^vlt-time (\d+) (\S+) vars=(\d+) props=(\d+) conflicts=(\d+) ticks=(\d+)"
                   r" bbclk=(\d+) bbkw=(\d+) bbclc=(\d+) bbcw=(\d+) bbax=(\d+) psz=(\d+)")
FIELDS = ("us", "route", "vars", "props", "conflicts", "ticks", "bbclk", "bbkw", "bbclc", "bbcw", "bbax", "psz")

# Engine forcing: shape routing off (CDCL wherever the instance router sends
# it), or bit-blast for everything.
MODES = {"cdcl": {"DV_VLT_SHAPE": "0"}, "bitblast": {"DV_ENGINE": "bitblast"},
         "bitblast-cadical": {"DV_ENGINE": "bitblast", "DV_SAT_BACKEND": "cadical"}}


def _illegal_instr(n: int) -> str:
    body = (_REPO / "tests/unit/data/vlt_illegal_instr_push.smt2").read_text(encoding="utf-8")
    body = body.replace("(push 1)\n", "")
    return "".join(f"(set-option :random-seed {k + 1})\n" + body
                   + f"(assert (not (= func7 (_ bv{k + 1} 7))))\n"
                   + "(check-sat)\n(get-value (exception))\n(reset)\n" for k in range(n))


_PRE = ("(set-option :produce-models true)\n(set-logic QF_ABV)\n"
        "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
        "(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))\n")


def _divisibility(n: int) -> str:
    body = ("(declare-fun init () (_ BitVec 32))\n(declare-fun step () (_ BitVec 32))\n"
            "(declare-fun limit () (_ BitVec 32))\n"
            "(assert (bvsge init #xfffffff6))(assert (bvsle init #x0000000a))\n"
            "(assert (bvsge step #xfffffff6))(assert (bvsle step #x0000000a))\n"
            "(assert (bvsge limit #xffffffe0))(assert (bvsle limit #x00000020))\n"
            "(assert (not (= limit init)))\n"
            "(assert (ite (bvslt init limit) (bvsgt step #x00000000) (bvslt step #x00000000)))\n"
            "(assert (= (bvsmod (bvsub limit init) step) #x00000000))\n")
    return "".join(f"(set-option :random-seed {k + 1})\n" + _PRE + body
                   + "(check-sat)\n(get-value (init step limit))\n(reset)\n" for k in range(n))


def _bench(name: str, exe: str, n: int) -> str:
    """The commands a Verilator randomize() loop sends, for bench `name`."""
    b = Bench(name)
    log: list = []
    s = vlt_protocol.Session([exe, "--interactive", "--mode=verilator"], b.widths,
                             b.smt_vlt(), log=log)
    rng = random.Random(7)
    try:
        for _ in range(n):
            s.randomize(rng)
    finally:
        s.close()
    return "".join(log)


def workloads(exe: str, n: int) -> dict:
    w = {"illegal_instr": _HANDSHAKE + _illegal_instr(n),
         "divisibility": _HANDSHAKE + _divisibility(n)}
    for name in list(ENUMERABLE) + list(WIDE):
        w[f"bench:{name}"] = _bench(name, exe, n)
    return w


def run(exe: str, script: str, env: dict) -> list:
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "dv.log"
        subprocess.run([exe, "--interactive", "--mode=verilator"], input=script + "(exit)\n",
                       capture_output=True, text=True, timeout=600,
                       env={**os.environ, "DV_LOG": str(log), **env})
        text = log.read_text(encoding="utf-8") if log.exists() else ""
    rows = []
    for line in text.splitlines():
        m = _LINE.match(line)
        if m:
            g = m.groups()
            rows.append(dict(zip(FIELDS, [int(g[0]), g[1]] + [int(x) for x in g[2:]])))
    return rows


def collect(exe: str, n: int, reps: int) -> list:
    out = []
    for wname, script in workloads(exe, n).items():
        for mode, env in MODES.items():
            runs = [run(exe, script, env) for _ in range(reps)]
            for i, row in enumerate(runs[0]):
                same = [r[i] for r in runs if len(r) > i]
                row = dict(row, us=min(r["us"] for r in same), workload=wname, mode=mode, idx=i)
                out.append(row)
    return out


def _fit(xs: list, ys: list) -> list:
    """Non-negative least squares by coordinate descent (few columns). On
    absolute error: the routing decisions are all made in the milliseconds
    range (a shape is slow past 3 ms), so that is where the fit must hold;
    a cheap check-sat's estimate only has to stay far below it."""
    k = len(xs[0])
    ws = [1.0] * len(ys)
    c = [0.0] * k
    for _ in range(2000):
        for j in range(k):
            num = sum(w * (y - sum(c[i] * x[i] for i in range(k) if i != j)) * x[j]
                      for x, y, w in zip(xs, ys, ws))
            den = sum(w * x[j] * x[j] for x, w in zip(xs, ws)) or 1.0
            c[j] = max(0.0, num / den)
    return c


def fit(rows: list) -> dict:
    cd = [r for r in rows if r["mode"] == "cdcl" and r["route"].startswith("cdcl/")
          and "->" not in r["route"] and r["idx"] > 0]
    bb = [r for r in rows if r["mode"].startswith("bitblast") and r["idx"] > 0]
    res = {}
    if cd:
        res["cdcl"] = _fit([(1.0, r["ticks"], r["conflicts"], r["vars"] + r["props"], r["psz"])
                            for r in cd],
                           [r["us"] for r in cd])
    if bb:
        res["bitblast"] = _fit([(1.0, r["bbclk"], r["bbkw"], r["bbclc"], r["bbcw"], r["bbax"],
                                 r["psz"]) for r in bb],
                               [r["us"] for r in bb])
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(_REPO / "build" / "dv-solve-smt2"))
    ap.add_argument("--n", type=int, default=20, help="randomize() calls per workload")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--csv")
    ap.add_argument("--from-csv", help="refit an earlier --csv instead of running")
    a = ap.parse_args(argv)
    if a.from_csv:
        with open(a.from_csv, newline="") as f:
            rows = [{k: (v if k in ("workload", "mode", "route") else int(v)) for k, v in r.items()}
                    for r in csv.DictReader(f)]
    else:
        rows = collect(a.exe, a.n, a.reps)
    if a.csv and not a.from_csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=["workload", "mode", "idx", *FIELDS])
            w.writeheader()
            w.writerows(rows)
    for eng, c in fit(rows).items():
        print(eng, " ".join(f"{x:.4g}" for x in c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
