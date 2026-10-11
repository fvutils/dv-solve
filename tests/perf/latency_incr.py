"""Incremental deepening (docs/fw_hdl_implementation_plan.md piece 5.3).

    python tests/perf/latency_incr.py   # env: BUDGET, K, SOLVERS, CASES

For a design at latency L, ask "does the output at step k equal the spec?" for
k = 1..K. mono = K separate files; incr = one session that asserts one more
transition per step and asks each query under push/pop. Expect sat except at
k == L. Results: docs/fw_hdl_latency_scaling.md."""
import os, re, sys, time, subprocess
from pathlib import Path
R = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(R / "tests/formal"), str(R)]
import gen_codegen_equiv as C, gen_fwhdl as G
from tests.perf import run_sat, tools
budget = float(os.environ.get("BUDGET", 120))
K = int(os.environ.get("K", 16))
d_by = {d.name: d for d in [C._crc32(16), C._adder_tree(), C._rotl32(), C._mac16(), C._fir4()]}
out_dir = R / "perf-out/latency/incr"; out_dir.mkdir(parents=True, exist_ok=True)
def texts(d, L):
    mod, v = d.verilog(L)
    smt2 = C.yosys_smt2(tools.path("yosys"), mod, v)
    f, ins = d.spec
    spec_defs = "\n".join(G.define_fun(g) for g in G._funs_of(f(*[G.var(n, w) for n, w in d.inputs])))
    args = " ".join(f"(|{mod}_n {nm}| s0)" for nm in ins)
    head = ["(set-option :produce-models true)", "(set-logic QF_BV)", smt2.rstrip(), spec_defs]
    q = lambda k: f"(assert (not (= (|{mod}_n out| s{k}) ({G._q(f.name)} {args}))))"
    mono = {}
    for k in range(1, K + 1):
        b = head + [f"(declare-fun s{i} () |{mod}_s|)" for i in range(k + 1)]
        b += [f"(assert (|{mod}_t| s{i} s{i + 1}))" for i in range(k)]
        mono[k] = "\n".join(b + [q(k), "(check-sat)", "(exit)"]) + "\n"
    b = head + ["(declare-fun s0 () |%s_s|)" % mod]
    for k in range(1, K + 1):
        b += [f"(declare-fun s{k} () |{mod}_s|)", f"(assert (|{mod}_t| s{k - 1} s{k}))",
              "(push 1)", q(k), "(check-sat)", "(pop 1)"]
    return mono, "\n".join(b + ["(exit)"]) + "\n"
def run(argv, env):
    t0 = time.monotonic()
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=budget, env={**os.environ, **env})
        vs = [l.strip() for l in p.stdout.splitlines() if l.strip() in ("sat", "unsat", "unknown")]
    except subprocess.TimeoutExpired:
        vs = ["timeout"]
    return vs, time.monotonic() - t0
def argv(s, f):
    if s == "dv": return [str(R / "build/dv-solve-smt2"), str(f)]
    p = str(tools.path(s))
    return {"z3": [p, "-smt2"], "bitwuzla": [p], "cvc5": [p, "-i"], "yices": [p, "--incremental"]}[s] + [str(f)]
solvers = os.environ.get("SOLVERS", "dv,bitwuzla").split(",")
for spec in os.environ.get("CASES", "crc32x16:8,addtree8x16:3,rotl32:5,mac16:3,fir4:3").split(","):
    name, L = spec.split(":"); L = int(L); d = d_by[name]
    mono, incr = texts(d, L)
    exp = ["unsat" if k == L else "sat" for k in range(1, K + 1)]
    fi = out_dir / f"{name}_l{L}_incr.smt2"; fi.write_text(incr)
    for s in solvers:
        env = {"LD_LIBRARY_PATH": ""} if s == "dv" else {}
        tm, vm = 0.0, []
        for k in range(1, K + 1):
            fm = out_dir / f"{name}_l{L}_k{k}.smt2"; fm.write_text(mono[k])
            v, t = run(argv(s, fm), env); tm += t; vm += v[:1]
        vi, ti = run(argv(s, fi), env)
        print(f"{name} L={L} {s:9s} mono {tm:7.2f}s {'ok' if vm == exp else vm}   "
              f"incr {ti:7.2f}s {'ok' if vi == exp else vi}", flush=True)
