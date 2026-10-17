"""Hotspot timing and profiling for docs/perf_plan_2026-10-17.md.

Four workloads, each a whole CLI process (so startup cost counts):

  easy  every fixture in easy_list.txt, x10, one dv-solve-smt2 process each
  a57   tests/formal/corpus/.../vlsat3_a57.smt2, one-shot
  a67   tests/formal/corpus/.../vlsat3_a67.smt2, one-shot
  vlt   --interactive --mode=verilator on a generated randomize transcript

Usage (from the repo root, with env -u LD_LIBRARY_PATH):

  python -m tests.perf.hotspots.hotspots gen-vlt  [--bin B] [--n 300]
  python -m tests.perf.hotspots.hotspots time     [--bin B ...] [--runs 3] [--only easy,a67]
  python -m tests.perf.hotspots.hotspots profile  [--bin B] [--only a67]

`time` prints the median of --runs per workload and binary. Pass --bin more
than once to compare builds (build an old one from a ref with
tests.perf.builds.build_tag, not by copying a binary). `profile` runs
perf record and prints the time grouped by component (kissat, loader,
dvs:<file>, ...). Generated files go in tests/perf/hotspots/out/.
"""
from __future__ import annotations

import argparse
import collections
import random
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
OUT = HERE / "out"
VLSAT = REPO / "tests/formal/corpus/smtcomp2025_parallel/smt2/QF_BV/20210312-Bouvier"
TRANSCRIPT = OUT / "vlt_transcript.smt2"
EASY_REPS = 10
EASY_TIMEOUT = 30


def _easy_list():
    return [REPO / l.strip() for l in (HERE / "easy_list.txt").read_text().splitlines() if l.strip()]


def gen_vlt(binp: Path, n: int, out: Path = TRANSCRIPT):
    """Record a Verilator-protocol session (n randomize per bench) as one transcript."""
    sys.path[:0] = [str(REPO), str(REPO / "src")]
    from tests.perf import vlt_protocol
    from tests.perf.rand_benches import Bench, ENUMERABLE, WIDE
    full = []
    for name in list(ENUMERABLE) + list(WIDE):
        b = Bench(name)
        log = []
        s = vlt_protocol.Session([str(binp), "--interactive", "--mode=verilator"],
                                 b.widths, b.smt_vlt(), log=log)
        rng = random.Random(1)
        for _ in range(n):
            s.randomize(rng)
        s.close()
        full += log if not full else log[1:]   # keep one liveness probe only
        print(f"  {name}: {len(log)} commands", file=sys.stderr)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("".join(full) + "(exit)\n")
    print(f"wrote {out}", file=sys.stderr)


def _workloads(binp: Path):
    """name -> list of (argv, stdin path or None)."""
    easy = [([str(binp), str(f)], None) for f in _easy_list()] * EASY_REPS
    return {
        "easy": easy,
        "a57": [([str(binp), str(VLSAT / "vlsat3_a57.smt2")], None)],
        "a67": [([str(binp), str(VLSAT / "vlsat3_a67.smt2")], None)],
        "vlt": [([str(binp), "--interactive", "--mode=verilator"], TRANSCRIPT)],
    }


def _script(name, cmds, tag=""):
    """A shell script running the whole workload: one fork per solve from sh,
    not from Python, so the easy band does not time Python's subprocess cost."""
    OUT.mkdir(exist_ok=True)
    script = OUT / f"run_{name}{tag}.sh"
    lines = ["#!/bin/sh"]
    for argv, stdin in cmds:
        lines.append(f"timeout {EASY_TIMEOUT} " + " ".join(f"'{a}'" for a in argv) +
                     (f" < '{stdin}'" if stdin else " < /dev/null") + " > /dev/null 2>&1")
    script.write_text("\n".join(lines) + "\n")
    script.chmod(0o755)
    return script


