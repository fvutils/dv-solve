"""Word-level facts in the bit-blaster (docs/fw_hdl_implementation_plan.md,
piece 5.4, src/c/dvs_bbword.c).

The bit-blaster remembers facts about the vectors it builds and uses them to
cancel inverse operations and to decide equalities without SAT:

 - shapes: `x ^ (x >> k)` sums is a GF(2) polynomial in the shift applied to
   a base; a multiply by a constant composes with the next one. An XOR whose
   polynomials cancel to 1 is the base, a product of inverse constants is the
   base, and f(x) = f(y) for the same invertible shape compares x and y;
 - values: bounds and linear forms (exact, or modulo the constant of a
   `bvurem`) through additions and constant multiplies that cannot wrap and
   low extracts that keep every value. Two values below a common modulus with
   congruent forms are equal.

The generator aims at the edges: shift amounts and polynomials truncated at
the width, even multipliers (not invertible), almost-inverse constants and
un-xorshifts one round short; checksums reduced per step against once at the
end, with accumulators one bit too narrow, a modulus off by one, an extract
that drops a bit, an extra term, and concat parts that do not line up. Each
case runs with the bit-blaster forced and with the default routing; the
verdict must match z3 and a sat model must satisfy the problem in z3.

Scale with WORD_N (default 150); WORD_FIRST moves the window.
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
_N = int(os.environ.get("WORD_N", "150"))
_FIRST = int(os.environ.get("WORD_FIRST", "0"))
_ENGINES = ["bitblast", None]

pytestmark = [
    pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built"),
    pytest.mark.skipif(_Z3 is None, reason="z3 not on PATH"),
]


def _run(cmd: list[str], text: str, engine: str | None = None,
         extra: dict | None = None) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "DV_ENGINE")}
    if engine:
        env["DV_ENGINE"] = engine
    env.update(extra or {})
    return subprocess.run(cmd, input=text, capture_output=True, text=True, timeout=120,
                          env=env)


def _values(out: str) -> list[str]:
    return re.findall(r"(#b[01]+|#x[0-9a-fA-F]+)\)", out)


def _c(v: int, w: int) -> str:
    return "(_ bv%d %d)" % (v % (1 << w), w)


def _check(decls: list[tuple[str, int]], body: str, engine) -> None:
    """dv-solve's verdict on `body` matches z3's; a sat model holds in z3."""
    head = "(set-logic QF_BV)\n" + "".join(
        "(declare-const %s (_ BitVec %d))\n" % d for d in decls)
    text = head + body + "(check-sat)\n"
    want = _run([_Z3, "-in"], text).stdout.split()[0]
    names = [n for n, _ in decls]
    p = _run([str(_EXE)], text + "(get-value (%s))\n" % " ".join(names), engine)
    out = p.stdout + p.stderr
    got = out.split()[0] if out.split() else ""
    assert got == want, "dv-solve %s, z3 %s\n%s\n%s" % (got, want, out, text)
    if got == "sat":
        vals = _values(p.stdout)
        assert len(vals) == len(names), out
        pins = "".join("(assert (= %s %s))\n" % nv for nv in zip(names, vals))
        chk = _run([_Z3, "-in"], head + body + pins + "(check-sat)\n").stdout.split()
        assert chk[0] == "sat", "model fails in z3\n%s\n%s" % (pins, text)


# ------------------------------------------------------------------ shapes

class _Shapes:
    """Chains of xorshifts and constant multiplies at width w, and inverses
    that are exact or off by a little."""

    def __init__(self, seed: int):
        self.r = random.Random(seed)
        self.w = self.r.randint(3, 12)

    def step(self):
        r, w = self.r, self.w
        kind = r.choices(["xs", "mul", "xs2", "shr"], [4, 4, 1, 1])[0]
        if kind == "mul":
            c = r.getrandbits(w) | (1 if r.random() < 0.8 else 0)
            return ("mul", c or 2)
        if kind == "xs2":
            return ("xs2", r.randint(1, w), r.randint(1, w))
        return (kind, r.randint(1, w + 1))         # k = w + 1: shifts all out

    def chain(self, n):
        return [self.step() for _ in range(n)]

    def inverse(self, chain):
        """The inverse chain, with a perturbation now and then."""
        r, w = self.r, self.w
        inv = []
        for op in reversed(chain):
            if op[0] == "mul":
                c = op[1]
                ci = pow(c, -1, 1 << w) if c & 1 else c
                if r.random() < 0.1:
                    ci ^= 1 << r.randrange(w)
                inv.append(("mul", ci))
            elif op[0] == "xs":
                k = op[1]
                rounds = -(-w // k) if k <= w else 1
                if r.random() < 0.1 and rounds > 1:
                    rounds -= 1
                inv.append(("unxs", k, rounds))
            else:
                inv.append(op)                     # not inverted: z3 decides
        return inv

    def apply(self, chain, x, binds):
        """chain applied to x; each step is a let binding (appended to
        `binds`), so a xorshift's two reads do not double the text."""
        w = self.w

        def bind(e):
            name = "t%d" % len(binds)
            binds.append((name, e))
            return name

        for op in chain:
            if op[0] == "mul":
                x = bind("(bvmul %s %s)" % (x, _c(op[1], w)) if self.r.random() < 0.7
                         else "(bvmul %s %s)" % (_c(op[1], w), x))
            elif op[0] == "xs":
                x = bind("(bvxor %s (bvlshr %s %s))" % (x, x, _c(op[1], w)))
            elif op[0] == "shr":
                x = bind("(bvlshr %s %s)" % (x, _c(op[1], w)))
            elif op[0] == "xs2":
                x = bind("(bvxor (bvxor %s (bvlshr %s %s)) (bvlshr %s %s))" % (
                    x, x, _c(op[1], w), x, _c(op[2], w)))
            else:                                  # un-xorshift, as fmix_inverse
                k, rounds = op[1], op[2]
                y = x
                t = bind("(bvxor %s (bvlshr %s %s))" % (y, y, _c(k, w)))
                for _ in range(rounds - 1):
                    t = bind("(bvxor %s (bvlshr %s %s))" % (y, t, _c(k, w)))
                x = t
        return x


