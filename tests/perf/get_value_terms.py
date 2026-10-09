"""get-value over many terms after a fw-hdl `sat` (docs/fw_hdl_implementation_plan.md
piece 6.2).

    python tests/perf/get_value_terms.py   # env: CASE (default crc32x16:8)

One design at latency L, compared one step early (`sat`: a counterexample).
After the check-sat, the script asks for every `|<mod>_n <signal>|` at every
step: once in a single get-value, once with one get-value per term. It times
both against the same file with no get-value, then pins z3 to dv-solve's
get-model and checks that z3 gives every term the same value. Results:
docs/fw_hdl_session_queries.md."""
import os, re, subprocess, sys, time
from pathlib import Path
R = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(R / "tests/formal"), str(R)]
import gen_codegen_equiv as C, gen_fwhdl as G
from tests.perf import tools

DV = str(R / "build/dv-solve-smt2")
ENV = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
d_by = {d.name: d for d in [C._crc32(16), C._adder_tree(), C._rotl32(), C._mac16(), C._fir4()]}
name, L = os.environ.get("CASE", "crc32x16:8").split(":"); L = int(L); d = d_by[name]

mod, v = d.verilog(L)
smt2 = C.yosys_smt2(tools.path("yosys"), mod, v)
f, ins = d.spec
spec_defs = "\n".join(G.define_fun(g) for g in G._funs_of(f(*[G.var(n, w) for n, w in d.inputs])))
args = " ".join(f"(|{mod}_n {nm}| s0)" for nm in ins)
head = ["(set-option :produce-models true)", "(set-logic QF_BV)", smt2.rstrip(), spec_defs]
head += [f"(declare-fun s{i} () |{mod}_s|)" for i in range(L + 1)]
head += [f"(assert (|{mod}_t| s{i} s{i + 1}))" for i in range(L)]
head += [f"(define-fun spec () (_ BitVec {f.w}) ({G._q(f.name)} {args}))",
         f"(define-fun got () (_ BitVec {f.w}) (|{mod}_n out| s{L - 1}))",
         "(assert (not (= got spec)))", "(check-sat)"]
base = "\n".join(head) + "\n"
signals = sorted(set(re.findall(r"\(define-fun (\|" + re.escape(mod) + r"_n [^|]*\|) \(\(state \|", smt2)))
terms = [f"({s} s{k})" for s in signals for k in range(L + 1)]
out_dir = R / "perf-out/get_value"; out_dir.mkdir(parents=True, exist_ok=True)
files = {"none": base, "one": base + f"(get-value ({' '.join(terms)}))\n",
         "each": base + "".join(f"(get-value ({t}))\n" for t in terms)}


def run(text, reps=7):
    best, out = None, ""
    for _ in range(reps):
        t0 = time.monotonic()
        out = subprocess.run([DV], input=text, capture_output=True, text=True, env=ENV).stdout
        dt = time.monotonic() - t0
        best = dt if best is None else min(best, dt)
    return best, out


times = {}
for k, text in files.items():
    (out_dir / f"{name}_l{L}_{k}.smt2").write_text(text)
    times[k], out = run(text)
    assert out.split()[0] == "sat", out[:200]
print(f"{name} L={L}: {len(terms)} terms ({len(signals)} signals x {L + 1} steps); "
      + ", ".join(f"{k} {1000 * t:.1f} ms" for k, t in times.items()), flush=True)

# z3, pinned to dv-solve's model, must give every term the same value.
_, out = run(base + "(get-model)\n" + f"(get-value ({' '.join(terms)}))\n", reps=1)
model = re.findall(r"\(define-fun (\S+) \(\) (?:\(_ BitVec \d+\)|Bool) (#b[01]+|true|false|\(_ bv\d+ \d+\))\)", out)
pinned = base.rsplit("(assert (not (= got spec)))", 1)[0] + "".join(
    f"(assert (= {n} {val}))\n" for n, val in model) + "(check-sat)\n" + f"(get-value ({' '.join(terms)}))\n"
zo = subprocess.run([str(tools.path("z3")), "-in"], input=pinned, capture_output=True, text=True,
                    timeout=300).stdout
vals = lambda t: re.findall(r"(#b[01]+|#x[0-9a-f]+|true|false)\)", t)
norm = lambda v: v if v in ("true", "false") else int(v[2:], 2 if v[1] == "b" else 16)
a = [norm(x) for x in vals(out[out.index("(("):])]
b = [norm(x) for x in vals(zo[zo.index("("):])]
print(f"z3 pinned to dv-solve's model ({len(model)} entries): {zo.split()[0]}; "
      f"{sum(x == y for x, y in zip(a, b))}/{len(terms)} values agree", flush=True)