def _run(script, prefix=()):
    t0 = time.perf_counter()
    subprocess.run([*prefix, str(script)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return time.perf_counter() - t0


def _names(only):
    return only.split(",") if only else ["easy", "a57", "a67", "vlt"]


def cmd_time(bins, runs, only):
    if "vlt" in _names(only) and not TRANSCRIPT.exists():
        gen_vlt(bins[0], 300)
    res = collections.defaultdict(list)
    scripts = {(n, b): _script(n, _workloads(b)[n], f"_{i}")
               for i, b in enumerate(bins) for n in _names(only)}
    for _ in range(runs):
        for b in bins:                     # interleave builds to share drift
            for name in _names(only):
                res[(name, b)].append(_run(scripts[(name, b)]))
    for name in _names(only):
        for b in bins:
            ts = res[(name, b)]
            print(f"{name:5s} {statistics.median(ts):8.3f}s  "
                  f"[{' '.join(f'{t:.3f}' for t in ts)}]  {b}")


def _group(data: Path, binp: Path, top=16):
    """perf report grouped by component; symbols map to src files via nm -l."""
    rep = subprocess.run(["perf", "report", "-i", str(data), "--no-children", "--sort", "dso,sym",
                          "--stdio", "-F", "overhead,dso,sym"],
                         capture_output=True, text=True).stdout
    loc = {}
    for libp in [binp] + sorted(binp.parent.glob("libdv_solve.so*")):
        nm = subprocess.run(["nm", "-l", "--defined-only", str(libp)],
                            capture_output=True, text=True).stdout
        for l in nm.splitlines():
            p = l.split()
            if len(p) >= 4:
                loc.setdefault(p[2].split(".")[0], p[3])
    g = collections.Counter()
    for l in rep.splitlines():
        m = re.match(r"\s+([\d.]+)%\s+(\S+)\s+\[(.)\]\s+(\S+)", l)
        if not m:
            continue
        pct, dso, k, sym = float(m[1]), m[2], m[3], m[4]
        base = sym.split(".")[0]
        if "ld-linux" in dso:
            c = "dynamic loader (startup relocs)"
        elif k == "k" or dso.startswith("[k") or dso.startswith("[unk"):
            c = "kernel"
        elif "libc" in dso:
            c = ("libc mem*/str*" if re.search("mem|str", sym) else
                 "malloc/free" if re.search("alloc|free|_int_|tcache", sym) else "libc other")
        elif "libstdc" in dso:
            c = "libstdc++"
        else:
            f = loc.get(base, "")
            if "/kissat/" in f or base.startswith(("kissat", "kitten")):
                c = "kissat"
            elif "cadical" in f.lower() or "CaDiCaL" in sym:
                c = "cadical"
            elif "/smt2/" in f:
                c = "smt2 frontend"
            else:
                m2 = re.search(r"src/c/([^/:]+)\.c", f)
                c = "dvs:" + (m2[1] if m2 else "?" + base)
        g[c] += pct
    return "\n".join(f"{v:6.1f}%  {c}" for c, v in g.most_common(top))


def cmd_profile(binp, only):
    OUT.mkdir(exist_ok=True)
    if "vlt" in _names(only) and not TRANSCRIPT.exists():
        gen_vlt(binp, 300)
    w = _workloads(binp)
    for name in _names(only):
        data = OUT / f"perf_{name}.data"
        # One perf process for the whole workload, so the easy band's
        # 2000+ processes land in one profile.
        script = _script(name, w[name])
        subprocess.run(["perf", "record", "-q", "-F", "2000", "-g", "-o", str(data), str(script)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        txt = _group(data, binp)
        (OUT / f"groups_{name}.txt").write_text(txt + "\n")
        print(f"== {name}\n{txt}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-vlt")
    g.add_argument("--bin", default=str(REPO / "build/dv-solve-smt2"))
    g.add_argument("--n", type=int, default=300)
    t = sub.add_parser("time")
    t.add_argument("--bin", action="append")
    t.add_argument("--runs", type=int, default=3)
    t.add_argument("--only")
    p = sub.add_parser("profile")
    p.add_argument("--bin", default=str(REPO / "build/dv-solve-smt2"))
    p.add_argument("--only")
    a = ap.parse_args()
    if a.cmd == "gen-vlt":
        gen_vlt(Path(a.bin).resolve(), a.n)
    elif a.cmd == "time":
        cmd_time([Path(b).resolve() for b in (a.bin or [REPO / "build/dv-solve-smt2"])],
                 a.runs, a.only)
    else:
        cmd_profile(Path(a.bin).resolve(), a.only)


if __name__ == "__main__":
    main()
