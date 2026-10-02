"""Render the docs "Results" pages from perf data (design §7).

    python3 -m tests.perf.render [--records DIR] [--out docs/site/results]

Inputs: the committed history (tests/perf/history/) and any full run records
in --records (fetched artifacts: `python3 -m tests.perf.history fetch`). The
"current" numbers come from the newest valid run record that has SAT data.
Writes index.md, sat.md and randomization.md plus _gen/*.svg and _gen/*.csv
(randomization comes from the newest valid record with rand-core data, which
may be a different run); methodology.md is
hand-written and tracked. With no SAT record at all, writes placeholder pages
so the site still builds.
"""
from __future__ import annotations

import argparse
import csv
import gzip
import io
import json
import math
import sys
from pathlib import Path

from . import history, normalize, svg

_REPO = Path(__file__).resolve().parents[2]
SUITE = "sat-core"
REPO_URL = "https://github.com/fvutils/dv-solve"
HEAD = normalize.HEAD
# arm key -> (display name, tool key for the version)
ARMS = {
    "dv-smt2@head": ("dv-solve", None),
    "dv-smt2-bb@head": ("dv-solve (bit-blast)", None),
    "dv-smt2@anchor": ("dv-solve {anchor}", None),
    "bitwuzla@ref": ("bitwuzla {v}", "bitwuzla"),
    "z3@ref": ("z3 {v}", "z3"),
    "boolector@ref": ("boolector {v}", "boolector"),
}
COMPARE = ["bitwuzla@ref", "z3@ref", "dv-smt2@anchor"]


def _name(rec: dict, key: str) -> str:
    fmt, tool = ARMS.get(key, (key, None))
    return fmt.format(anchor=(rec.get("anchor") or {}).get("tag", "anchor"),
                      v=rec["tools"].get(tool, "") if tool else "").strip()


def load_records(d) -> list:
    out = []
    if d:
        for p in sorted(Path(d).glob("*.json.gz")):
            out.append(json.loads(gzip.decompress(p.read_bytes())))
    for p in sorted((history.HISTORY / "releases").glob("*.json.gz")):
        out.append(json.loads(gzip.decompress(p.read_bytes())))
    return out


def current(records: list):
    ok = [r for r in records if r.get("valid") and any(s["suite"] == SUITE for s in r.get("sat", []))]
    return max(ok, key=lambda r: r["run"]["utc"]) if ok else None


def _when(utc: str) -> str:
    return f"{utc[0:4]}-{utc[4:6]}-{utc[6:8]} {utc[9:11]}:{utc[11:13]} UTC"


def provenance(rec: dict) -> str:
    run, m, t = rec["run"], rec["machine"], rec["tools"]
    sha = run["commit"]
    lines = [
        "```{list-table}",
        ":widths: 25 75",
        "",
        f"* - Measured\n  - {_when(run['utc'])}, {rec['kind']} run"
        + (" (host was busy: treat timings with caution)" if m["noisy"] else ""),
        f"* - dv-solve\n  - commit [`{sha[:9]}`]({REPO_URL}/commit/{sha})",
        f"* - Baseline build\n  - dv-solve {(rec.get('anchor') or {}).get('tag', 'none')}, built from its tag",
        "* - Reference solvers\n  - " + ", ".join(f"{k} {t[k]}" for k in ("bitwuzla", "z3", "boolector") if t.get(k)),
        f"* - Machine\n  - {m['cpu']}, {m['cores']} logical CPUs, {m['mem_gb']} GB, Linux {m['kernel']}",
        "```",
    ]
    return "\n".join(lines)


def _cat_score(rat, fixtures, cat):
    xs = [r for r, f in zip(rat, fixtures) if r is not None and f["cat"] == cat]
    return (math.exp(sum(xs) / len(xs) / 1000), len(xs)) if xs else (None, 0)


def _factor(x) -> str:
    if x is None:
        return "–"
    return f"{x:.2f}×" if x < 10 else f"{x:.0f}×"


