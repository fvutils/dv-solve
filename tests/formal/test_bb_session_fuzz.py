"""Differential fuzz of the bit-blast session (fw-hdl plan piece 6.1).

One SMT-LIB session keeps one bit-blast instance across check-sats
(_check_sat_session, dvs_bbsolver_sess_*): constraints are added per
check-sat, push/pop map to selector scopes, and variable ids are reused after
a pop. These scripts mix nested push/pop, declarations inside scopes, names
re-declared at other widths after a pop, defining equalities (`(= v e)`, which
the bit-blaster substitutes), slice definitions (`(= ((_ extract h l) s) e)`)
and `let`, with a check-sat, get-value and get-model every few commands.

z3 runs the same script; every dv-solve sat/unsat must match it, and every
dv-solve model must satisfy the assertions in scope (checked by z3), and
get-model must list every visible name with get-value's value. The model
check is off (DV_CHECK_MODEL=0) so wrong answers show instead of `unknown`.
CDCL's validation net stays on: it is how constraints the CDCL compile leaves
out are enforced. A violation it reports with every constraint compiled is a
CDCL bug all the same, and fails the test.
Most seeds force the bit-blast engine; every fourth keeps the default
routing, so narrow problems take the CDCL path (check-sat-assuming included).

    FUZZ_N=2000 pytest tests/formal/test_bb_session_fuzz.py
"""
from __future__ import annotations

