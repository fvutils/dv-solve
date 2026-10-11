"""The "most capable" scorecard of a SAT suite run (docs/fw_hdl_implementation_plan.md §3.1).

    python3 -m tests.perf.scorecard perf-out/perf-manual-....json.gz \\
        [--suite fwhdl-core] [--out docs/fwhdl_scorecard.md]

Reads one run record (tests/perf/collect.py) and writes Markdown: per
category, and over the whole suite,

- soundness (gate): answers contradicting the expected status, per solver;
- solved and PAR-2 at each budget up to the run's (1 s, 60 s, 600 s): an
  instance counts as solved at budget b when the answer is right and its
  CPU time is at most b; PAR-2 adds 2b for each one that is not;
- the virtual best solver (VBS) over the competitors, and the VBS gap:
  dv-solve's PAR-2 over the VBS's, 1.0 when dv-solve matches the best
  competitor on every instance;
- the instances some competitor solves and dv-solve does not;
- scaling: per family, the slope of log time against log knob value;
- overhead: start-up (one-variable problem) and peak memory.

The headline claim for a category holds when no answer is wrong, dv-solve
solves every instance some competitor solves, its PAR-2 is at most the best
single competitor's, and the VBS gap is at most 1.2.

The expected status of an instance is the manifest's (from the generator's
index), else the file's `(set-info :status ...)`, else the competitors'
agreed answer.
"""
from __future__ import annotations

import argparse
import gzip
import json
import math
import re
import statistics
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
HEAD = "dv-smt2@head"
BUDGETS = (1, 60, 600)
GAP_OK = 1.2
_STATUS = re.compile(r"\(\s*set-info\s+:status\s+(sat|unsat)\s*\)")


def _arm(r):
    return f"{r['arm']}@{r['build']}"


def _name(k):
    return {"dv-smt2@head": "dv-solve", "dv-smt2-bb@head": "dv-solve bit-blast",
            "dv-smt2@anchor": "dv-solve anchor"}.get(k, k.split("@")[0])


class Data:
    def __init__(self, rec, suite):
        self.rec, self.suite = rec, suite
        man = rec["manifests"][suite]
        self.fixtures = man["fixtures"]
        self.budget = rec["budgets"][suite]
        self.rows = {}
        for r in rec["sat"]:
            if r["suite"] == suite:
                self.rows[(r["fixture"], _arm(r))] = r
        self.arms = sorted({_arm(r) for r in rec["sat"] if r["suite"] == suite},
                           key=lambda k: (not k.endswith("@head"), k))
        self.refs = [k for k in self.arms if k.endswith("@ref")]
        self.status = {}
        for fx in self.fixtures:
            st = fx.get("status")
            if not st:
                p = _REPO / fx["path"]
                m = _STATUS.search(p.read_text(errors="replace")) if p.exists() else None
                st = m.group(1) if m else None
            if not st:
                ans = {self.rows[(fx["path"], k)]["verdict"] for k in self.refs
                       if (fx["path"], k) in self.rows} & {"sat", "unsat"}
                st = ans.pop() if len(ans) == 1 else None
            self.status[fx["path"]] = st
        self.meta = _index_meta()

    def time(self, fx, arm):
        """CPU seconds of a right answer, else None."""
        r = self.rows.get((fx, arm))
        if not r or r["verdict"] not in ("sat", "unsat") or r["cpu_ms_med"] is None:
            return None
        st = self.status.get(fx)
        if st and r["verdict"] != st:
            return None
        return r["cpu_ms_med"] / 1000

    def wrong(self, fx, arm):
        r = self.rows.get((fx, arm))
        st = self.status.get(fx)
        return bool(r and st and r["verdict"] in ("sat", "unsat") and r["verdict"] != st)

    def solved(self, fxs, arm, b):
        return sum(1 for f in fxs if (t := self.time(f, arm)) is not None and t <= b)

    def par2(self, fxs, arm, b):
        return sum(t if (t := self.time(f, arm)) is not None and t <= b else 2 * b for f in fxs)

    def vbs_par2(self, fxs, b):
        tot = 0.0
        for f in fxs:
            ts = [t for k in self.refs if (t := self.time(f, k)) is not None and t <= b]
            tot += min(ts) if ts else 2 * b
        return tot


