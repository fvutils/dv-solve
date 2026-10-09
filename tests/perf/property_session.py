"""Many properties per session (docs/fw_hdl_implementation_plan.md piece 6.1).

    python tests/perf/property_session.py   # env: BUDGET, SOLVERS, CASES

One design unrolled to its latency L, asserted once; one property per output
bit, "bit i of the output at step L equals bit i of the specification", named
by a Bool p_i. Three ways to ask all of them:
  assume  one session, (check-sat-assuming ((not p_i))) per property
  push    one session, (push 1) (assert (not p_i)) (check-sat) (pop 1)
  mono    one file per property
Every answer is unsat (the design is correct). Results:
docs/fw_hdl_property_session.md."""
import os, sys, time, subprocess
from pathlib import Path
R = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(R / "tests/formal"), str(R)]
import gen_codegen_equiv as C, gen_fwhdl as G
from tests.perf import tools
budget = float(os.environ.get("BUDGET", 120))
d_by = {d.name: d for d in [C._crc32(16), C._adder_tree(), C._rotl32(), C._mac16(), C._fir4()]}
solvers = os.environ.get("SOLVERS", "dv,bitwuzla,z3").split(",")
out_dir = R / "perf-out/property" / "-".join(solvers); out_dir.mkdir(parents=True, exist_ok=True)


def texts(d, L):
    mod, v = d.verilog(L)
    smt2 = C.yosys_smt2(tools.path("yosys"), mod, v)
    f, ins = d.spec
    spec_defs = "\n".join(G.define_fun(g) for g in G._funs_of(f(*[G.var(n, w) for n, w in d.inputs])))
    args = " ".join(f"(|{mod}_n {nm}| s0)" for nm in ins)
    W = f.w
    head = ["(set-option :produce-models true)", "(set-logic QF_BV)", smt2.rstrip(), spec_defs]
    head += [f"(declare-fun s{i} () |{mod}_s|)" for i in range(L + 1)]
    head += [f"(assert (|{mod}_t| s{i} s{i + 1}))" for i in range(L)]
    head += [f"(define-fun spec () (_ BitVec {W}) ({G._q(f.name)} {args}))",
             f"(define-fun got () (_ BitVec {W}) (|{mod}_n out| s{L}))"]
    head += [f"(declare-fun p{i} () Bool)\n(assert (= p{i} (= ((_ extract {i} {i}) got) "
             f"((_ extract {i} {i}) spec))))" for i in range(W)]
    assume = "\n".join(head + [f"(check-sat-assuming ((not p{i})))" for i in range(W)] + ["(exit)"])
    push = "\n".join(head + [f"(push 1)\n(assert (not p{i}))\n(check-sat)\n(pop 1)" for i in range(W)]
                     + ["(exit)"])
    mono = ["\n".join(head + [f"(assert (not p{i}))", "(check-sat)", "(exit)"]) for i in range(W)]
    return W, assume + "\n", push + "\n", [m + "\n" for m in mono]


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
    return {"z3": [p, "-smt2"], "bitwuzla": [p], "cvc5": [p, "-i"], "yices": [p, "--incremental"],
            "boolector": [p, "-i"]}[s] + [str(f)]


for spec in os.environ.get("CASES", "crc32x16:8,addtree8x16:3,rotl32:5,mac16:3,fir4:3").split(","):
    name, L = spec.split(":"); L = int(L); d = d_by[name]
    W, assume, push, mono = texts(d, L)
    exp = ["unsat"] * W
    fa = out_dir / f"{name}_l{L}_assume.smt2"; fa.write_text(assume)
    fp = out_dir / f"{name}_l{L}_push.smt2"; fp.write_text(push)
    for s in solvers:
        env = {"LD_LIBRARY_PATH": ""} if s == "dv" else {}
        va, ta = run(argv(s, fa), env)
        vp, tp = run(argv(s, fp), env)
        tm, vm = 0.0, []
        for i, m in enumerate(mono):
            fm = out_dir / f"{name}_l{L}_p{i}.smt2"; fm.write_text(m)
            v, t = run(argv(s, fm), env); tm += t; vm += v[:1]
        st = lambda v: "ok" if v == exp else f"{sum(x == 'unsat' for x in v)}/{W} {sorted(set(v))}"
        print(f"{name} L={L} W={W} {s:9s} assume {ta:7.2f}s {st(va):12s} push {tp:7.2f}s {st(vp):12s} "
              f"mono {tm:7.2f}s {st(vm)}", flush=True)