import os
import random
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get("DV_SOLVE_SMT2", REPO / "build" / "dv-solve-smt2"))
Z3 = shutil.which("z3")
N = int(os.environ.get("FUZZ_N", "200"))
WIDTHS = [1, 2, 3, 4, 5, 8, 8, 16, 33, 65, 70]
POOL = [f"p{i}" for i in range(8)]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.scopes = [{"vars": {}, "asserts": []}]

    def visible(self):
        v = {}
        for s in self.scopes:
            v.update(s["vars"])
        return v

    def fresh_name(self):
        vis = self.visible()
        free = [n for n in POOL if n not in vis]
        if free:
            return self.r.choice(free)
        return f"q{self.r.randrange(10**6)}"

    def declare(self, w=None):
        n = self.fresh_name()
        w = w or self.r.choice(WIDTHS)
        self.scopes[-1]["vars"][n] = w
        return n, f"(declare-fun {n} () (_ BitVec {w}))"

    def bvs(self):
        return {n: w for n, w in self.visible().items() if w}

    def bools(self):
        return [n for n, w in self.visible().items() if not w]

    def const(self, w):
        return f"(_ bv{self.r.randrange(1 << min(w, 70)) % (1 << w)} {w})"

    def term(self, w, d):
        r = self.r
        vis = self.bvs()
        same = [n for n, vw in vis.items() if vw == w]
        wider = [(n, vw) for n, vw in vis.items() if vw > w]
        k = r.random()
        if d <= 0 or k < 0.25:
            if same and r.random() < 0.7:
                return r.choice(same)
            if wider and r.random() < 0.5:
                n, vw = r.choice(wider)
                lo = r.randrange(vw - w + 1)
                return f"((_ extract {lo + w - 1} {lo}) {n})"
            return self.const(w)
        if k < 0.55:
            op = r.choice(["bvadd", "bvxor", "bvand", "bvor", "bvsub"]
                          + (["bvmul"] if w <= 8 else []))
            return f"({op} {self.term(w, d - 1)} {self.term(w, d - 1)})"
        if k < 0.65:
            return f"({r.choice(['bvnot', 'bvneg'])} {self.term(w, d - 1)})"
        if k < 0.75 and w >= 2:
            a = r.randrange(1, w)
            return f"(concat {self.term(a, d - 1)} {self.term(w - a, d - 1)})"
        if k < 0.85:
            return f"(ite {self.pred(d - 1)} {self.term(w, d - 1)} {self.term(w, d - 1)})"
        if k < 0.92 and w >= 2:
            a = r.randrange(1, w)
            return f"((_ zero_extend {w - a}) {self.term(a, d - 1)})"
        t = self.term(w, d - 1)
        return f"(let ((t{d} {t})) (bvxor t{d} {self.term(w, d - 1)}))"

    def pred(self, d):
        r = self.r
        k = r.random()
        if d > 0 and k < 0.2:
            op = r.choice(["and", "or", "xor", "=>"])
            return f"({op} {self.pred(d - 1)} {self.pred(d - 1)})"
        if d > 0 and k < 0.27:
            return f"(not {self.pred(d - 1)})"
        if self.bools() and r.random() < 0.15:
            return r.choice(self.bools())
        vis = self.bvs()
        w = r.choice(list(vis.values())) if vis and r.random() < 0.8 else r.choice(WIDTHS)
        op = r.choice(["=", "distinct", "bvult", "bvule", "bvugt", "bvslt", "bvsge"])
        return f"({op} {self.term(w, d)} {self.term(w, d)})"

    def assert_(self, text):
        self.scopes[-1]["asserts"].append(text)
        return f"(assert {text})"

    def step(self):
        """One or more commands."""
        r = self.r
        k = r.random()
        depth = len(self.scopes) - 1
        if k < 0.15 or not self.bvs():
            return [self.declare()[1]]
        if k < 0.19:
            # A Bool, usually naming a predicate: check-sat-assuming's literals.
            n = self.fresh_name()
            self.scopes[-1]["vars"][n] = 0
            out = [f"(declare-fun {n} () Bool)"]
            if r.random() < 0.8:
                out.append(self.assert_(f"(= {n} {self.pred(r.randint(0, 2))})"))
            return out
        if k < 0.24 and self.bools():
            bs = self.bools()
            lits = [b if r.random() < 0.5 else f"(not {b})"
                    for b in r.sample(bs, r.randint(1, min(3, len(bs))))]
            return self.check(lits)
        if k < 0.40:
            return [self.assert_(self.pred(r.randint(0, 2)))]
        if k < 0.52:
            # A defining equality over existing variables: substituted.
            w = r.choice(WIDTHS)
            rhs = self.term(w, r.randint(1, 3))
            n, decl = self.declare(w)
            eq = f"(= {n} {rhs})" if r.random() < 0.7 else f"(= {rhs} {n})"
            return [decl, self.assert_(eq)]
        if k < 0.56:
            # A defining equality for a variable declared earlier, possibly in
            # an outer scope and not yet read: substitutable only there.
            n, w = r.choice(list(self.bvs().items()))
            return [self.assert_(f"(= {n} {self.term(w, r.randint(1, 3))})")]
        if k < 0.59 and depth < 4:
            # The same for a variable nothing has read yet: declare, push,
            # define it inside the scope. After the pop, its negation is
            # sometimes asserted, which a definition that outlived its scope
            # turns into a wrong unsat.
            n, decl = self.declare()
            w = self.visible()[n]
            c = self.const(w)
            self.scopes.append({"vars": {}, "asserts": [], "undo": (n, c)})
            return [decl, "(push 1)", self.assert_(f"(= {n} {c})")]
        if k < 0.64:
            # Slices of a fresh variable defined piecewise, in one conjunction.
            w = r.choice([4, 5, 8, 16, 33, 65])
            n, decl = self.declare(w)
            cuts = sorted(r.sample(range(1, w), r.randint(1, min(3, w - 1))))
            parts, lo = [], 0
            for c in cuts + [w]:
                if r.random() < 0.8:
                    parts.append(f"(= ((_ extract {c - 1} {lo}) {n}) {self.term(c - lo, 2)})")
                lo = c
            if not parts:
                return [decl]
            body = parts[0] if len(parts) == 1 else f"(and {' '.join(parts)})"
            return [decl, self.assert_(body)]
        if k < 0.76 and depth < 4:
            n = 1 if r.random() < 0.85 else 2
            for _ in range(n):
                self.scopes.append({"vars": {}, "asserts": []})
            return [f"(push {n})"]
        if k < 0.86 and depth > 0:
            n = 1 if depth == 1 or r.random() < 0.8 else 2
            undo = [s["undo"] for s in self.scopes[-n:] if "undo" in s]
            del self.scopes[-n:]
            out = [f"(pop {n})"]
            for v, c in undo:
                if v in self.visible() and r.random() < 0.6:
                    out.append(self.assert_(f"(distinct {v} {c})"))
            return out
        return self.check()

    def check(self, lits=None):
        names = list(self.visible())
        cmds = [f"(check-sat-assuming ({' '.join(lits)}))" if lits else "(check-sat)"]
        if names:
            cmds.append(f"(get-value ({' '.join(names)}))")
            cmds.append("(get-model)")
        flat = (self.visible(), [a for s in self.scopes for a in s["asserts"]] + (lits or []))
        self.checks.append(flat)
        return cmds

    def script(self, n_steps):
        self.checks = []
        out = ["(set-option :produce-models true)", "(set-logic QF_BV)"]
        for _ in range(n_steps):
            out += self.step()
        out += self.check()
        return "\n".join(out) + "\n", self.checks


_LIT = r"(#b[01]+|#x[0-9a-fA-F]+|\(_ bv\d+ \d+\)|true|false)"
_VAL = re.compile(r"\(\s*([A-Za-z_][\w]*)\s+" + _LIT + r"\s*\)")
_DEF = re.compile(r"\(define-fun\s+([A-Za-z_][\w]*)\s+\(\)\s+(Bool|\(_ BitVec (\d+)\))\s+" + _LIT + r"\s*\)")


def _int(v):
    if v in ("true", "false"):
        return v
    if v.startswith("#b"):
        return int(v[2:], 2)
    if v.startswith("#x"):
        return int(v[2:], 16)
    return int(v.split()[1][2:])


