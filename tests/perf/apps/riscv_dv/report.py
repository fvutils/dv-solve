"""Markdown summary of the riscv-dv suite in a run record.

    python3 -m tests.perf.apps.riscv_dv.report perf-out/perf-manual-....json.gz [--out FILE]

Until the results pages render the `app` family (once Verilator releases
+verilator+rand+sampler+), this is how a nightly record is read: performance
and diversity side by side, then every cell.
"""
from __future__ import annotations

import argparse
import gzip
import json
import sys
from pathlib import Path


def _f(x, fmt="{:.2f}", none="—"):
    return none if x is None else fmt.format(x)


def render(rec: dict) -> str:
    out = []
    for name, s in rec.get("app_summary", {}).items():
        man = rec["manifests"][name]
        inp = man["inputs"]
        ref = s["reference"]
        arms = list(s["arms"])
        out.append(f"# {name}: {rec['run']['utc']}\n")
        out.append(f"- **Valid:** {rec['valid']}" + (f" ({rec.get('reason')})" if not rec["valid"] else ""))
        out.append(f"- **dv-solve:** `{rec['run']['commit'][:10]}`")
        v = inp["verilator"]
        out.append(f"- **Verilator:** {v['version']}, commit `{v['commit'][:10]}`"
                   + (f" + uncommitted changes `{v['dirty']}`" if v.get("dirty") else ""))
        out.append(f"- **riscv-dv:** `{inp['riscv_dv']['commit'][:10]}`; patches: "
                   + ", ".join(f"`{p}`" for p in inp["patches"]))
        out.append(f"- **Machine:** {rec['machine']['cpu']}, load "
                   f"{rec['machine']['loadavg_start']}→{rec['machine']['loadavg_end']}"
                   + (" (**noisy**)" if rec["machine"]["noisy"] else ""))
        out.append(f"- **Manifest:** `{man['hash']}`" + (" (**subset of cells**)" if man.get("subset") else ""))
        if s.get("degraded"):
            out.append(f"- **Degraded:** value spread below {ref}: {', '.join(s['degraded'])}")
        out.append("")

        out.append("## Performance\n")
        out.append("| arm | cells passed | wall (s) | solver CPU (s) | simulator CPU (s) | "
                   f"end-to-end speed-up vs {ref} | solver speed-up vs {ref} |")
        out.append("|---|---:|---:|---:|---:|---:|---:|")
        for a in arms:
            x, r = s["arms"][a], s["ratios"].get(a, {})
            out.append(f"| {a} | {x['pass']}/{x['cells']} | {x['wall_s']:.1f} | {x['solver_cpu_s']:.1f} | "
                       f"{x['sim_cpu_s']:.1f} | {_f(r.get('e2e_geomean'), '{:.1f}x')} | "
                       f"{_f(r.get('cpu_geomean'), '{:.1f}x')} |")
        out.append("\nSpeed-ups are geometric means over the cells both arms passed.")
        noisy = {a: s["arms"][a].get("noisy_cells", 0) for a in arms}
        if any(noisy.values()):
            out.append("**Noisy:** host load above 2 at the start of a run, for "
                       + ", ".join(f"{a} on {n} cell(s)" for a, n in noisy.items() if n)
                       + ". CPU times there are inflated by contention (shared caches, SMT "
                       "siblings, lower boost), so treat those cells' ratios with care.")
        out.append("")

        out.append("## Diversity\n")
        out.append("| arm | value H/Hmax | value mode% | value JS vs ref | pairs | program dup% | "
                   "register H | mnemonic H | imm u% | mnemonic JS vs ref | deterministic |")
        out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|")
        for a in arms:
            x = s["arms"][a]
            vv, pp = x.get("values") or {}, x["program"]
            out.append(f"| {a} | {_f(vv.get('hhmax'), '{:.3f}')} | {_f(vv.get('mode'), '{:.1f}')} | "
                       f"{_f(vv.get('js_ref'), '{:.3f}')} | {vv.get('pairs', '—')} | {_f(pp['dup'], '{:.1f}')} | "
                       f"{_f(pp['reg_h'], '{:.3f}')} | {_f(pp['mnem_h'], '{:.3f}')} | {_f(pp.get('imm_u'), '{:.1f}')} | "
                       f"{_f(pp.get('js_ref'), '{:.4f}')} | {'yes' if x['deterministic'] else '**no**'} |")
        out.append("\nValues: the last model of each randomize(), per call site and variable, "
                   "over the pairs every arm drew at least 50 times. H/Hmax is relative to the union "
                   "of the values the arms produced. Programs: the generated `main` and `sub_*` "
                   "bodies, pooled over the cells.\n")

        out.append("## Cells\n")
        out.append("| cell | " + " | ".join(f"{a}: outcome, wall, solver CPU" for a in arms) + " |")
        out.append("|---|" + "---|" * len(arms))
        rows = {(r["target"], r["test"], r["arm"]): r for r in rec["app"] if r["suite"] == name}
        for c in man["cells"]:
            cells = []
            for a in arms:
                r = rows.get((c["target"], c["test"], a))
                if not r:
                    cells.append("—")
                    continue
                o = r["outcome"] if r["outcome"] == "pass" else f"**{r['outcome']}** ({r.get('signature')})"
                cells.append(f"{o}, {r['wall_s']:.1f}, {r['solver_cpu_s']:.2f}"
                             + (" (noisy)" if r.get("noisy") else ""))
            out.append(f"| {c['target']}/{c['test'].replace('riscv_', '')} | " + " | ".join(cells) + " |")
        out.append("")
    return "\n".join(out) if out else "no app suites in this record\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("record")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    op = gzip.open if a.record.endswith(".gz") else open
    with op(a.record, "rt") as f:
        rec = json.load(f)
    text = render(rec)
    if a.out:
        Path(a.out).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