def _ms(x) -> str:
    if x is None:
        return "–"
    return f"{x:.1f}" if x < 100 else f"{x:.0f}"


def summary(rec: dict) -> dict:
    man = rec["manifests"][SUITE]
    out = {"n": len(man["fixtures"]), "solved": normalize.solved(rec, SUITE),
           "floor": rec.get("sat_floor", {}).get(SUITE, {}), "scores": {}}
    for k in COMPARE:
        rat = normalize.ratios(rec, SUITE, k)
        out["scores"][k] = (normalize.score(rat, man["fixtures"], man["weights"]),
                            sum(r is not None for r in rat))
    dis = rec.get("sat_disagree", {}).get(SUITE, [])
    out["wrong_head"] = [d for d in dis if d["kind"].endswith("@head")]
    out["wrong_anchor"] = [d for d in dis if d["kind"] == "dv-smt2@anchor"]
    out["ref_split"] = [d for d in dis if d["kind"] == "references"]
    return out


RAND_PLACEHOLDER = """# Randomization

No performance run with randomization results has been recorded yet. This
page fills in after the next nightly performance run; see {doc}`methodology`.
"""

PLACEHOLDER = """# {title}

No performance run with SAT results has been recorded yet. This page fills in
after the first nightly performance run; see {{doc}}`methodology` for what
it will show and how the numbers are produced.
"""


def page_index(rec: dict, s: dict, rrec=None, rs=None) -> str:
    L = ["# Results", "",
         "How dv-solve compares with the reference solvers bitwuzla and z3 on "
         "the same machine in the same run: as an SMT-LIB2 solver, and as the "
         "constraint solver behind randomize(). The numbers are "
         "regenerated from the latest nightly performance run; "
         "{doc}`methodology` explains how each one is produced.", "",
         provenance(rec), "",
         "## SMT-LIB2 solving at a glance", ""]
    L += [f"| Solver | Answered correctly (of {s['n']}) | Start-up, CPU ms | dv-solve speed-up | Wrong answers |",
          "|---|---|---|---|---|"]
    wrong = {HEAD: len(s["wrong_head"]), "dv-smt2@anchor": len(s["wrong_anchor"])}
    for k in [HEAD] + COMPARE:
        f, n = s["scores"].get(k, (None, 0))
        sp = "–" if k == HEAD else (f"{_factor(f)} ({n} fixtures)" if f else "–")
        L.append(f"| {_name(rec, k)} | {s['solved'].get(k, '–')} | {_ms(s['floor'].get(k))} | {sp} | "
                 f"{wrong.get(k, '–')} |")
    L += ["",
          "Speed-up is a weighted geometric mean; above 1× means dv-solve solved "
          "faster. It compares time spent "
          "beyond each solver's own start-up cost, over the fixtures that both "
          "solvers answered and that take at least a millisecond beyond start-up; "
          "most fixtures in this suite are quicker than that, which is why "
          "start-up cost is shown separately. Every dv-solve `sat` or `unsat` is "
          "checked against the reference solvers in the same run, and a wrong "
          "answer is one they contradict; those of the old release are bugs "
          "fixed since. "
          f"dv-solve answers `unknown` on {s['n'] - s['solved'].get(HEAD, 0)} "
          "fixtures that use constructs it does not yet decide "
          "({doc}`../concepts/soundness`).", "",
          "Details per category and per fixture: {doc}`sat`.", ""]
    if rrec:
        L += ["## Randomization at a glance", ""] + rand_glance(rrec, rs)
        L += ["",
              "The Verilator rows drive each solver with Verilator's own randomize() "
              "protocol. Excess JSD measures how far the solutions are from a true "
              "random choice, beyond the sampling noise of a true random sample; "
              "0 is ideal."
              + (f" Under Verilator, dv-solve is {_factor(rs['speedup'])} faster "
                 "per randomize() than z3, Verilator's default solver."
                 if rs["speedup"] else ""), "",
              "Details per benchmark, with the distributions: {doc}`randomization`.", ""]
    L += ["```{toctree}", ":hidden:", "", "sat", "randomization", "methodology", "```", ""]
    return "\n".join(L)


