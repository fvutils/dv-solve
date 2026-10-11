"""Sum normalization and slice substitution in the bit-blaster
(docs/fw_hdl_implementation_plan.md, piece 5.1).

Two rewrites make an adder tree and an adder chain over the same terms blast
to the same gates:

 - dvs_bbsum flattens an addition's operand that is itself a sum used once,
   when that keeps the value: the inner sum cannot wrap (exact), or the outer
   width is no wider (modular). The flat list is sorted and constants folded;
 - a top-level `(= ((_ extract hi lo) v) e)` defines those bits of v as e
   (Yosys `-stbv` registers), when no cycle results and slices do not overlap.

The generator aims at their edges: sums of zero-extended, `concat #b0` and
truncated terms whose inner sums wrap or don't by a bit, constants, sums
shared by two users, sums under `bvsub`/`ite`; state vectors whose slices
overlap, leave gaps, read their own vector, or are set twice. Each case runs
with the bit-blaster forced (DV_ENGINE=bitblast) and the default routing:

 - pinned: leaves fixed, z3 gives each term's value, dv-solve must find
   `(not (= term value))` unsat;
 - miter: a sum against a reassociated copy (same value, so unsat), and
   against a copy with one wrap point moved (z3 decides);
 - free: random assertions over state vectors; verdict against z3, a sat
   model's state vectors replayed in z3.

Scale with SUMN_N (default 150); SUMN_FIRST moves the window.
"""
from __future__ import annotations

import os
import random
import re
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = Path(os.environ.get("DVS_SMT2_EXE", _REPO / "build" / "dv-solve-smt2"))
_Z3 = shutil.which("z3")
_N = int(os.environ.get("SUMN_N", "150"))
_FIRST = int(os.environ.get("SUMN_FIRST", "0"))
_ENGINES = ["bitblast", None]

pytestmark = [
    pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built"),
    pytest.mark.skipif(_Z3 is None, reason="z3 not on PATH"),
]


def _run(cmd: list[str], text: str, engine: str | None = None) -> str:
    env = {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "DV_ENGINE")}
    if engine:
        env["DV_ENGINE"] = engine
    p = subprocess.run(cmd, input=text, capture_output=True, text=True, timeout=120, env=env)
    return p.stdout + p.stderr


def _values(out: str) -> list[str]:
    return re.findall(r"(#b[01]+|#x[0-9a-fA-F]+|true|false)\)", out)


class _Sums:
    """Random sums. A term is (text, width, max value); the max value lets the
    generator steer an inner sum to wrap or not by a bit."""

    def __init__(self, seed: int):
        self.r = random.Random(seed)
        r = self.r
        self.leaves = {"x%d" % i: r.randint(1, 10) for i in range(r.randint(3, 6))}
        self.lines = ["(set-logic QF_BV)"]
        self.lines += ["(declare-const %s (_ BitVec %d))" % kv for kv in self.leaves.items()]
        self.shared: list[tuple[str, int, int]] = []
        self.nlet = 0

    def widen(self, t, to):
        text, w, mx = t
        if to == w:
            return t
        if to < w:
            return ("((_ extract %d 0) %s)" % (to - 1, text), to, min(mx, (1 << to) - 1))
        if self.r.random() < 0.5:
            return ("((_ zero_extend %d) %s)" % (to - w, text), to, mx)
        return ("(concat (_ bv0 %d) %s)" % (to - w, text), to, mx)

    def leaf(self):
        r = self.r
        if self.shared and r.random() < 0.25:
            return r.choice(self.shared)
        if r.random() < 0.15:
            w = r.randint(1, 10)
            v = r.getrandbits(w)
            return ("(_ bv%d %d)" % (v, w), w, v)
        n = r.choice(list(self.leaves))
        w = self.leaves[n]
        return (n, w, (1 << w) - 1)

    def sum(self, d: int, w: int | None = None):
        """A sum at width w (random if None), its operands fitted to w."""
        r = self.r
        if d == 0 or r.random() < 0.2:
            t = self.leaf()
            return self.widen(t, w) if w else t
        a = self.sum(d - 1)
        b = self.sum(d - 1)
        if w is None:
            # Exactly wide enough, one bit short (it may wrap), or wider.
            need = max(1, (a[2] + b[2]).bit_length())
            w = max(1, min(14, need + r.choice([-1, 0, 0, 1, 2])))
        a, b = self.widen(a, w), self.widen(b, w)
        op = r.choice(["bvadd"] * 8 + ["bvsub", "bvxor"])
        mx = min(a[2] + b[2], (1 << w) - 1) if op == "bvadd" else (1 << w) - 1
        t = ("(%s %s %s)" % (op, a[0], b[0]), w, mx)
        if r.random() < 0.15:
            t = ("(ite (bvult %s %s) %s %s)" % (a[0], b[0], t[0], a[0]), w, (1 << w) - 1)
        if r.random() < 0.2:
            # Share it: a define-fun result used by more than one sum.
            name = "s%d" % self.nlet
            self.nlet += 1
            self.lines.append("(define-fun %s () (_ BitVec %d) %s)" % (name, w, t[0]))
            t = (name, w, t[2])
            self.shared.append(t)
        return t


