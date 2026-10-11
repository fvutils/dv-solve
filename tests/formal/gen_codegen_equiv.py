"""The fwhdl-core `codegen-equiv` family (docs/fw_hdl_implementation_plan.md piece 0.3).

fw-hdl's equivalence question: is a pipelined Verilog implementation the same
function as the IR it was generated from, at the right latency? Each design
here is written twice:

- the implementation: pipelined Verilog, L register stages, sent through Yosys
  `write_smt2 -stbv`, so the state is a bit-vector `define-sort` (fw-hdl
  request R4) and the transition relation is Yosys' own, verbatim;
- the specification: one combinational `define-fun`, written independently in
  gen_fwhdl's expression language, as fw-hdl prints IR.

The miter unrolls the transition relation L steps from an arbitrary state and
asserts that the output differs from the specification of the inputs L cycles
earlier: unsat when the latency is right. The `early` and `late` variants
compare one cycle too soon or too late: sat (the output then depends on the
arbitrary initial state, or on a later input).

Used by gen_fwhdl.py; needs the pinned yosys (tests/perf/tools.py).
"""
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

import gen_fwhdl as G

CRC_POLY = 0xEDB88320


class Design:
    """A dataflow of steps over named signals. A step is a list of
    (name, width, verilog_expr); `{n}` in an expression is signal n's current
    Verilog name. Every live signal is registered at each stage boundary."""

    def __init__(self, name, inputs, steps, out, out_w, spec, functions=""):
        self.name, self.inputs, self.steps = name, inputs, steps
        self.out, self.out_w, self.spec, self.functions = out, out_w, spec, functions

    def verilog(self, L):
        mod = f"{self.name}_l{L}"
        ports = ["input clk"] + [f"input [{w - 1}:0] {n}" for n, w in self.inputs] + \
                [f"output [{self.out_w - 1}:0] out"]
        body = [self.functions] if self.functions else []
        cur = {n: n for n, _ in self.inputs}
        width = dict(self.inputs)
        per = -(-len(self.steps) // L)                    # steps per stage, rounded up
        k = 0
        for s in range(L):
            for step in self.steps[s * per:(s + 1) * per]:
                new = {}
                for n, w, e in step:
                    k += 1
                    v = f"{n}_w{k}"
                    body.append(f"  wire [{w - 1}:0] {v} = {e.format(**cur)};")
                    new[n] = v
                    width[n] = w
                cur.update(new)
            for n in list(cur):
                r = f"{n}_r{s}"
                body.append(f"  reg [{width[n] - 1}:0] {r};")
                body.append(f"  always @(posedge clk) {r} <= {cur[n]};")
                cur[n] = r
        body.append(f"  assign out = {self.out.format(**cur)};")
        return mod, f"module {mod}({', '.join(ports)});\n" + "\n".join(body) + "\nendmodule\n"


def _crc32(nbytes):
    fn = ("  function [31:0] crc_byte(input [31:0] c, input [7:0] b);\n"
          "    integer k; reg [31:0] x;\n"
          "    begin\n"
          "      x = c ^ {24'b0, b};\n"
          f"      for (k = 0; k < 8; k = k + 1) x = (x >> 1) ^ (32'h{CRC_POLY:08X} & {{32{{x[0]}}}});\n"
          "      crc_byte = x;\n"
          "    end\n"
          "  endfunction")
    steps = [[("c", 32, "crc_byte(32'hFFFFFFFF, {m}[7:0])")]]
    for i in range(1, nbytes):
        steps.append([("c", 32, f"crc_byte({{c}}, {{m}}[{8 * i + 7}:{8 * i}])")])
    f = G.crc_bitwise(32, nbytes)
    return Design(f"crc32x{nbytes}", [("m", 8 * nbytes)], steps, "~{c}", 32,
                  (f, ["m"]), fn)


def _adder_tree(n=8, w=16):
    lw = w + (n - 1).bit_length()
    steps, names = [], [f"x{i}" for i in range(n)]
    steps.append([(f"x{i}", lw, f"{{{{{lw - w}'b0, {{a}}[{w * i + w - 1}:{w * i}]}}}}")
                  for i in range(n)])
    lvl = 0
    while len(names) > 1:
        lvl += 1
        nxt = [f"s{lvl}_{j}" for j in range(len(names) // 2)]
        steps.append([(t, lw, f"{{{a}}} + {{{b}}}") for t, a, b in
                      zip(nxt, names[0::2], names[1::2])])
        names = nxt

    def spec(a):
        xs = [a.ext(w * i + w - 1, w * i).zext(lw) for i in range(n)]
        return G.xor_free_sum(xs)
    f = G.Fun(f"adder_tree_{n}x{w}", [("a", n * w)], spec)
    return Design(f"addtree{n}x{w}", [("a", n * w)], steps, "{" + names[0] + "}", lw, (f, ["a"]))


def _rotl32():
    steps = []
    for k in range(5):
        sh = 1 << k
        steps.append([("x", 32, f"{{amt}}[{k}] ? {{{{{{x}}[{31 - sh}:0], {{x}}[31:{32 - sh}]}}}} : {{x}}")])

    def spec(x, amt):
        a = amt.zext(32)
        return (x << a) | (x >> (32 - a))
    f = G.Fun("rotl32", [("x", 32), ("amt", 5)], spec)
    return Design("rotl32", [("x", 32), ("amt", 5)], steps, "{x}", 32, (f, ["x", "amt"]))


def _mac16():
    steps = [[("p", 32, "{a} * {b}")], [("r", 32, "{p} + {{16'b0, {c}}}")]]

    def spec(a, b, c):
        return a.zext(32) * b.zext(32) + c.zext(32)
    f = G.Fun("mac16", [("a", 16), ("b", 16), ("c", 16)], spec)
    return Design("mac16", [("a", 16), ("b", 16), ("c", 16)], steps, "{r}", 32,
                  (f, ["a", "b", "c"]))


def _fir4(coef=(3, 7, 7, 3), w=12):
    ow = w + 6
    steps = [[(f"p{i}", ow, f"{{{{{ow - w}'b0, {{x}}[{w * i + w - 1}:{w * i}]}}}} * {ow}'d{c}")
              for i, c in enumerate(coef)],
             [("q0", ow, "{p0} + {p1}"), ("q1", ow, "{p2} + {p3}")],
             [("y", ow, "{q0} + {q1}")]]

    def spec(x):
        return G.xor_free_sum([x.ext(w * i + w - 1, w * i).zext(ow) * c
                               for i, c in enumerate(coef)])
    f = G.Fun("fir4", [("x", 4 * w)], spec)
    return Design("fir4", [("x", 4 * w)], steps, "{y}", ow, (f, ["x"]))


CONFIGS = [(_crc32(4), (1, 2, 4)), (_crc32(8), (1, 2, 4, 8)), (_adder_tree(), (1, 2, 3)),
           (_rotl32(), (1, 2, 3, 5)), (_mac16(), (1, 2, 3)), (_fir4(), (1, 2, 3))]


def yosys_smt2(yosys, mod, verilog):
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "m.v").write_text(verilog)
        subprocess.run([str(yosys), "-q", "-p",
                        f"read_verilog m.v; prep -top {mod}; write_smt2 -stbv m.smt2"],
                       cwd=d, check=True, capture_output=True)
        return (Path(d) / "m.smt2").read_text()


class Instance:
    cat = "codegen-equiv"
    seed = 0

    def __init__(self, design, L, variant, smt2):
        self.family = design.name
        self.knobs = {"latency": L, "variant": variant}
        self.id = f"{design.name}_l{L}_{variant}"
        self.expect = "unsat" if variant == "ok" else "sat"
        import hashlib
        h = int(hashlib.sha256(self.id.encode()).hexdigest()[:8], 16)
        self.split = "held" if h % 10 < 3 else "dev"
        self._text = self._miter(design, L, variant, smt2)

    def _miter(self, d, L, variant, smt2):
        mod = f"{d.name}_l{L}"
        at = {"ok": L, "early": L - 1, "late": L + 1}[variant]
        f, ins = d.spec
        spec_defs = "\n".join(G.define_fun(g) for g in G._funs_of(f(*[G.var(n, w) for n, w in d.inputs])))
        n = max(at, L) + 1
        out = [f"; fwhdl-core codegen-equiv: {d.name} latency={L} variant={variant}",
               f"; {d.name}: {L}-stage pipeline (Verilog -> yosys write_smt2 -stbv) against its "
               "combinational specification;",
               f"; the output at step {at} compared with the specification of the inputs at step 0.",
               "; Generated by tests/formal/gen_codegen_equiv.py; do not edit.",
               "(set-info :smt-lib-version 2.6)",
               f"(set-info :status {self.expect})",
               "(set-option :produce-models true)",
               "(set-logic QF_BV)",
               smt2.rstrip(),
               "; ---- specification ----",
               spec_defs,
               "; ---- miter ----"]
        out += [f"(declare-fun s{i} () |{mod}_s|)" for i in range(n)]
        out += [f"(assert (|{mod}_t| s{i} s{i + 1}))" for i in range(n - 1)]
        args = " ".join(f"(|{mod}_n {nm}| s0)" for nm in ins)
        out.append(f"(assert (not (= (|{mod}_n out| s{at}) ({G._q(f.name)} {args}))))")
        out.append("(check-sat)")
        out.append("(exit)")
        return "\n".join(out) + "\n"

    def text(self):
        return self._text


def build(yosys):
    out = []
    for d, lats in CONFIGS:
        for L in lats:
            mod, v = d.verilog(L)
            smt2 = yosys_smt2(yosys, mod, v)
            for variant in ("ok", "early", "late"):
                out.append(Instance(d, L, variant, smt2))
    return out