def _lets(binds, body):
    for name, e in reversed(binds):
        body = "(let ((%s %s)) %s)" % (name, e, body)
    return body


@pytest.mark.parametrize("engine", _ENGINES)
@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_shapes_match_z3(seed: int, engine) -> None:
    g = _Shapes(seed)
    r, w = g.r, g.w
    f = g.chain(r.randint(1, 5))
    q = r.randrange(4)
    bs: list = []
    if q == 0:      # an inverse undoes f
        fx = g.apply(f, "x", bs)
        prop = "(not (= %s x))" % g.apply(g.inverse(f), fx, bs)
    elif q == 1:    # f is injective
        prop = "(not (= (= %s %s) (= x y)))" % (g.apply(f, "x", bs), g.apply(f, "y", bs))
    elif q == 2:    # a collision, if f has one
        prop = "(and (= %s %s) (distinct x y))" % (g.apply(f, "x", bs), g.apply(f, "y", bs))
    else:           # two chains of the same base
        h = f[:r.randint(0, len(f))] + g.chain(r.randint(0, 2))
        prop = "(not (= %s %s))" % (g.apply(f, "x", bs), g.apply(h, "x", bs))
    body = "(assert %s)\n" % _lets(bs, prop)
    _check([("x", w), ("y", w)], body, engine)


# ------------------------------------------------------------------ residues