def _terms(seed: int):
    g = _Sums(seed)
    return g, [g.sum(g.r.randint(2, 4)) for _ in range(6)]


@pytest.mark.parametrize("engine", _ENGINES)
@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_sum_values_match_z3(seed: int, engine) -> None:
    g, terms = _terms(seed)
    pins = "".join("(assert (= %s (_ bv%d %d)))\n" % (n, g.r.getrandbits(w), w)
                   for n, w in g.leaves.items())
    head = "\n".join(g.lines) + "\n" + pins
    z3 = _run([_Z3, "-in"], head + "(check-sat)\n(get-value (%s))\n"
              % " ".join(t[0] for t in terms))
    vals = _values(z3)
    assert len(vals) == len(terms), z3
    one = "".join("(push 1)(assert (not (= %s %s)))(check-sat)(pop 1)\n" % (t[0], v)
                  for t, v in zip(terms, vals))
    got = _run([str(_EXE)], head + one, engine).split()
    assert got == ["unsat"] * len(terms), "dv-solve %s\n%s%s" % (got, head, one)


def _reassoc(t: str, r: random.Random) -> str:
    """Swap the operands of bvadd nodes at random: the same value."""
    out, i = [], 0
    # Rebuild by parsing the s-expression.
    def parse(s, i):
        if s[i] != "(":
            j = i
            while j < len(s) and s[j] not in " ()":
                j += 1
            return s[i:j], j
        items, i = [], i + 1
        while True:
            while s[i] == " ":
                i += 1
            if s[i] == ")":
                return items, i + 1
            x, i = parse(s, i)
            items.append(x)

    def show(x):
        if isinstance(x, str):
            return x
        if x and x[0] == "bvadd" and r.random() < 0.6:
            x = [x[0], x[2], x[1]]
        return "(" + " ".join(show(y) for y in x) + ")"

    tree, _ = parse(t, 0)
    return show(tree)


@pytest.mark.parametrize("engine", _ENGINES)
@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_sum_miters_match_z3(seed: int, engine) -> None:
    g, terms = _terms(50_000 + seed)
    a = terms[0]
    # Same sum, operands swapped: equal. A different sum: z3 decides.
    other = g.widen(terms[1], a[1])
    body = "\n".join(g.lines) + "\n"
    for rhs in (_reassoc(a[0], g.r), other[0]):
        text = body + "(assert (not (= %s %s)))\n(check-sat)\n" % (a[0], rhs)
        want = _run([_Z3, "-in"], text).split()[0]
        names = list(g.leaves)
        out = _run([str(_EXE)], text + "(get-value (%s))\n" % " ".join(names), engine)
        got = out.split()[0]
        assert got == want, "dv-solve %s, z3 %s\n%s" % (got, want, text)
        if got == "sat":
            vals = _values(out)
            assert len(vals) == len(names), out
            pins = "".join("(assert (= %s %s))\n" % nv for nv in zip(names, vals))
            chk = _run([_Z3, "-in"], text.replace("(check-sat)\n", pins + "(check-sat)\n"))
            assert chk.split()[0] == "sat", "model fails in z3\n%s\n%s" % (pins, text)