def _index_meta():
    """path -> (family, knobs) from the generators' index files."""
    out = {}
    for ip in (_REPO / "tests/formal/smt2").glob("**/index.json"):
        try:
            for e in json.loads(ip.read_text())["instances"]:
                out[str((ip.parent / e["path"]).relative_to(_REPO))] = (e["family"], e["knobs"])
        except (KeyError, ValueError):
            continue
    return out


def _fmt_t(x):
    return "—" if x is None else (f"{x:.2f}" if x < 100 else f"{x:.0f}")


def _slope(points):
    """Least-squares slope of log t against log k; None with fewer than 3 points."""
    pts = [(math.log(k), math.log(max(t, 1e-3))) for k, t in points if k > 0]
    if len(pts) < 3 or len({p[0] for p in pts}) < 2:
        return None
    mx = statistics.mean(p[0] for p in pts)
    my = statistics.mean(p[1] for p in pts)
    den = sum((p[0] - mx) ** 2 for p in pts)
    return sum((p[0] - mx) * (p[1] - my) for p in pts) / den if den else None


def render(rec, suite) -> str:
    d = Data(rec, suite)
    budgets = [b for b in BUDGETS if b <= d.budget] or [d.budget]
    cats = []
    for fx in d.fixtures:
        if fx["cat"] not in cats:
            cats.append(fx["cat"])
    by_cat = {c: [f["path"] for f in d.fixtures if f["cat"] == c] for c in cats}
    allfx = [f["path"] for f in d.fixtures]
    run, m, tools = rec["run"], rec["machine"], rec["tools"]
    L = [f"# {suite} scorecard", "",
         f"Run {run['utc']} on commit `{run['commit'][:9]}`, {m['cpu']} "
         f"({m['cores']} logical CPUs){', host busy' if m.get('noisy') else ''}. "
         f"Budget {d.budget:g} s per instance, CPU time, median of the repeats. "
         f"Competitors: " + ", ".join(f"{_name(k)} {tools.get(_name(k), '')}".strip() for k in d.refs)
         + ".", "",
         "Generated by `python -m tests.perf.scorecard`; the definitions are in "
         "`docs/fw_hdl_implementation_plan.md` §3.1.", ""]

    # headline
    L += ["## Headline", "",
          "| Category | Instances | Wrong answers (all solvers) | dv-solve solved | Best competitor solved | "
          f"Lost to a competitor | PAR-2 dv-solve / best single | VBS gap | Claim |",
          "|---|---|---|---|---|---|---|---|---|"]
    b = d.budget
    for c in cats + ["all"]:
        fxs = allfx if c == "all" else by_cat[c]
        wrong = {k: sum(d.wrong(f, k) for f in fxs) for k in d.arms}
        wrong_s = ", ".join(f"{_name(k)} {n}" for k, n in wrong.items() if n) or "0"
        head_s = d.solved(fxs, HEAD, b)
        best = min(d.refs, key=lambda k: d.par2(fxs, k, b)) if d.refs else None
        lost = [f for f in fxs if d.time(f, HEAD) is None or d.time(f, HEAD) > b
                if any((t := d.time(f, k)) is not None and t <= b for k in d.refs)]
        ph, pb = d.par2(fxs, HEAD, b), d.par2(fxs, best, b) if best else None
        vbs = d.vbs_par2(fxs, b)
        gap = ph / vbs if vbs else None
        claim = (not wrong.get(HEAD) and not lost and pb is not None and ph <= pb
                 and gap is not None and gap <= GAP_OK)
        L.append(f"| {c} | {len(fxs)} | {wrong_s} | {head_s} | "
                 f"{d.solved(fxs, best, b) if best else '—'} ({_name(best) if best else ''}) | "
                 f"{len(lost)} | {_fmt_t(ph)} / {_fmt_t(pb)} | "
                 f"{'—' if gap is None else f'{gap:.2f}'} | {'**yes**' if claim else 'no'} |")
    L.append("")

    # solved and PAR-2 per budget
    for c in cats + ["all"]:
        fxs = allfx if c == "all" else by_cat[c]
        L += [f"## {c} ({len(fxs)})", "",
              "| Solver | " + " | ".join(f"solved @{x:g}s | PAR-2 @{x:g}s" for x in budgets) + " |",
              "|---|" + "---|---|" * len(budgets)]
        for k in d.arms + ["VBS"]:
            cells = []
            for x in budgets:
                if k == "VBS":
                    s = sum(1 for f in fxs if any((t := d.time(f, r)) is not None and t <= x
                                                  for r in d.refs))
                    cells += [str(s), _fmt_t(d.vbs_par2(fxs, x))]
                else:
                    cells += [str(d.solved(fxs, k, x)), _fmt_t(d.par2(fxs, k, x))]
            L.append(f"| {'VBS of the competitors' if k == 'VBS' else _name(k)} | "
                     + " | ".join(cells) + " |")
        lost = [f for f in fxs if (d.time(f, HEAD) is None)
                and any(d.time(f, r) is not None for r in d.refs)]
        if lost and c != "all":
            L += ["", "Solved by a competitor, not by dv-solve: " + ", ".join(
                f"`{Path(f).stem}` ({', '.join(_name(r) for r in d.refs if d.time(f, r) is not None)})"
                for f in lost)]
        L.append("")

    # scaling
    fam = {}
    for f in allfx:
        if f in d.meta:
            name, knobs = d.meta[f]
            fam.setdefault(name, []).append((f, knobs))
    rows = []
    for name, items in sorted(fam.items()):
        keys = [k for k in items[0][1] if isinstance(items[0][1][k], int)]
        for key in keys:
            vals = {kn[key] for _, kn in items}
            if len(vals) < 3:
                continue
            rest = lambda kn: tuple((k, v) for k, v in sorted(kn.items()) if k != key)
            groups = {}
            for f, kn in items:
                groups.setdefault(rest(kn), []).append((kn[key], f))
            cells = []
            for k in [HEAD] + d.refs:
                slopes, top = [], None
                for g in groups.values():
                    pts = [(v, d.time(f, k)) for v, f in g if d.time(f, k) is not None]
                    s = _slope(pts)
                    if s is not None:
                        slopes.append(s)
                    if pts:
                        top = max(top or 0, max(v for v, _ in pts))
                cells.append(("—" if not slopes else f"{statistics.median(slopes):.1f}")
                             + f" (≤{top})" if top is not None else "none solved")
            rows.append(f"| {name} | {key} | " + " | ".join(cells) + " |")
    if rows:
        L += ["## Scaling", "",
              "Median over knob groups of the slope of log(CPU time) against log(knob): 1 is "
              "linear, 2 quadratic. In brackets: the largest knob value solved.", "",
              "| Family | Knob | " + " | ".join(_name(k) for k in [HEAD] + d.refs) + " |",
              "|---|---|" + "---|" * (1 + len(d.refs))] + rows + [""]

    # overhead
    fl = rec.get("sat_floor", {}).get(suite, {})
    trivial = by_cat.get("quickcheck-trivial", [])
    L += ["## Overhead", "",
          "| Solver | start-up ms (one-variable problem) | median CPU ms, quickcheck-trivial | "
          "peak RSS MB |", "|---|---|---|---|"]
    for k in d.arms:
        ts = [t * 1000 for f in trivial if (t := d.time(f, k)) is not None]
        rss = max((r.get("rss_kb") or 0) for (f, a), r in d.rows.items() if a == k)
        L.append(f"| {_name(k)} | {_fmt_t(fl.get(k))} | "
                 f"{_fmt_t(statistics.median(ts)) if ts else '—'} | {rss / 1024:.0f} |")
    L.append("")
    dis = rec.get("sat_disagree", {}).get(suite, [])
    if dis:
        L += ["## Disagreements", ""] + [f"- `{x['fixture']}`: {x['kind']}: {x['answers']}"
                                         for x in dis] + [""]
    return "\n".join(L)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("record")
    ap.add_argument("--suite", default="fwhdl-core")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    raw = Path(a.record).read_bytes()
    rec = json.loads(gzip.decompress(raw) if a.record.endswith(".gz") else raw)
    suite = a.suite if a.suite in rec["manifests"] else next(
        (s for s in rec["manifests"] if s.startswith(a.suite)), a.suite)
    md = render(rec, suite)
    if a.out:
        Path(a.out).write_text(md + "\n")
    else:
        print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
