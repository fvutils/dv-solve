"""Routing gaps by rule, from a perfmodel_corpus training file.

    python -m tests.perf.route_gaps perf-out/perfmodel/train-<commit>.json

For each routing reason the default route recorded (the stats `reasons` of an
item's classes; "cdcl-default" when none sent it elsewhere): the items it
decided, how many of them one of our own forced engines solved at least
MISROUTE_X faster, and the time lost to that, with the worst items. Only our
own engines count here: this is the gap that routing alone can close
(docs/routing_gap_plan.md).
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict

MISROUTE_X = 2.0


def rows(train: dict):
    """(item, reasons, ours, best own arm, its time) per SMT-LIB2 item."""
    cls = {c["class"]: c for c in train["stats"]["snapshot"]["classes"]}
    for name, v in train["items"].items():
        if not v or not name.endswith(".smt2"):
            continue
        own = {a: t for a, t in v["arms"].items() if a.startswith("dv-")}
        reasons = sorted({r for c in v["classes"] for r, n in cls[c].get("reasons", {}).items()
                          if n} or {"cdcl-default"})
        best = min(own, key=own.get) if own else None
        yield name, tuple(reasons), v["ours"], best, own.get(best, v["ours"])


def report(train: dict, top: int = 5) -> str:
    by = defaultdict(list)
    for r in rows(train):
        by[r[1]].append(r)
    lines = []
    total_lost = 0.0
    for reasons, rs in sorted(by.items(), key=lambda kv: -sum(max(0, r[2] - r[4]) for r in kv[1])):
        bad = [r for r in rs if r[4] * MISROUTE_X <= r[2]]
        lost = sum(r[2] - r[4] for r in bad)
        total_lost += lost
        lines.append(f"{'+'.join(reasons)}: {len(rs)} items, {len(bad)} misrouted, {lost:.2f} s lost; "
                     f"better engine: {dict(Counter(r[3] for r in bad))}")
        for name, _, ours, best, t in sorted(bad, key=lambda r: -(r[2] - r[4]))[:top]:
            lines.append(f"    {name.split('/smt2/')[-1]:62s} {ours * 1e3:8.1f} ms -> {best} {t * 1e3:7.1f} ms")
    allr = list(rows(train))
    ours = sum(r[2] for r in allr)
    own = sum(min(r[2], r[4]) for r in allr)
    lines.insert(0, f"{len(allr)} SMT-LIB2 items: {ours:.1f} s on the default route, {own:.1f} s on "
                    f"our own best engine per item; {total_lost:.1f} s lost to misroutes "
                    f"(an own engine >= {MISROUTE_X:.0f}x faster)")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("train")
    ap.add_argument("--top", type=int, default=5)
    a = ap.parse_args(argv)
    with open(a.train, encoding="utf-8") as f:
        print(report(json.load(f), a.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
