"""Triage the hard corpus for the MEDIUM band: cases that actually SOLVE
(sat/unsat) in ~1-5s, on dv bitblast and/or bitwuzla. Emits wall + result +
the dv bitblast encode/sat split so we can locate the hotspot for that band.

Usage: direnv exec . python tests/formal/triage_medium.py --timeout 15
"""
from __future__ import annotations
import argparse, os, re, subprocess, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DV = REPO / "build" / "dv-solve-smt2"
BWZ = REPO / "packages" / "verilator-bin" / "bin" / "bitwuzla"
CORPUS = HERE / "corpus" / "smtcomp2025_parallel" / "smt2"
BB_RE = re.compile(r"bb=([\d.]+)ms sat=([\d.]+)ms ands=(\d+) vars=(\d+) clauses=(\d+)")


def parse_res(out):
    for line in out.splitlines():
        t = line.strip()
        if t in ("sat", "unsat", "unknown"):
            return t
    return "none"


def run(argv, env=None, timeout=15.0):
    e = os.environ.copy()
    if env:
        e.update(env)
    t0 = time.monotonic()
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=e)
    except subprocess.TimeoutExpired:
        return "timeout", (time.monotonic() - t0) * 1000.0, ""
    dt = (time.monotonic() - t0) * 1000.0
    return parse_res(p.stdout), dt, p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--out", default=str(HERE / "results" / "triage_medium.csv"))
    args = ap.parse_args()

    files = sorted(CORPUS.rglob("*.smt2"))
    rows = []
    for fx in files:
        rel = str(fx.relative_to(CORPUS))
        bb_res, bb_ms, bb_err = run([str(DV), str(fx)], env={"DV_ENGINE": "bitblast", "DV_BB_STATS": "1"}, timeout=args.timeout)
        enc = sat = ncl = nv = None
        m = BB_RE.search(bb_err)
        if m:
            enc, sat, nv, ncl = float(m.group(1)), float(m.group(2)), int(m.group(4)), int(m.group(5))
        bw_res, bw_ms, _ = run([str(BWZ), str(fx)], timeout=args.timeout)
        rows.append((bb_ms, bb_res, bw_ms, bw_res, enc, sat, ncl, nv, rel))
        print(f"dv_bb={bb_ms:8.1f}({bb_res:<7}) bwz={bw_ms:8.1f}({bw_res:<7}) "
              f"enc={enc if enc is None else round(enc,1)} sat={sat if sat is None else round(sat,1)} cl={ncl} {rel}", flush=True)

    # medium band: dv bitblast solved (sat/unsat) in 1000-6000ms
    print("\n== MEDIUM BAND: dv bitblast solved in 1-6s ==")
    for r in sorted(rows):
        bb_ms, bb_res, bw_ms, bw_res, enc, sat, ncl, nv, rel = r
        if bb_res in ("sat", "unsat") and 1000.0 <= bb_ms <= 6000.0:
            print(f"  dv={bb_ms:7.1f}({bb_res}) bwz={bw_ms:7.1f}({bw_res}) enc={round(enc,1) if enc else enc} sat={round(sat,1) if sat else sat} cl={ncl} {rel}")

    import csv
    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["dv_bb_ms","dv_bb_res","bwz_ms","bwz_res","enc_ms","sat_ms","clauses","vars","bench"])
        for r in sorted(rows):
            w.writerow(r)
    print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
