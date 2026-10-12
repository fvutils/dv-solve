"""Summarize alloc_trace.so output by dv-solve call site.

Each traced stack is attributed to its first frame in dv-solve's own C
sources (src/c, excluding the bundled SAT solvers under src/c/sat/), with
the next two dv-solve frames as context. Stacks with no dv-solve frame
(Python, libstdc++ startup) go to "<outside>"; stacks whose first dv-solve
frame is in src/c/sat/ count as "<sat backend>". Allocations made inside a
bundled SAT solver are keyed "[in sat] <dv-solve caller>".

    python tests/perf/alloc_trace/alloc_report.py OUT_PREFIX [--top 40] [--json f]

OUT_PREFIX is the ALLOC_TRACE_OUT given to the traced run; every
OUT_PREFIX.<pid> file is merged.
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import subprocess

NB = 48


def _parse(path):
    sites = []
    cur = None
    with open(path) as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            if t[0] == "S":
                cur = {"allocs": int(t[1]), "reallocs": int(t[2]), "frees": int(t[3]),
                       "bytes": int(t[4]), "live": int(t[5]), "peak": int(t[6]),
                       "frames": []}
                sites.append(cur)
            elif t[0] == "L":
                cur["life"] = [int(x) for x in t[1:]]
            elif t[0] == "Z":
                cur["size"] = [int(x) for x in t[1:]]
            elif t[0] == "F":
                cur["frames"].append((t[1], int(t[2], 16), int(t[3], 16)))
    return sites


def _is_pie(obj, _cache={}):
    if obj not in _cache:
        try:
            with open(obj, "rb") as f:
                hdr = f.read(18)
            _cache[obj] = hdr[16] == 3          # ET_DYN
        except OSError:
            _cache[obj] = True
    return _cache[obj]


def _symbolize(frames):
    """frames: set of (obj, off, base) -> {(obj,off,base): [(func, file:line), ...]}"""
    by_obj = collections.defaultdict(list)
    for fr in frames:
        if fr[0] != "?" and os.path.exists(fr[0]):
            by_obj[fr[0]].append(fr)
    out = {}
    for obj, frs in by_obj.items():
        pie = _is_pie(obj)
        addrs = [(fr[1] if pie else fr[1] + fr[2]) - 1 for fr in frs]
        res = subprocess.run(["addr2line", "-a", "-f", "-i", "-C", "-e", obj] +
                             ["0x%x" % a for a in addrs],
                             capture_output=True, text=True).stdout.splitlines()
        # -a heads each address's group of (function, file:line) pairs
        # (more than one pair when inlined) with the address itself.
        groups, cur = [], None
        for line in res:
            if line.startswith("0x") and (cur is None or len(cur) % 2 == 0):
                cur = []
                groups.append(cur)
            else:
                cur.append(line)
        for fr, g in zip(frs, groups):
            out[fr] = [(g[k], g[k + 1]) for k in range(0, len(g) - 1, 2)]
    return out


def _ours(loc):
    return "/src/c/" in loc and "/src/c/sat/" not in loc


def _short(loc):
    i = loc.find("/src/c/")
    s = loc[i + len("/src/c/"):] if i >= 0 else os.path.basename(loc)
    return s.split(" ")[0]


def _median_bucket(hist):
    n = sum(hist)
    if not n:
        return None
    acc = 0
    for b, c in enumerate(hist):
        acc += c
        if acc * 2 >= n:
            return b
    return None


def _fmt_ns(b):
    if b is None:
        return "-"
    v = 1 << b
    for unit, d in (("s", 1e9), ("ms", 1e6), ("us", 1e3)):
        if v >= d:
            return "%.3g%s" % (v / d, unit)
    return "%dns" % v


def load(prefix):
    sites = []
    for p in glob.glob(prefix + ".*"):
        sites.extend(_parse(p))
    frames = {fr for s in sites for fr in s["frames"]}
    sym = _symbolize(frames)
    agg = {}
    for s in sites:
        chain = []
        sat = False
        for fr in s["frames"]:
            for func, loc in sym.get(fr, []):
                if "/src/c/sat/" in loc and not chain:
                    sat = True
                if _ours(loc):
                    chain.append("%s %s" % (func, _short(loc)))
        if chain:
            key = ("[in sat] " if sat else "") + chain[0]
            ctx = " <- ".join(chain[1:3])
        else:
            key, ctx = ("<sat backend>" if sat else "<outside>"), ""
        a = agg.setdefault(key, {"site": key, "ctx": collections.Counter(), "allocs": 0,
                                 "reallocs": 0, "frees": 0, "bytes": 0, "peak": 0,
                                 "live": 0, "life": [0] * NB, "size": [0] * NB})
        for k in ("allocs", "reallocs", "frees", "bytes", "live"):
            a[k] += s[k]
        a["peak"] = max(a["peak"], s["peak"])
        a["life"] = [x + y for x, y in zip(a["life"], s.get("life", [0] * NB))]
        a["size"] = [x + y for x, y in zip(a["size"], s.get("size", [0] * NB))]
        a["ctx"][ctx] += s["allocs"] + s["reallocs"]
    return list(agg.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json")
    ap.add_argument("--calls", type=int, default=0,
                    help="workload operations (randomize calls, solves) for per-op rates")
    args = ap.parse_args()
    rows = load(args.prefix)
    rows.sort(key=lambda r: -(r["allocs"] + r["reallocs"]))
    tot = sum(r["allocs"] + r["reallocs"] for r in rows)
    ours = sum(r["allocs"] + r["reallocs"] for r in rows
               if not r["site"].startswith("<"))
    print("total alloc+realloc calls %d; in dv-solve sources %d; frees %d" %
          (tot, ours, sum(r["frees"] for r in rows)))
    if args.calls:
        print("per operation: %.1f (dv-solve %.1f)" % (tot / args.calls, ours / args.calls))
    print("%9s %8s %10s %8s %8s %8s  %s" % ("calls", "reallocs", "bytes", "med-size",
                                            "med-life", "peak", "site [top caller]"))
    for r in rows[:args.top]:
        mz = _median_bucket(r["size"])
        print("%9d %8d %10d %8s %8s %8d  %s [%s]" % (
            r["allocs"] + r["reallocs"], r["reallocs"], r["bytes"],
            "-" if mz is None else str(1 << mz), _fmt_ns(_median_bucket(r["life"])),
            r["peak"], r["site"], r["ctx"].most_common(1)[0][0] if r["ctx"] else ""))
    if args.json:
        for r in rows:
            r["ctx"] = r["ctx"].most_common(5)
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
