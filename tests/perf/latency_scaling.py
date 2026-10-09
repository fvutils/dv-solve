"""Latency scaling (docs/fw_hdl_implementation_plan.md piece 5.3).

    python tests/perf/latency_scaling.py   # env: BUDGET, LATS, SOLVERS, ONLY, JOBS

The codegen-equiv designs (crc32x16 in place of crc32x4/x8) at latency 1..32,
ok and early, on dv-solve and the pinned competitors. Writes the instances to
perf-out/latency/inst/, a table to stdout and the raw results to
perf-out/latency/scale.json. Results: docs/fw_hdl_latency_scaling.md."""
import os, re, sys, json, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
R = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(R / "tests/formal"), str(R)]
import gen_codegen_equiv as C
from tests.perf import run_sat, tools
budget = float(os.environ.get("BUDGET", 60))
lats = [int(x) for x in os.environ.get("LATS", "1,2,4,8,12,16,24,32").split(",")]
solvers = os.environ.get("SOLVERS", "dv,bitwuzla,z3,yices,cvc5,boolector").split(",")
only = os.environ.get("ONLY")
designs = [C._crc32(16), C._adder_tree(), C._rotl32(), C._mac16(), C._fir4()]
inst_dir = R / "perf-out/latency/inst"; inst_dir.mkdir(parents=True, exist_ok=True)
files = []
for d in designs:
    for L in lats:
        f_ok = inst_dir / f"{d.name}_l{L}_ok.smt2"
        if not f_ok.exists():
            mod, v = d.verilog(L)
            smt2 = C.yosys_smt2(tools.path("yosys"), mod, v)
            for var in ("ok", "early"):
                (inst_dir / f"{d.name}_l{L}_{var}.smt2").write_text(C.Instance(d, L, var, smt2).text())
        for var in ("ok", "early"):
            files.append(inst_dir / f"{d.name}_l{L}_{var}.smt2")
if only: files = [f for f in files if re.search(only, f.name)]
env = {"LD_LIBRARY_PATH": ""}
def argv(s, f):
    if s == "dv": return [str(R / "build/dv-solve-smt2"), str(f)]
    p = str(tools.path(s))
    return {"z3": [p, "-smt2"], "bitwuzla": [p], "boolector": [p, "--smt2"],
            "cvc5": [p], "yices": [p]}[s] + [str(f)]
def one(job):
    f, s = job
    v, cpu, wall, rss = run_sat.run_once(argv(s, f), env if s == "dv" else {}, budget)
    return f, s, v, wall / 1000
jobs = [(f, s) for f in files for s in solvers]
res = {}
with ThreadPoolExecutor(int(os.environ.get("JOBS", 8))) as ex:
    for f, s, v, t in ex.map(one, jobs):
        res.setdefault(f.stem, {})[s] = (v, t)
print("%-28s " % "instance" + " ".join("%-16s" % s for s in solvers))
for f in files:
    r = res[f.stem]; exp = "unsat" if f.stem.endswith("_ok") else "sat"
    cells = []
    for s in solvers:
        v, t = r[s]
        mark = "" if v == exp else ("!" if v in ("sat", "unsat") else "-")
        cells.append("%-16s" % (f"{t:7.2f}{mark}" if v == exp else f"{v[:7]}{mark}"))
    print("%-28s " % f.stem + " ".join(cells), flush=True)
json.dump({k: {s: list(x) for s, x in v.items()} for k, v in res.items()},
          open(R / "perf-out/latency/scale.json", "w"), indent=1)