def page_sat(rec: dict, s: dict, gen: Path) -> str:
    man = rec["manifests"][SUITE]
    rows = [r for r in rec["sat"] if r["suite"] == SUITE]
    keys = [k for k in ARMS if any(f"{r['arm']}@{r['build']}" == k for r in rows)]
    by = {(r["fixture"], f"{r['arm']}@{r['build']}"): r for r in rows}
    cons = normalize.consensus(rows, SUITE)
    ok = lambda r: normalize._correct(r, cons)

    # Cactus: definite answers only, raw CPU time (start-up included, as a user sees it).
    series = {}
    for k in keys:
        ts = sorted(r["cpu_ms_min"] for r in rows if f"{r['arm']}@{r['build']}" == k and ok(r))
        series[_name(rec, k)] = ts
    (gen / "sat-cactus.svg").write_text(svg.cactus(series, "CPU time per fixture, ms (log scale)",
                                                   "fixtures answered"))
    rat_b = normalize.ratios(rec, SUITE, "bitwuzla@ref")
    groups = {}
    for r, fx in zip(rat_b, man["fixtures"]):
        groups.setdefault(fx["cat"], [])
        if r is not None:
            groups[fx["cat"]].append(math.exp(r / 1000))
    (gen / "sat-vs-bitwuzla.svg").write_text(
        svg.strip({c: v for c, v in groups.items() if v},
                  "dv-solve speed-up (log scale)", _name(rec, "bitwuzla@ref")))

    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["fixture", "category", "arm", "verdict", "cpu_ms_min", "cpu_ms_med", "wall_ms_min", "reps"])
    for r in rows:
        w.writerow([r["fixture"], r["cat"], _name(rec, f"{r['arm']}@{r['build']}"), r["verdict"],
                    r["cpu_ms_min"], r["cpu_ms_med"], r["wall_ms_min"], r["reps"]])
    (gen / "sat-core.csv").write_text(buf.getvalue())

    L = ["# SMT-LIB2 solving", "",
         f"Each of the {s['n']} fixtures of the `{SUITE}` suite is solved by every "
         "solver several times in alternation, and the minimum CPU time is kept. "
         "boolector runs only on the QF_BV fixtures, since it cannot read the "
         "others' dialect.", "",
         provenance(rec), "",
         "## Fixtures answered within a time", "",
         "Each line counts the fixtures a solver answered correctly within "
         "the CPU time on the x axis, start-up included; further right is slower, "
         "higher is more answered.", "",
         "![Fixtures answered against CPU time](_gen/sat-cactus.svg)", "",
         f"## Speed-up against {_name(rec, 'bitwuzla@ref')}", "",
         "One dot per fixture that both solvers answered and that takes at least a "
         "millisecond beyond start-up; above the dashed line dv-solve is faster.", "",
         "![dv-solve speed-up against bitwuzla, by category](_gen/sat-vs-bitwuzla.svg)", "",
         "## By category", ""]
    cats = list(dict.fromkeys(f["cat"] for f in man["fixtures"]))
    short = {HEAD: "dv-solve", "bitwuzla@ref": "bitwuzla", "z3@ref": "z3",
             "dv-smt2@anchor": (rec.get("anchor") or {}).get("tag", "anchor")}
    for c in cats:
        L.append(f"- **{c}** (weight {man['weights'].get(c, 1.0):g}): {man.get('about', {}).get(c, '')}")
    L += ["", "Fixtures answered correctly (`sat` or `unsat`, agreeing with the references):", "",
          "| Category | Fixtures | " + " | ".join(short[k] for k in [HEAD] + COMPARE) + " |",
          "|---|---|" + "---|" * (1 + len(COMPARE))]
    for c in cats:
        fxs = [f for f in man["fixtures"] if f["cat"] == c]
        ans = [str(sum(1 for f in fxs if ok(by.get((f["path"], k))))) for k in [HEAD] + COMPARE]
        L.append(f"| {c} | {len(fxs)} | {' | '.join(ans)} |")
    rats = {k: normalize.ratios(rec, SUITE, k) for k in COMPARE}
    L += ["", "dv-solve speed-up against each solver, as a geometric mean over the "
          "fixtures counted in brackets:", "",
          "| Category | " + " | ".join(short[k] for k in COMPARE) + " |",
          "|---|" + "---|" * len(COMPARE)]
    for c in cats:
        sc = []
        for k in COMPARE:
            f, n = _cat_score(rats[k], man["fixtures"], c)
            sc.append(f"{_factor(f)} ({n})" if f else "–")
        L.append(f"| {c} | {' | '.join(sc)} |")
    L += ["",
          "## Start-up cost", "",
          "CPU time to answer a one-variable problem: the cost of starting the "
          "process and reading the input, paid once per call.", "",
          "| Solver | CPU ms |", "|---|---|"]
    for k in keys:
        L.append(f"| {_name(rec, k)} | {_ms(s['floor'].get(k))} |")

    L += ["", "## Disagreements", ""]
    if not (s["wrong_head"] or s["wrong_anchor"] or s["ref_split"]):
        L.append("None: every definite answer agrees with the reference solvers.")
    else:
        if not s["wrong_head"]:
            L.append("The current dv-solve agrees with the reference solvers on every fixture.")
        for title, items in (("Current dv-solve", s["wrong_head"]),
                             (f"dv-solve {(rec.get('anchor') or {}).get('tag', '')} "
                              "(old release; these are bugs fixed since)", s["wrong_anchor"]),
                             ("Reference solvers disagreeing with each other", s["ref_split"])):
            if items:
                L += ["", f"**{title}**", ""]
                for d in items:
                    ans = ", ".join(f"{_name(rec, k) if '@' in k else k}: {v}" for k, v in d["answers"].items())
                    L.append(f"- `{Path(d['fixture']).name}`: {ans}")
    unknown = [f for f in man["fixtures"] if (by.get((f["path"], HEAD)) or {}).get("verdict") == "unknown"]
    L += ["", "## Fixtures dv-solve answers `unknown`", "",
          f"{len(unknown)} fixtures use constructs dv-solve does not yet decide, so it "
          "answers `unknown` rather than guess ({doc}`../concepts/soundness`):", ""]
    L.append(", ".join(f"`{Path(f['path']).stem}`" for f in unknown) + "." if unknown else "None.")

    L += ["", "## Every fixture", "",
          "CPU ms, minimum over the repetitions; `–` is no definite answer "
          "(`unknown`, timeout or error); `wrong` is an answer the reference "
          "solvers contradict. "
          "Download: {download}`all measurements as CSV <_gen/sat-core.csv>`.", "",
          "| Fixture | Category | Answer | " + " | ".join(_name(rec, k) for k in keys) + " |",
          "|---|---|---|" + "---|" * len(keys)]
    for f in man["fixtures"]:
        h = by.get((f["path"], HEAD)) or {}
        cells = []
        for k in keys:
            r = by.get((f["path"], k))
            cells.append(_ms(r["cpu_ms_min"]) if ok(r) else ("wrong" if r and r["verdict"] in ("sat", "unsat") else "–"))
        L.append(f"| `{Path(f['path']).stem}` | {f['cat']} | {h.get('verdict', '–')} | {' | '.join(cells)} |")
    L.append("")
    return "\n".join(L)


