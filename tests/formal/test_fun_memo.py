"""define-fun applications memoized within a command (docs/fw_hdl_implementation_plan.md, piece 5.0).

The front end translates a define-fun body once per distinct call: a call
whose arguments are all global variables or translated let bindings is keyed
by the function and those identities, and reused for the rest of the command.
That is sound only if a body's translation depends on nothing but its
arguments' values, which is what lexical scoping (B78) gives.

The generator aims at the ways a key could be wrong: functions calling
functions with their parameters, let names and globals as arguments; one name
pool for globals, parameters and lets, so parameters shadow globals and lets
shadow both; the same call text under two let bindings of a name ("twins");
repeated identical calls; Bool functions in both polarities; array arguments
(never memoized). Two checks:

 - pinned: the globals are fixed, z3 gives each term's value, and dv-solve
   must find `(not (= term value))` unsat, one term at a time and all the
   terms in one assertion (so memo entries are shared across terms);
 - free: random assertions, the verdict against z3, a sat model replayed in z3.

Scale with MEMO_N (default 200); MEMO_FIRST moves the window.
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
_N = int(os.environ.get("MEMO_N", "200"))
_FIRST = int(os.environ.get("MEMO_FIRST", "0"))

pytestmark = [
    pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built"),
    pytest.mark.skipif(_Z3 is None, reason="z3 not on PATH"),
]

_NAMES = ["x", "y", "state", "t"]         # globals, parameters and lets alike
_WIDTHS = [4, 8]
_ARR = "(Array (_ BitVec 4) (_ BitVec 8))"


class _Gen:
    def __init__(self, seed: int):
        self.r = random.Random(seed)
        r = self.r
        # Globals: some pool names (so parameters shadow them) and fresh ones.
        self.globals: dict[str, int] = {}
        for n in r.sample(_NAMES, r.randint(1, 3)) + ["g0", "g1"]:
            self.globals[n] = r.choice(_WIDTHS)
        self.funs: list[tuple[str, list[int], str]] = []   # name, param widths, ret ("bool" or width)
        self.lines = ["(set-logic QF_ABV)"]
        self.lines += ["(declare-const %s (_ BitVec %d))" % (n, w) for n, w in self.globals.items()]
        self.lines.append("(declare-const m %s)" % _ARR)
        self.lines.append("(define-fun rd ((a %s) (i (_ BitVec 4))) (_ BitVec 8) (select a i))"
                          % _ARR)
        for i in range(r.randint(3, 7)):
            self._define(i)

    # -- terms ------------------------------------------------------------
    def lit(self, w: int) -> str:
        return "(_ bv%d %d)" % (self.r.getrandbits(w), w)

    def fit(self, t: str, w: int, to: int) -> str:
        if w == to:
            return t
        if w > to:
            return "((_ extract %d 0) %s)" % (to - 1, t)
        return "((_ zero_extend %d) %s)" % (to - w, t)

    def leaf(self, w: int, env: dict) -> str:
        """A name of width w if there is one in scope, else a fitted one or a literal."""
        r = self.r
        names = [n for n, ww in env.items() if ww == w]
        if names and r.random() < 0.85:
            return r.choice(names)
        if env and r.random() < 0.6:
            n = r.choice(list(env))
            return self.fit(n, env[n], w)
        return self.lit(w)

    def call(self, w: int, d: int, env: dict) -> str | None:
        fs = [f for f in self.funs if f[2] == w]
        if not fs:
            return None
        name, ps, _ = self.r.choice(fs)
        # Arguments are mostly bare names, so the memo engages.
        args = [self.leaf(pw, env) if self.r.random() < 0.75 else self.bv(pw, d - 1, env)
                for pw in ps]
        return "(%s %s)" % (name, " ".join(args))

    def bv(self, w: int, d: int, env: dict) -> str:
        r = self.r
        if d <= 0:
            return self.leaf(w, env)
        c = r.random()
        if c < 0.12:
            # Twins: one call text under two bindings of a name; a memo keyed
            # by the text, or by the wrong binding, gives both the same value.
            fs = [f for f in self.funs if f[2] == w]
            if fs:
                name, ps, _ = r.choice(fs)
                n, nw = r.choice(_NAMES), r.choice(ps)
                inner = {**env, n: nw}
                args = [n if pw == nw and r.random() < 0.8 else self.leaf(pw, inner)
                        for pw in ps]
                t = "(%s %s)" % (name, " ".join(args))
                a = self.leaf(nw, env)
                b = self.leaf(nw, env)
                if b == a:
                    b = self.lit(nw)
                op = r.choice(["bvsub", "bvxor", "concat"])
                pair = "(let ((%s %s)) %s) (let ((%s %s)) %s)" % (n, a, t, n, b, t)
                if op == "concat":
                    return "((_ extract %d 0) (concat %s))" % (w - 1, pair) if r.random() < 0.5 \
                        else "((_ extract %d %d) (concat %s))" % (2 * w - 1, w, pair)
                return "(%s %s)" % (op, pair)
        if c < 0.4:
            t = self.call(w, d, env)
            if t:
                return t
        if c < 0.55:
            return self.let(lambda e: self.bv(w, d - 1, e), d, env)
        if c < 0.75:
            op = r.choice(["bvadd", "bvxor", "bvand", "bvsub", "bvor", "bvmul"])
            return "(%s %s %s)" % (op, self.bv(w, d - 1, env), self.bv(w, d - 1, env))
        if c < 0.85:
            return "(ite %s %s %s)" % (self.bool(d - 1, env), self.bv(w, d - 1, env),
                                       self.bv(w, d - 1, env))
        if c < 0.9 and w == 8:
            return "(rd m %s)" % self.leaf(4, env)
        if c < 0.95:
            return "(bvnot %s)" % self.bv(w, d - 1, env)
        return self.leaf(w, env)

    def bool(self, d: int, env: dict) -> str:
        r = self.r
        c = r.random()
        if d > 0 and c < 0.3:
            fs = [f for f in self.funs if f[2] == "bool"]
            if fs:
                name, ps, _ = r.choice(fs)
                t = "(%s %s)" % (name, " ".join(self.leaf(pw, env) for pw in ps))
                return t if r.random() < 0.5 else "(not %s)" % t
        if d > 0 and c < 0.4:
            return self.let(lambda e: self.bool(d - 1, e), d, env)
        if d > 0 and c < 0.5:
            return "(%s %s %s)" % (r.choice(["and", "or", "xor", "="]), self.bool(d - 1, env),
                                   self.bool(d - 1, env))
        w = r.choice(_WIDTHS)
        op = r.choice(["=", "bvult", "bvsle", "distinct"])
        return "(%s %s %s)" % (op, self.bv(w, d - 1, env), self.bv(w, d - 1, env))

    def let(self, body, d: int, env: dict) -> str:
        r = self.r
        inner = dict(env)
        binds = []
        for n in r.sample(_NAMES, r.randint(1, 2)):
            w = r.choice(_WIDTHS)
            # Often rebind a name to another name: the same call text, a new key.
            v = self.leaf(w, env) if r.random() < 0.6 else self.bv(w, d - 1, env)
            binds.append("(%s %s)" % (n, v))
            inner[n] = w
        return "(let (%s) %s)" % (" ".join(binds), body(inner))

    def _define(self, i: int) -> None:
        r = self.r
        params = {}
        for n in r.sample(_NAMES, r.randint(1, 3)):
            params[n] = r.choice(_WIDTHS)
        # The body sees its parameters and the globals (shadowed by parameters).
        env = {**self.globals, **params}
        sig = " ".join("(%s (_ BitVec %d))" % (n, w) for n, w in params.items())
        if r.random() < 0.25:
            self.lines.append("(define-fun f%d (%s) Bool %s)" % (i, sig, self.bool(3, env)))
            self.funs.append(("f%d" % i, list(params.values()), "bool"))
        else:
            w = r.choice(_WIDTHS)
            self.lines.append("(define-fun f%d (%s) (_ BitVec %d) %s)" % (i, sig, w, self.bv(w, 3, env)))
            self.funs.append(("f%d" % i, list(params.values()), w))

    def term(self) -> str:
        if self.r.random() < 0.3:
            return self.bool(4, dict(self.globals))
        return self.bv(self.r.choice(_WIDTHS), 4, dict(self.globals))


def _run(cmd: list[str], text: str) -> str:
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    p = subprocess.run(cmd, input=text, capture_output=True, text=True, timeout=120, env=env)
    return p.stdout + p.stderr


def _values(out: str) -> list[str]:
    return re.findall(r"(#b[01]+|#x[0-9a-fA-F]+|true|false)\)", out)


def _pins(g: _Gen, r: random.Random) -> str:
    s = "".join("(assert (= %s %s))\n" % (n, g.lit(w)) for n, w in g.globals.items())
    for k in r.sample(range(16), 4):
        s += "(assert (= (select m #x%x) %s))\n" % (k, g.lit(8))
    return s


@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_memoized_translation_matches_z3_values(seed: int) -> None:
    g = _Gen(seed)
    r = random.Random(seed ^ 0x5EED)
    head = "\n".join(g.lines) + "\n" + _pins(g, r)
    terms = [g.term() for _ in range(8)]
    z3 = _run([_Z3, "-in"], head + "(check-sat)\n(get-value (%s))\n" % " ".join(terms))
    assert z3.split()[0] == "sat", z3 + head
    vals = _values(z3)
    assert len(vals) == len(terms), z3
    # z3's value is unique only where the term does not read an unpinned array
    # element: ask z3 whether another value is possible, and keep the unique ones.
    uniq = []
    for t, v in zip(terms, vals):
        alt = _run([_Z3, "-in"], head + "(assert (not (= %s %s)))(check-sat)\n" % (t, v))
        if alt.split()[0] == "unsat":
            uniq.append((t, v))
    one = "".join("(push 1)(assert (not (= %s %s)))(check-sat)(pop 1)\n" % tv for tv in uniq)
    together = "(assert (or %s))(check-sat)\n" % " ".join(
        "(not (= %s %s))" % tv for tv in uniq) if uniq else ""
    got = _run([str(_EXE)], head + one + together).split()
    want = ["unsat"] * (len(uniq) + (1 if uniq else 0))
    assert got == want, "dv-solve %s\n%s\n%s%s" % (got, head, one, together)


@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_memoized_verdicts_match_z3(seed: int) -> None:
    g = _Gen(100_000 + seed)
    text = "\n".join(g.lines) + "\n"
    text += "".join("(assert %s)\n" % g.bool(4, dict(g.globals))
                    for _ in range(g.r.randint(1, 3)))
    text += "(check-sat)\n"
    want = _run([_Z3, "-in"], text).split()[0]
    if want not in ("sat", "unsat"):
        pytest.skip("z3: %s" % want)
    names = list(g.globals)
    out = _run([str(_EXE)], text + "(get-value (%s))\n" % " ".join(names))
    got = out.split()[0]
    assert got == want, "dv-solve %s, z3 %s\n%s\n%s" % (got, want, out, text)
    if got != "sat":
        return
    vals = _values(out)
    assert len(vals) == len(names), out
    pins = "".join("(assert (= %s %s))\n" % nv for nv in zip(names, vals))
    check = _run([_Z3, "-in"], text.replace("(check-sat)\n", pins + "(check-sat)\n"))
    assert check.split()[0] == "sat", "model fails in z3\n%s\n%s" % (pins, text)


def test_memo_engages() -> None:
    """A Yosys-style chain, each cell reading the previous one twice: 2^60
    expansions without the memo. Arguments are names (a global, a let), as
    Yosys writes them; a compound argument is not keyed."""
    lines = ["(set-logic QF_BV)", "(declare-const s (_ BitVec 16))",
             "(define-fun c0 ((state (_ BitVec 16))) (_ BitVec 16) state)"]
    lines += ["(define-fun c%d ((state (_ BitVec 16))) (_ BitVec 16) "
              "(bvxor ((_ rotate_left 1) (c%d state)) (bvand (c%d state) #x00ff)))"
              % (i, i - 1, i - 1) for i in range(1, 61)]
    lines += ["(assert (let ((n (bvnot s))) (= (c60 s) (c60 n))))", "(check-sat)"]
    out = _run([str(_EXE)], "\n".join(lines) + "\n")
    assert out.split()[0] in ("sat", "unsat"), out
    z3 = _run([_Z3, "-in"], "\n".join(lines) + "\n")
    assert out.split()[0] == z3.split()[0], (out, z3)