def _parse(out):
    """[(answer, get-value {name: value} or None, get-model {name: (width,
    value)} or None)] per check-sat. The responses may span lines; they run
    until the next answer."""
    res, text = [], None
    for line in out.splitlines() + ["sat"]:
        w = line.strip()
        if w in ("sat", "unsat", "unknown"):
            if res and text and text.lstrip().startswith("(("):
                gv, _, gm = text.partition("(define-fun")
                res[-1][1] = {m.group(1): _int(m.group(2)) for m in _VAL.finditer(gv)}
                if gm:
                    res[-1][2] = {m.group(1): (int(m.group(3) or 0), _int(m.group(4)))
                                  for m in _DEF.finditer("(define-fun" + gm)}
            res.append([w, None, None])
            text = ""
        elif text is not None:
            text += " " + w
    return res[:-1]


def _run(argv, script, env=None, timeout=60):
    p = subprocess.run(argv, input=script, capture_output=True, text=True,
                       timeout=timeout, env={**os.environ, **(env or {})})
    return p.stdout, p.stderr


def _model_problems(checks, answers):
    """One z3 problem per dv-solve model: the assertions in scope plus the
    model's values. Each must be sat."""
    blocks, idx = [], []
    for i, ((vis, asserts), (ans, model, _)) in enumerate(zip(checks, answers)):
        if ans != "sat" or not vis:
            continue
        if model is None or set(model) != set(vis):
            blocks.append(None)
            idx.append(i)
            continue
        b = ["(reset)", "(set-logic QF_BV)"]
        b += [f"(declare-fun {n} () (_ BitVec {w}))" if w else f"(declare-fun {n} () Bool)"
              for n, w in vis.items()]
        b += [f"(assert {a})" for a in asserts]
        b += [f"(assert (= {n} (_ bv{model[n]} {vis[n]})))" if vis[n]
              else f"(assert (= {n} {model[n]}))" for n in vis]
        b.append("(check-sat)")
        blocks.append("\n".join(b))
        idx.append(i)
    return blocks, idx


@pytest.mark.skipif(not EXE.exists(), reason="dv-solve-smt2 not built")
@pytest.mark.skipif(Z3 is None, reason="z3 not on PATH")
@pytest.mark.parametrize("seed", range(N))
def test_bb_session_fuzz(seed, tmp_path):
    g = Gen(seed)
    script, checks = g.script(g.r.randint(15, 60))
    env = {"DV_ENGINE": "bitblast", "DV_CHECK_MODEL": "0", "DV_LOG": "1", "LD_LIBRARY_PATH": ""}
    if seed % 3 == 1:
        env["DV_SIM"] = "0"                  # plain SAT under the scope assumptions
    if seed % 4 == 3:
        del env["DV_ENGINE"]                 # default routing: CDCL for narrow problems
    dv_out, dv_err = _run([str(EXE), "--interactive"], script, env)
    z3_out, _ = _run([Z3, "-in", "-smt2"], script)
    dv, z3 = _parse(dv_out), _parse(z3_out)
    (tmp_path / "s.smt2").write_text(script)
    where = f"seed {seed}; script {tmp_path / 's.smt2'}\n{dv_err[-2000:]}"
    for m in re.finditer(r"top-level constraint\(s\) violated.*\ncdcl-unknown: .*"
                         r"uncompiled-constraints=(\d+)", dv_err):
        assert m.group(1) != "0", f"CDCL model wrong with every constraint compiled; {where}"
    assert len(dv) == len(checks), f"dv-solve gave {len(dv)} answers for {len(checks)}: {where}"
    assert len(z3) == len(checks), f"z3 gave {len(z3)} answers: {where}"
    for i, ((a, *_), (b, *_)) in enumerate(zip(dv, z3)):
        if a in ("sat", "unsat") and b in ("sat", "unsat"):
            assert a == b, f"check-sat {i + 1}: dv-solve {a}, z3 {b}; {where}"
    for i, ((vis, _), (a, gv, gm)) in enumerate(zip(checks, dv)):
        if a != "sat" or not vis:
            continue
        want = {n: (w, gv.get(n) if gv else None) for n, w in vis.items()}
        assert gm == want, f"check-sat {i + 1}: get-model {gm} != get-value {want}; {where}"
    blocks, idx = _model_problems(checks, dv)
    assert all(b is not None for b in blocks), f"incomplete get-value; {where}"
    if blocks:
        out, _ = _run([Z3, "-in", "-smt2"], "\n".join(blocks) + "\n")
        verdicts = [w for w in out.split() if w in ("sat", "unsat", "unknown")]
        for i, v in zip(idx, verdicts):
            assert v != "unsat", f"check-sat {i + 1}: dv-solve's model is wrong; {where}"