# ---- randomization ----------------------------------------------------------

RAND = "rand-core"
# arm -> display name; the order is the table and legend order.
RARMS = {
    "uniform": "true random (ideal)",
    "dv-api@head": "dv-solve API",
    "dv-swizzle@head": "Verilator + dv-solve",
    "dv-swizzle-nohash@head": "Verilator + dv-solve, parity ignored",
    "z3-swizzle": "Verilator + z3 {z3}",
    "bitwuzla-swizzle": "Verilator + bitwuzla {bitwuzla}",
    "dv-swizzle@anchor": "Verilator + dv-solve {anchor}",
}
RREF = "z3-swizzle"          # what a Verilator user gets by default


def _rname(rec: dict, arm: str) -> str:
    t = rec["tools"]
    return RARMS.get(arm, arm).format(z3=t.get("z3", ""), bitwuzla=t.get("bitwuzla", ""),
                                      anchor=(rec.get("anchor") or {}).get("tag", "anchor"))


def current_rand(records: list):
    ok = [r for r in records if r.get("valid") and r.get("rand")]
    return max(ok, key=lambda r: r["run"]["utc"]) if ok else None


def rand_summary(rec: dict) -> dict:
    """Per arm: geometric-mean CPU ms per call and mean excess JSD, over the
    benchmarks every arm answered; per benchmark: rows by arm."""
    rows = [r for r in rec["rand"] if "error" not in r]
    by = {(r["bench"], r["arm"]): r for r in rows}
    benches = [b["name"] for b in rec["manifests"][RAND]["benches"]]
    arms = [a for a in RARMS if any(r["arm"] == a for r in rec["rand"])]
    xjsd = {}
    for b in benches:
        u = by.get((b, "uniform"))
        for a in arms:
            r = by.get((b, a))
            if u and r:
                xjsd[(b, a)] = max(0.0, r["jsd"] - u["jsd"])
    out = {"benches": benches, "arms": arms, "by": by, "xjsd": xjsd, "arm": {}}
    for a in arms:
        got = [b for b in benches if (b, a) in by]
        cpu = [by[(b, a)]["cpu_ms"] for b in got if by[(b, a)].get("cpu_ms")]
        out["arm"][a] = {
            "n": len(got),
            "cpu_ms": math.exp(sum(math.log(c) for c in cpu) / len(cpu)) if cpu else None,
            "xjsd": sum(xjsd[(b, a)] for b in got) / len(got) if got else None,
            "errors": sum(1 for r in rec["rand"] if r["arm"] == a and "error" in r),
            "bad": sum(by[(b, a)].get("bad", 0) for b in got),
        }
    ref = out["arm"].get(RREF, {}).get("cpu_ms")
    head = out["arm"].get("dv-swizzle@head", {}).get("cpu_ms")
    out["speedup"] = ref / head if ref and head else None
    return out