class _Residues:
    """A checksum over bytes b_i: per step reduced against once at the end,
    with perturbations that make the two differ."""

    def __init__(self, seed: int):
        self.r = random.Random(seed)
        r = self.r
        # At most 12 input bits: z3, the oracle, cannot prove these identities
        # much past that (that is the point of the facts).
        self.bw = r.randint(2, 5)
        self.n = r.randint(1, max(1, 12 // self.bw))
        self.bytes = ["b%d" % i for i in range(self.n)]
        top = (1 << self.bw) + 2
        self.m = r.choice([r.randint(2, top), (1 << self.bw) - 1, 1 << r.randint(1, self.bw),
                           1, 0])

    def zext(self, v, frm, to):
        if to == frm:
            return v
        if to < frm:
            return "((_ extract %d 0) %s)" % (to - 1, v)
        return "((_ zero_extend %d) %s)" % (to - frm, v) if self.r.random() < 0.7 \
            else "(concat %s %s)" % (_c(0, to - frm), v)

    def need(self, v):
        return max(1, v.bit_length())

    def sums(self, deferred: bool, perturb: str):
        """(s1, s2, width): s1 = Σ b_i, s2 = Σ running s1, each reduced mod m."""
        r, bw, m, n = self.r, self.bw, self.m, self.n
        mm = m if m else 1 << (bw + 3)
        if deferred:
            # Wide enough for s2 = Σ (n - i)·b_i unless the perturbation asks.
            w = self.need((1 << bw) * n * (n + 1) // 2 + 1) + r.randint(0, 3)
            if perturb == "narrow":
                w = max(2, w - r.randint(1, 3))
            s1 = s2 = None
            for i, b in enumerate(self.bytes):
                t = self.zext(b, bw, w)
                if perturb == "extra" and i == 0:
                    t = "(bvadd %s %s)" % (t, _c(1, w))
                if r.random() < 0.2:      # a multiply by one that cannot wrap
                    t = "(bvmul %s %s)" % (t, _c(1, w))
                s1 = t if s1 is None else "(bvadd %s %s)" % (s1, t)
                s2 = s1 if s2 is None else "(bvadd %s %s)" % (s2, s1)
            mod = m + 1 if perturb == "modulus" else m
            return ("(bvurem %s %s)" % (s1, _c(mod, w)),
                    "(bvurem %s %s)" % (s2, _c(mod, w)), w)
        w = self.need(2 * mm) + r.randint(0, 2)
        if perturb == "narrow":
            w = max(2, w - r.randint(1, 2))
        s1 = s2 = _c(0, w)
        for b in self.bytes:
            s1 = "(bvurem (bvadd %s %s) %s)" % (s1, self.zext(b, bw, w), _c(m, w))
            s2 = "(bvurem (bvadd %s %s) %s)" % (s2, s1, _c(m, w))
        return s1, s2, w

    def low(self, v, w, k):
        return self.zext(v, w, k)


@pytest.mark.parametrize("engine", _ENGINES)
@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_residues_match_z3(seed: int, engine) -> None:
    g = _Residues(10_000 + seed)
    r = g.r
    perturb = r.choice(["none"] * 4 + ["narrow", "extra", "modulus", "drop"])
    d1, d2, dw = g.sums(True, perturb)
    m1, m2, mw = g.sums(False, perturb if perturb == "narrow" and r.random() < 0.5
                        else "none")
    # Each output is k bits of a residue; "drop" makes k one bit too few.
    k = g.need(max(g.m - 1, 1)) if g.m else min(dw, mw)
    k = min(k + r.randint(0, 2), dw, mw)
    if perturb == "drop":
        k = max(1, k - 1)
    a = [g.low(d1, dw, k), g.low(d2, dw, k)]
    b = [g.low(m1, mw, k), g.low(m2, mw, k)]
    if r.random() < 0.3:      # concat parts that do not line up
        cut = r.randint(1, 2 * k - 1)
        whole = "(concat %s %s)" % (b[1], b[0])
        rhs = "(concat ((_ extract %d %d) %s) ((_ extract %d 0) %s))" % (
            2 * k - 1, cut, whole, cut - 1, whole)
    else:
        rhs = "(concat %s %s)" % (b[1], b[0])
    body = "(assert (not (= (concat %s %s) %s)))\n" % (a[1], a[0], rhs)
    _check([(x, g.bw) for x in g.bytes], body, engine)


@pytest.mark.parametrize("engine", _ENGINES)
@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_residue_edges_match_z3(seed: int, engine) -> None:
    """One rule's boundary per case: two moduli, a reduced value against an
    unreduced one, an extract that drops a bit or starts above bit 0, and a
    constant multiply that wraps."""
    r = random.Random(20_000 + seed)
    xw = r.randint(2, 6)
    W = xw + r.randint(1, 5)
    m = r.randint(2, 1 << xw)

    def z(v):
        return "((_ zero_extend %d) %s)" % (W - xw, v)

    x, y = z("x"), z("y")
    R = "(bvurem (bvadd %s %s) %s)" % (x, y, _c(m, W))
    q = seed % 5
    if q == 0:      # reduced against unreduced: congruent, not equal
        a, b = R, "(bvadd (bvurem %s %s) %s)" % (x, _c(m, W), y)
    elif q == 1:    # an inner modulus that m may not divide
        m1 = r.choice([m * r.randint(1, 3), r.randint(2, 1 << xw)])
        a, b = R, "(bvurem (bvadd (bvurem %s %s) %s) %s)" % (x, _c(m1, W), y, _c(m, W))
    elif q == 2:    # an extract that may drop a value bit
        j = r.randint(0, W - 2)
        a, b = R, "((_ zero_extend %d) ((_ extract %d 0) %s))" % (W - 1 - j, j, R)
    elif q == 3:    # an extract above bit 0
        lo = r.randint(1, W - 1)
        a, b = R, "((_ zero_extend %d) ((_ extract %d %d) %s))" % (lo, W - 1, lo, R)
    else:           # a constant multiply that wraps at a narrow width
        c = r.randint(2, 9)
        Wn = r.randint(xw, W)
        xn = "((_ zero_extend %d) x)" % (Wn - xw) if Wn > xw else "x"
        prod = "(bvmul %s %s)" % (xn, _c(c, Wn))
        if Wn < W:
            prod = "((_ zero_extend %d) %s)" % (W - Wn, prod)
        a = "(bvurem %s %s)" % (prod, _c(m, W))
        b = "(bvurem (bvmul (bvurem %s %s) %s) %s)" % (x, _c(m, W), _c(c, W), _c(m, W))
    _check([("x", xw), ("y", xw)], "(assert (not (= %s %s)))\n" % (a, b), engine)


def test_fmix_and_fletcher_need_no_search() -> None:
    """The fwhdl-core instances these facts were built for decide in the
    bit-blaster, with no SAT search."""
    gen = _REPO / "tests/formal/smt2/fw_hdl/gen/mixed-proof"
    for name in ("fmix_inverse_w32", "fmix_injective_w32", "fletcher16_bytes16"):
        text = (gen / (name + ".smt2")).read_text()
        p = _run([str(_EXE)], text, extra={"DV_BB_STATS": "1"})
        assert p.stdout.split()[0] == "unsat", name + p.stdout + p.stderr
        m = re.search(r"word=(\d+)/(\d+)/(\d+)", p.stderr)
        assert m and sum(map(int, m.groups())) > 0, name + p.stderr
