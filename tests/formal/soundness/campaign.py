"""Soundness campaign: generate, run every front door, judge, shrink, record.

    python -m tests.formal.soundness.campaign --seed 1 --n 2000 [--doors smt2,incr,steps,builder]
        [--exe build/dv-solve-smt2] [--out DIR]

Every problem is small enough to enumerate, so each answer and model is
judged against brute force. A failure is shrunk and written to --out (by
default tests/formal/soundness/regressions/) as JSON, which
test_regressions.py replays through every door. Run against a
-DDVS_STEP_CHECK=ON build, every learnt clause and explanation is checked
too; a violation shows on stderr and counts as a failure.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import random
import sys
import time
from pathlib import Path

from . import doors
from .gen import Gen, WIDE_WIDTHS, WIDTHS
from .ir import Problem
from .shrink import shrink

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
DEFAULT_EXE = REPO / "build" / "dv-solve-smt2"


BUILDER_LIMIT_MS = 10000   # raised for step-checker runs, which fork per step


def run_doors(p: Problem, which, exe: str) -> list:
    out = []
    if "smt2" in which:
        out += doors.smt2_batch(p, exe)
    if "incr" in which:
        out += doors.smt2_incremental(p, exe)
    if "steps" in which:
        out += doors.smt2_steps(p, exe)
    if "builder" in which and doors.builder_safe(p):
        out += doors.builder(p, BUILDER_LIMIT_MS)
    if "protocol" in which and doors.builder_safe(p):
        out += doors.protocol(p)
    return out


def _step_check_violation(o) -> bool:
    return "[step-check] INVALID" in o.detail


def failing(outcomes) -> list:
    return [o for o in outcomes if o.failed or _step_check_violation(o)]


def to_json(p: Problem) -> dict:
    return {"widths": p.widths, "cons": p.cons}


def from_json(d: dict) -> Problem:
    def tup(x):
        return tuple(tup(y) for y in x) if isinstance(x, list) else x
    return Problem(dict(d["widths"]), [tup(c) for c in d["cons"]])


def record(p: Problem, why: str, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha1(json.dumps(to_json(p), sort_keys=True).encode()).hexdigest()[:10]
    path = out_dir / f"r_{h}.json"
    path.write_text(json.dumps({"why": why, **to_json(p)}, indent=1) + "\n")
    path.with_suffix(".smt2").write_text(f"; {why}\n" + p.smt2(get_model=False))
    return path


def campaign(seed: int, n: int, which, exe: str, out_dir: Path | None, log=print,
             wide: float = 0.0, minutes: float = 0.0):
    rng = random.Random(seed)
    stats = collections.Counter()
    bins = collections.Counter()
    failures = []
    t0 = time.time()
    deadline = t0 + 60 * minutes if minutes else None
    for i in range(n if not minutes else 1 << 30):
        if deadline and time.time() > deadline:
            break
        stats["problems"] += 1
        # Half the problems keep to the operators the builder shares with
        # SMT-LIB, so every door sees a share of the stimulus.
        builder_safe = ("builder" in which or "protocol" in which) and rng.random() < 0.5
        layout = rng.choice(WIDE_WIDTHS) if rng.random() < wide else rng.choice(WIDTHS)
        p = Gen(rng, layout, builder_safe=builder_safe).problem()
        if max(layout.values()) > 8:
            p.bins.add(f"width:{max(layout.values())}")
        for b in p.bins:
            bins[b] += 1
        t1 = time.time()
        outs = run_doors(p, which, exe)
        if time.time() - t1 > 2.0:
            # Speed is reported, not judged here: a timeout (60 s per call)
            # is a failure, a slow answer is a number for the nightly report.
            stats["slow>2s"] += 1
        for o in outs:
            stats[f"{o.door.split('[')[0]}:{o.got}"] += 1
        bad = failing(outs)
        if not bad:
            continue
        first = bad[0]
        why = (f"seed={seed} i={i} door={first.door} expect={first.expect} got={first.got}"
               f"{'' if first.model_ok else ' bad-model'}"
               f"{' step-check' if _step_check_violation(first) else ''}")
        log("FAIL " + why)

        def sig(o):
            # A protocol outcome names its solve and pin, which are drawn from
            # the problem text and so move as it shrinks: keep the door only.
            door = "protocol" if o.door.startswith("protocol") else o.door
            return (door, o.expect, o.got, o.model_ok, _step_check_violation(o))

        def still_fails(q, want=sig(first)):
            # The same door must fail the same way, or shrinking can wander
            # off to a different bug.
            door = want[0].split("[")[0]
            sel = {"smt2": ["smt2"], "smt2-incr": ["incr"], "smt2-steps": ["steps"],
                   "builder": ["builder"], "protocol": ["protocol"]}[door]
            return any(sig(o) == want for o in failing(run_doors(q, sel, exe)))
        small = shrink(p, still_fails)
        log("     shrunk: " + small.smt2(get_model=False).replace("\n", " "))
        failures.append((why, small))
        if out_dir is not None:
            log("     wrote " + str(record(small, why, out_dir)))
    stats["seconds"] = int(time.time() - t0)
    return failures, stats, bins


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--n", type=int, default=1000)
    ap.add_argument("--doors", default="smt2,incr,steps,builder,protocol")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--out", default=str(HERE / "regressions"))
    ap.add_argument("--minutes", type=float, default=0.0,
                    help="run for this long instead of --n problems")
    ap.add_argument("--ucis", default="",
                    help="write stimulus coverage to this UCIS (covsight NCDB) file")
    ap.add_argument("--wide", type=float, default=0.0,
                    help="share of problems with 16- to 65-bit variables (z3 is their oracle)")
    ap.add_argument("--builder-limit-ms", type=int, default=BUILDER_LIMIT_MS,
                    help="per-solve budget for the builder door (raise it under the step checker)")
    a = ap.parse_args(argv)
    globals()["BUILDER_LIMIT_MS"] = a.builder_limit_ms
    failures, stats, bins = campaign(a.seed, a.n, a.doors.split(","), a.exe, Path(a.out),
                                     wide=a.wide, minutes=a.minutes)
    if a.ucis:
        from . import ucis_coverage
        ucis_coverage.write(a.ucis, bins, stats, f"campaign_seed{a.seed}", a.seed)
    print("STATS", dict(sorted(stats.items())))
    print("BINS", dict(sorted(bins.items())))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