def _x(v) -> str:
    return "–" if v is None else f"{v:.3f}"


def _cpu(v) -> str:
    if v is None:
        return "–"
    return f"{v * 1000:.0f} µs" if v < 0.1 else (f"{v:.2f} ms" if v < 10 else f"{v:.0f} ms")


def rand_glance(rec: dict, rs: dict) -> list:
    L = ["| Randomization | CPU per randomize() | Excess JSD (0 is ideal) |", "|---|---|---|"]
    for a in rs["arms"]:
        if a == "uniform":
            continue
        s = rs["arm"][a]
        L.append(f"| {_rname(rec, a)} | {_cpu(s['cpu_ms'])} | {_x(s['xjsd'])} |")
    return L


def page_rand(rec: dict, rs: dict, gen: Path) -> str:
    man = rec["manifests"][RAND]
    benches = {b["name"]: b for b in man["benches"]}
    by, arms = rs["by"], rs["arms"]

    pts = [(_rname(rec, a), by[(b, a)]["cpu_ms"], rs["xjsd"][(b, a)])
           for b in rs["benches"] for a in arms
           if a != "uniform" and (b, a) in by and by[(b, a)].get("cpu_ms")]
    (gen / "rand-quality-cost.svg").write_text(svg.scatter(
        pts, "CPU per randomize(), ms (log scale)", "excess JSD (lower is better)"))
    panels = {b: {_rname(rec, a): by[(b, a)]["hist"] for a in arms if (b, a) in by}
              for b in rs["benches"]}
    (gen / "rand-histograms.svg").write_text(svg.small_multiples(panels))
    cost = {b: {_rname(rec, a): by[(b, a)].get("cpu_ms") for a in arms
                if a != "uniform" and (b, a) in by} for b in rs["benches"]}
    (gen / "rand-cost.svg").write_text(svg.dotplot(cost, "CPU per randomize(), ms (log scale)"))

    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["benchmark", "arm", "n", "coverage", "jsd", "excess_jsd", "chi2_p", "thin",
                "distinct", "cpu_ms_per_call", "wall_ms_p50", "wall_ms_p95", "wall_ms_p99",
                "check_sats_per_call", "bad_models"])
    for r in rec["rand"]:
        if "error" in r:
            w.writerow([r["bench"], _rname(rec, r["arm"]), "error: " + r["error"]])
            continue
        w.writerow([r["bench"], _rname(rec, r["arm"]), r["n"], r["cov"], r["jsd"],
                    rs["xjsd"].get((r["bench"], r["arm"])), r["chi2p"], r.get("thin"),
                    r["distinct"], r.get("cpu_ms"), r.get("p50_ms"), r.get("p95_ms"),
                    r.get("p99_ms"), r.get("checks"), r["bad"]])
    (gen / "rand-core.csv").write_text(buf.getvalue())

    n = man["benches"][0]["n"] if man["benches"] else 0
    sp = rs["speedup"]
    L = ["# Randomization", "",
         f"Each benchmark is randomized {n} times by every solver, and the "
         "solutions are compared with the exact distribution of a true random "
         "choice among all solutions. The Verilator rows use Verilator "
         f"{man['protocol'].split('-')[-1]}'s own randomize() protocol (below), "
         "with the solver it would run; the dv-solve API row is dv-solve called "
         "directly, as from SystemVerilog DPI or zuspec.", "",
         provenance(rec), "",
         "## At a glance", ""]
    L += rand_glance(rec, rs)
    L += ["",
          "CPU per call is a geometric mean over the benchmarks; for the "
          "Verilator rows it is the solver process's CPU time, which covers the "
          "five solver queries each call makes, and for the API row the time "
          "inside the solve call. Excess JSD is the distance from the ideal "
          "distribution beyond what a true random sample of the same size "
          "shows, averaged over the benchmarks: 0 is as good as true random."
          + (f" Under Verilator, dv-solve is {_factor(sp)} faster per "
             "randomize() than z3, Verilator's default solver." if sp else ""), "",
          "## Quality against cost", "",
          "One dot per benchmark and solver. Further left is cheaper, lower is "
          "closer to true random.", "",
          "![Excess JSD against CPU per randomize()](_gen/rand-quality-cost.svg)", "",
          "## How often each solution comes up", "",
          "For each benchmark, every solution (or bin of solutions, for the two "
          "with 32-bit fields) sorted from most to least frequent, as a multiple "
          "of how often a true random choice would pick it. The ideal is a flat "
          "line at 1; a sampler that keeps returning the same few solutions "
          "starts high and drops to the floor, which stands for never.", "",
          "![Solution frequency, sorted](_gen/rand-histograms.svg)", "",
          "## Cost per randomize()", "",
          "Per benchmark, the CPU each solver spends on one randomize().", "",
          "![CPU per randomize() by solver](_gen/rand-cost.svg)", "",
          "## The benchmarks", ""]
    for b in rs["benches"]:
        L.append(f"- **{b}**: {benches[b]['about']}")
    L += ["", "## Every benchmark", "",
          "Coverage is the share of solutions (or bins) returned at least once; "
          "a true random sample of the same size does not reach 100% when there "
          "are more solutions than calls. Thin is how often the rarest branch "
          "comes up relative to its share (1 is ideal; see the benchmark list). "
          "Download: {download}`all measurements as CSV <_gen/rand-core.csv>`.", ""]
    for b in rs["benches"]:
        L += [f"### {b}", "",
              "| Solver | Coverage | Excess JSD | Thin | CPU per call | Wall p50 / p99 |",
              "|---|---|---|---|---|---|"]
        for a in arms:
            r = by.get((b, a))
            if r is None:
                err = next((x["error"] for x in rec["rand"] if x["bench"] == b and x["arm"] == a
                            and "error" in x), None)
                if err:
                    L.append(f"| {_rname(rec, a)} | failed: {err} | | | | |")
                continue
            th = "–" if r.get("thin") is None else f"{r['thin']:.2f}"
            wall = "–" if a == "uniform" else f"{r['p50_ms']:.2f} / {r['p99_ms']:.2f} ms"
            L.append(f"| {_rname(rec, a)} | {r['cov'] * 100:.0f}% | {_x(rs['xjsd'][(b, a)])} | "
                     f"{th} | {_cpu(r.get('cpu_ms')) if a != 'uniform' else '–'} | {wall} |")
        L.append("")
    L += ["## Verilator's randomize() protocol", "",
          "Verilator does not ask its solver for a random solution. For each "
          "randomize() it sends the constraints and asks for any solution; then, "
          "up to four times, it adds a random parity constraint over about half "
          "of all the random bits and asks again, keeping the last solution "
          "found. The solver's own choice of solution decides the rest. These "
          "measurements drive each solver with exactly that sequence of "
          "commands; a test compares it with a real Verilator "
          f"{man['protocol'].split('-')[-1]} simulation, command by command and "
          "in the distribution of the values it returns "
          "(`tests/unit/test_vlt_protocol.py`).", ""]
    return "\n".join(L)


