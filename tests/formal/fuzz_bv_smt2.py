#!/usr/bin/env python3
"""Deterministic generative fuzzer for QF_BV differential cross-checking.

Hand-authored fixtures cover the shapes we *think* of; a typed random generator
covers the combinatorial op space we don't. ``generate_problem(seed)`` emits a
self-contained, **type-correct** QF_BV problem (every operand width matches, so
z3 and dv-solve parse the identical text) built from the operators dv-solve
supports — including wide (>64-bit) vars/ops from Phase W1. The same seed always
produces the same problem, so a failing case is fully reproducible.

Constants use only the ``(_ bvK W)`` form with ``K`` fitting in 63 bits (W1's
sound constant range), so a disagreement with z3 is a real solver bug, not the
documented wide-``#x``-literal limitation.

Used by ``test_fuzz_cross_check.py``. Also runnable standalone to eyeball or
persist a sample:  ``python3 tests/formal/fuzz_bv_smt2.py [seed]``
"""
from __future__ import annotations

import random

# Widths the generator draws from — a mix of narrow and wide (<=128 =
# SMT2_MAX_BV_BITS) so Phase-W1 wide paths get exercised alongside the 64-bit
# core. Small widths dominate so z3 usually returns a definite answer.
_WIDTHS = [1, 2, 3, 4, 8, 8, 12, 16, 16, 24, 32, 32, 48, 64, 65, 96, 128]

_BINOPS = ["bvand", "bvor", "bvxor", "bvadd", "bvsub"]
_CMPS = ["=", "distinct", "bvult", "bvule", "bvugt", "bvuge"]


def _split_args(s: str) -> list[str] | None:
    """Split `(op a b c)` into [a, b, c], respecting nesting. None if malformed."""
    if not s.startswith("(") or not s.endswith(")"):
        return None
    body = s[1:-1]
    sp = body.find(" ")
    if sp < 0:
        return None
    body = body[sp + 1:]
    args, depth, start = [], 0, 0
    for i, ch in enumerate(body):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == " " and depth == 0:
            if i > start:
                args.append(body[start:i])
            start = i + 1
    if len(body) > start:
        args.append(body[start:])
    return args or None