class _State:
    """Yosys-shaped state vectors: s0 .. sk of width W, each step's slices
    set from the previous step, read through define-fun wrappers."""

    def __init__(self, seed: int):
        self.r = r = random.Random(seed)
        self.W = W = r.choice([8, 12, 20])
        self.k = r.randint(2, 4)
        self.lines = ["(set-logic QF_BV)", "(define-sort S () (_ BitVec %d))" % W]
        self.lines += ["(declare-fun s%d () S)" % i for i in range(self.k + 1)]
        self.lines.append("(declare-const in (_ BitVec 4))")
        # Register fields of the state: [lo, hi] pairs, some overlapping.
        cuts = sorted(r.sample(range(1, W), r.randint(2, 4)))
        self.fields = [(lo, hi - 1) for lo, hi in zip([0] + cuts, cuts + [W])]
        if r.random() < 0.3:
            lo = r.randrange(W - 2)
            self.fields.append((lo, min(W - 1, lo + r.randint(1, 4))))   # overlaps
        for f, (lo, hi) in enumerate(self.fields):
            self.lines.append("(define-fun r%d ((state S)) (_ BitVec %d) ((_ extract %d %d) state))"
                              % (f, hi - lo + 1, hi, lo))

    def expr(self, state: str, w: int, d: int = 2) -> str:
        r = self.r
        if d == 0 or r.random() < 0.3:
            f = r.randrange(len(self.fields))
            lo, hi = self.fields[f]
            t, tw = "(r%d %s)" % (f, state), hi - lo + 1
            if r.random() < 0.3:
                t, tw = "in", 4
            if tw > w:
                return "((_ extract %d 0) %s)" % (w - 1, t)
            return "((_ zero_extend %d) %s)" % (w - tw, t) if tw < w else t
        op = r.choice(["bvadd", "bvadd", "bvxor", "bvand", "bvsub"])
        return "(%s %s %s)" % (op, self.expr(state, w, d - 1), self.expr(state, w, d - 1))

    def step(self, i: int) -> list[str]:
        """Assertions setting s_i's fields from s_{i-1} (sometimes from s_i
        itself, or a field set twice)."""
        r = self.r
        out = []
        for f, (lo, hi) in enumerate(self.fields):
            if r.random() < 0.15:
                continue                                   # a gap: left free
            src = "s%d" % (i if r.random() < 0.1 else i - 1)
            e = self.expr(src, hi - lo + 1)
            lhs = "(r%d s%d)" % (f, i) if r.random() < 0.7 else \
                "((_ extract %d %d) s%d)" % (hi, lo, i)
            out.append("(= %s %s)" % ((lhs, e) if r.random() < 0.7 else (e, lhs)))
            if r.random() < 0.1:
                out.append("(= %s %s)" % (lhs, self.expr("s%d" % (i - 1), hi - lo + 1)))
        return out


@pytest.mark.parametrize("engine", _ENGINES)
@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_state_slices_match_z3(seed: int, engine) -> None:
    g = _State(100_000 + seed)
    r = g.r
    text = "\n".join(g.lines) + "\n"
    for i in range(1, g.k + 1):
        cs = g.step(i)
        if r.random() < 0.5 and len(cs) > 1:
            text += "(assert (and %s))\n" % " ".join(cs)
        else:
            text += "".join("(assert %s)\n" % c for c in cs)
    # A property of the last state, and sometimes a pin of the first.
    lo, hi = r.choice(g.fields)
    text += "(assert (%s (r%d s%d) %s))\n" % (
        r.choice(["=", "distinct", "bvult"]), g.fields.index((lo, hi)), g.k,
        g.expr("s0", hi - lo + 1))
    if r.random() < 0.5:
        text += "(assert (= s0 (_ bv%d %d)))\n" % (r.getrandbits(g.W), g.W)
    text += "(check-sat)\n"
    want = _run([_Z3, "-in"], text).split()[0]
    names = ["s%d" % i for i in range(g.k + 1)] + ["in"]
    out = _run([str(_EXE)], text + "(get-value (%s))\n" % " ".join(names), engine)
    got = out.split()[0]
    assert got == want, "dv-solve %s, z3 %s\n%s\n%s" % (got, want, out, text)
    if got == "sat":
        vals = _values(out)
        assert len(vals) == len(names), out
        pins = "".join("(assert (= %s %s))\n" % nv for nv in zip(names, vals))
        chk = _run([_Z3, "-in"], text.replace("(check-sat)\n", pins + "(check-sat)\n"))
        assert chk.split()[0] == "sat", "model fails in z3\n%s\n%s" % (pins, text)


def test_adder_tree_collapses() -> None:
    """A balanced tree with growing widths (Yosys) against a chain at the
    final width (an IR spec): the normal forms meet, so the bit-blaster sees
    the same gates and the miter needs no search."""
    xs = ["a%d" % i for i in range(8)]
    lines = ["(set-logic QF_BV)"] + ["(declare-const %s (_ BitVec 16))" % x for x in xs]
    lvl, w = list(xs), 16
    while len(lvl) > 1:
        lvl = ["(bvadd (concat #b0 %s) (concat #b0 %s))" % (lvl[i], lvl[i + 1])
               for i in range(0, len(lvl), 2)]
        w += 1
    chain = "((_ zero_extend 3) a0)"
    for x in xs[1:]:
        chain = "(bvadd %s ((_ zero_extend 3) %s))" % (chain, x)
    lines += ["(assert (not (= %s %s)))" % (lvl[0], chain), "(check-sat)"]
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_ENGINE="bitblast", DV_BB_STATS="1")
    p = subprocess.run([str(_EXE)], input="\n".join(lines) + "\n", capture_output=True,
                       text=True, timeout=60, env=env)
    assert p.stdout.split()[0] == "unsat", p.stdout + p.stderr
    m = re.search(r"sums=(\d+)/", p.stderr)
    assert m and int(m.group(1)) > 0, p.stderr
    # The two sides blast to the same vector, so SAT has nothing to search
    # (about 0.05 ms; over a second without the normalization).
    m = re.search(r"sat=([0-9.]+)ms", p.stderr)
    assert m and float(m.group(1)) < 100, p.stderr