def render(records_dir, out: Path) -> str:
    gen = out / "_gen"
    gen.mkdir(parents=True, exist_ok=True)
    records = load_records(records_dir)
    rec, rrec = current(records), current_rand(records)
    rs = rand_summary(rrec) if rrec else None
    if rrec:
        (out / "randomization.md").write_text(page_rand(rrec, rs, gen))
    else:
        (out / "randomization.md").write_text(RAND_PLACEHOLDER)
    if rec is None:
        (out / "index.md").write_text(PLACEHOLDER.format(title="Results") +
                                      "\n```{toctree}\n:hidden:\n\nsat\nrandomization\nmethodology\n```\n")
        (out / "sat.md").write_text(PLACEHOLDER.format(title="SMT-LIB2 solving"))
        return "placeholder"
    s = summary(rec)
    (out / "index.md").write_text(page_index(rec, s, rrec, rs))
    (out / "sat.md").write_text(page_sat(rec, s, gen))
    return rec["run"]["utc"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--records", help="directory of fetched run records (*.json.gz)")
    ap.add_argument("--out", default=str(_REPO / "docs/site/results"))
    ap.add_argument("--max-age-days", type=float, default=None,
                    help="fail if the newest SAT data is older than this")
    a = ap.parse_args(argv)
    what = render(a.records, Path(a.out))
    print(f"rendered results pages from {what}")
    if a.max_age_days is not None:
        newest = max([l["utc"] for l in history.committed_lines() if l.get("sat")]
                     + ([what] if what != "placeholder" else []), default=None)
        if newest is None:
            print("no SAT run recorded yet: placeholder pages")
        elif history._utc_age_days(newest) > a.max_age_days:
            print(f"::error::newest SAT results are from {newest}, older than "
                  f"{a.max_age_days:g} days: is the nightly perf job running?")
            return 1
        elif what == "placeholder":
            print("::error::SAT results are committed but no full run record could "
                  "be fetched to render them")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