class _Gen:
    def __init__(self, rng: random.Random):
        self.rng = rng
        self.vars: dict[str, int] = {}   # name -> width
        self._n = 0

    def _new_var(self, width: int) -> str:
        name = f"v{self._n}_{width}"
        self._n += 1
        self.vars[name] = width
        return name

    def _var_of(self, width: int) -> str:
        """A variable of exactly `width` bits — reuse one ~half the time."""
        candidates = [n for n, w in self.vars.items() if w == width]
        if candidates and self.rng.random() < 0.5:
            return self.rng.choice(candidates)
        return self._new_var(width)

    def _const(self, width: int) -> str:
        # Keep K within 63 bits (W1's sound constant range) and within 2**width.
        hi = (1 << width) - 1
        hi = min(hi, (1 << 63) - 1)
        return f"(_ bv{self.rng.randint(0, hi)} {width})"

    def term(self, width: int, depth: int) -> str:
        """A BV term of exactly `width` bits."""
        if depth <= 0 or self.rng.random() < 0.35:
            return self._const(width) if self.rng.random() < 0.4 else self._var_of(width)

        choice = self.rng.random()
        if choice < 0.45:                                   # same-width binop
            op = self.rng.choice(_BINOPS)
            return f"({op} {self.term(width, depth-1)} {self.term(width, depth-1)})"
        if choice < 0.55:                                   # bvnot
            return f"(bvnot {self.term(width, depth-1)})"
        if choice < 0.70:                                   # ite
            return (f"(ite {self.pred(depth-1)} "
                    f"{self.term(width, depth-1)} {self.term(width, depth-1)})")
        if choice < 0.78 and width >= 2:                    # concat halves
            a = self.rng.randint(1, width - 1)
            b = width - a
            return f"(concat {self.term(a, depth-1)} {self.term(b, depth-1)})"
        if choice < 0.90 and width >= 2:                    # zero/sign-extend to width
            src = self.rng.randint(1, width - 1)
            ext = "sign_extend" if self.rng.random() < 0.5 else "zero_extend"
            return f"((_ {ext} {width - src}) {self.term(src, depth-1)})"
        # extract `width` bits out of a wider (<=128) term
        room = self.rng.randint(0, min(128 - width, 16))
        src = width + room
        lo = self.rng.randint(0, src - width)
        hi = lo + width - 1
        return f"((_ extract {hi} {lo}) {self.term(src, depth-1)})"

    def disj_over_var(self, depth: int) -> str:
        """An OR whose leaves are direct `var op const` / `var op var` compares.

        The generic `or` production below builds disjunctions over arbitrary
        *terms*, which almost never reduce to the shape `_flatten_or` in
        zsp_compile.c recognises -- so they route to a Boolean guard and never
        reach the DisjClause propagator. This production targets that
        propagator (and its interval-hull pass) head on: it is the `x inside
        {a, b, c}` family that Verilator emits constantly.

        Deliberately mixes constants with same-width vars so both the var-const
        arm and the var-var mirror arm of _disj_var_range get exercised.
        """
        w = self.rng.choice(_WIDTHS)
        v = self._var_of(w)
        n = self.rng.randint(2, 5)
        parts = []
        for _ in range(n):
            op = self.rng.choice(_CMPS)
            rhs = self._const(w) if self.rng.random() < 0.7 else self._var_of(w)
            # Put the target var on the right sometimes, to hit the mirror arm.
            parts.append(f"({op} {rhs} {v})" if self.rng.random() < 0.25
                         else f"({op} {v} {rhs})")
        return f"(or {' '.join(parts)})"

    def reify(self, p: str, depth: int) -> str:
        """Rewrite a Bool predicate into Verilator's reified 1-bit encoding.

        Verilator does not emit Boolean `and`/`or`: it emits
        `(= #b1 (bvand #b1 (bvor (__Vbv A) (__Vbv B))))`. That is a *different
        translator path* from the Boolean one -- the reified-bvor normalization
        and the top-level reified-conjunction assert split both live there and
        neither is reachable from the plain `(assert (or ...))` shapes above.
        Fuzzing only the Boolean form leaves them untested.

        Structural rewrite only: `(and ..)` -> bvand, `(or ..)` -> bvor,
        anything else -> `(__Vbv <pred>)`. Randomly re-inserts the `#b1` masks
        Verilator sprinkles through the real transcripts.
        """
        def mask(s: str) -> str:
            if self.rng.random() < 0.25:
                return (f"(bvand #b1 {s})" if self.rng.random() < 0.5
                        else f"(bvand {s} #b1)")
            return s

        def walk(s: str, d: int) -> str:
            if d > 0 and s.startswith("(and ") or d > 0 and s.startswith("(or "):
                op = "bvand" if s.startswith("(and ") else "bvor"
                parts = _split_args(s)
                if parts is not None and len(parts) >= 2:
                    return mask(f"({op} " + " ".join(walk(a, d - 1)
                                                     for a in parts) + ")")
            return mask(f"(__Vbv {s})")

        return f"(= #b1 {walk(p, depth)})"

    def pred(self, depth: int) -> str:
        """A Bool-valued predicate."""
        if depth <= 0 or self.rng.random() < 0.4:
            w = self.rng.choice(_WIDTHS)
            op = self.rng.choice(_CMPS)
            return f"({op} {self.term(w, depth-1)} {self.term(w, depth-1)})"
        c = self.rng.random()
        if c < 0.3:
            return f"(not {self.pred(depth-1)})"
        if c < 0.45:
            return self.disj_over_var(depth - 1)
        op = "and" if c < 0.72 else "or"
        n = self.rng.randint(2, 3)
        return f"({op} {' '.join(self.pred(depth-1) for _ in range(n))})"


def generate_problem(seed: int) -> str:
    rng = random.Random(seed)
    g = _Gen(rng)
    depth = rng.randint(2, 4)
    n_asserts = rng.randint(1, 3)
    # Each assert is emitted either as a plain Bool predicate or in Verilator's
    # reified 1-bit form; both translator paths therefore get fuzzed.
    asserts = [(g.pred(depth), rng.random() < 0.45) for _ in range(n_asserts)]
    lines = ["(set-logic QF_BV)",
             "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))"]
    for name, width in g.vars.items():
        lines.append(f"(declare-const {name} (_ BitVec {width}))")
    for a, reified in asserts:
        lines.append(f"(assert {g.reify(a, depth) if reified else a})")
    lines.append("(check-sat)")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    import sys
    s = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    print(generate_problem(s), end="")
