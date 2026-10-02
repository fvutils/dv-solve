#!/usr/bin/env python3
"""Deterministic generative fuzzer for QF_BV differential cross-checking.

Hand-authored fixtures cover the shapes we *think* of; a typed random generator
covers the combinatorial op space we don't. ``generate_problem(seed)`` emits a
self-contained, **type-correct** QF_BV problem (every operand width matches, so
z3 and dv-solve parse the identical text) built from the operators dv-solve
supports — including wide (>64-bit) vars/ops from Phase W1. The same seed always
produces the same problem, so a failing case is fully reproducible.

By default constants use only the ``(_ bvK W)`` form with ``K`` fitting in 63
bits. ``generate_problem(seed, wide_lits=True)`` instead draws constants over
each width's full range -- including values >= 2^64 for >64-bit widths
(Phase W2) and boundary values around 2^63 / 2^64 / 2^W -- and spells them as
``#x…``, ``#b…`` or ``(_ bvK W)`` (occasionally with K >= 2^W, which SMT-LIB /
z3 reduce mod 2^W).

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
    def __init__(self, rng: random.Random, wide_lits: bool = False):
        self.rng = rng
        self.wide_lits = wide_lits
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
        if self.wide_lits:
            return self._wide_const(width)
        # Keep K within 63 bits (W1's sound constant range) and within 2**width.
        hi = (1 << width) - 1
        hi = min(hi, (1 << 63) - 1)
        return f"(_ bv{self.rng.randint(0, hi)} {width})"

    def _wide_const(self, width: int) -> str:
        """Full-range constant of `width` bits, in a random literal spelling."""
        r = self.rng
        top = (1 << width) - 1
        edges = [0, 1, top, top - 1, 1 << (width - 1), (1 << (width - 1)) - 1]
        for e in (63, 64):                    # the int64 / uint64 cliffs
            if e < width:
                edges += [1 << e, (1 << e) - 1, (1 << e) + 1]
        k = r.choice(edges) if r.random() < 0.4 else r.randint(0, top)
        form = r.random()
        if form < 0.35 and width % 4 == 0:
            return f"#x{k:0{width // 4}x}"
        if form < 0.65:
            return f"#b{k:0{width}b}"
        if r.random() < 0.1:                  # unreduced K: value is K mod 2^W
            k += (1 << width) * r.randint(1, 3)
        return f"(_ bv{k} {width})"

    def term(self, width: int, depth: int) -> str:
        """A BV term of exactly `width` bits."""
        if depth <= 0 or self.rng.random() < 0.35:
            return self._const(width) if self.rng.random() < 0.4 else self._var_of(width)

        choice = self.rng.random()
        if choice < 0.45:                                   # same-width binop
            if self.rng.random() < 0.15:
                # bvashr (arithmetic shift right): the amount is mostly a small
                # constant, sometimes >= the width (all sign bits), sometimes a
                # term (an arbitrary, usually huge, unsigned amount).
                if self.rng.random() < 0.7:
                    k = self.rng.randint(0, width + 2)
                    k = min(k, (1 << width) - 1)
                    amt = f"(_ bv{k} {width})"
                else:
                    amt = self.term(width, depth - 1)
                return f"(bvashr {self.term(width, depth-1)} {amt})"
            op = self.rng.choice(_BINOPS)
            return f"({op} {self.term(width, depth-1)} {self.term(width, depth-1)})"
        if choice < 0.55:                                   # bvnot / bvneg
            # bvneg was never generated before 2026-09-30, which hid a CDCL
            # wrong-`unsat` on every `(= (bvneg x) K)` (it lowered to an
            # unwrapped integer negation).
            op = "bvnot" if self.rng.random() < 0.5 else "bvneg"
            return f"({op} {self.term(width, depth-1)})"
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
        dvs_compile.c recognises -- so they route to a Boolean guard and never
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


def generate_problem(seed: int, wide_lits: bool = False) -> str:
    rng = random.Random(seed)
    g = _Gen(rng, wide_lits)
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
