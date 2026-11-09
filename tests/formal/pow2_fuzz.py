"""Differential fuzz for the bit-blast engine's power-of-two encodings
(docs/structural_reasoning_design.md, "Steps 1-2 in the bit-blast engine").

Random problems `c = a op b`. Two generators:

  * ``var``: b confined to powers of two (or not: a stray 0 or 3 must turn the
    fact off) by an `||` of equalities, `(b & (b-1)) == 0`, or `b == 1 << k`;
    b possibly narrower (zero-extended) or under an ITE; op any of
    bvudiv/bvmul/bvurem/bvsdiv/bvsrem/bvsmod.
  * ``const``: signed div/rem/mod by a constant +-2^k, MIN included.
  * ``pair``: a quotient and a remainder of the same operands (bvudiv/bvurem
    or bvsdiv/bvsrem), the divisor a variable, `1 << k`, a power-of-two
    variable or one that may be 0 -- the CDCL engine's `a == q * b + r` link
    (step 3 part 4) -- and products with a power-of-two factor.
  * ``slice``: a remainder `a % b` beside low slices of a (`extract` or
    `a & (2^j-1)`) fixed or bounded -- the CDCL engine's low-slice remainder
    link (axi_burst_bytes_64).

Each problem goes to ``dv-solve-smt2 --engine=E`` (bitblast by default; cdcl
runs with DV_NO_BITBLAST=1, so no fall-back hides a wrong answer) and z3; the
answers must agree (an ``unknown`` from cdcl is counted, not a mismatch), and a
sat model from dv-solve is re-checked by z3.

    python3 tests/formal/pow2_fuzz.py [--gen var|const|pair|slice] [--engine bitblast|cdcl]
                                      [--seed N] [--n N] [--bin PATH]

Mismatching problems are written to the current directory.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
from pathlib import Path

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"


def bv(v: int, w: int) -> str:
    return f"(_ bv{v % (1 << w)} {w})"


def gen_var(rng: random.Random) -> str:
    w = rng.choice([3, 4, 8, 13, 32, 63, 64])
    bw = rng.choice([w, w, max(1, w // 2)])           # b narrower: zero_extend
    dom = rng.sample([1 << i for i in range(bw)], rng.randint(1, min(4, bw)))
    r = rng.random()
    if r < 0.1:
        dom.append(0)
    elif r < 0.2:
        dom.append(rng.choice([3, 5, 6, 7]) % (1 << bw) or 3)
    style = rng.choice(["or", "and_or", "or", "clearlow", "shl"])
    lines = [f"(declare-fun a () (_ BitVec {w}))", f"(declare-fun b () (_ BitVec {bw}))",
             f"(declare-fun c () (_ BitVec {w}))", "(declare-fun p () Bool)"]
    eqs = " ".join(f"(= b {bv(c, bw)})" for c in dom)
    dc = f"(or {eqs} false)" if len(dom) > 1 else f"(= b {bv(dom[0], bw)})"
    if style == "and_or":
        dc = f"(and (bvuge a (_ bv0 {w})) {dc})"
    if style == "clearlow":
        dc = f"(= (bvand b (bvsub b {bv(1, bw)})) {bv(0, bw)})"
        if rng.random() < 0.5:
            dc = f"(and (not (= b {bv(0, bw)})) {dc})"
    if style == "shl":
        lines.append(f"(declare-fun k () (_ BitVec {bw}))")
        dc = f"(and (= b (bvshl {bv(1, bw)} k)) (bvule k {bv(rng.randrange(bw + 2), bw)}))"
    lines.append(f"(assert {dc})")
    bb = "b" if bw == w else f"((_ zero_extend {w - bw}) b)"
    if rng.random() < 0.2:
        bb = f"(ite p {bb} {bv(1 << rng.randrange(w), w)})"
    op = rng.choice(["bvudiv", "bvmul", "bvmul_l", "bvurem", "bvsdiv", "bvsrem", "bvsmod"])
    e = f"(bvmul {bb} a)" if op == "bvmul_l" else f"({op} a {bb})"
    lines.append(f"(assert (= c {e}))")
    m = (1 << w) - 1
    lo = rng.randrange(m + 1)
    hi = min(m, lo + rng.choice([0, 1, 5, 100, 1 << (w // 2)]))
    lines.append(f"(assert (bvuge c {bv(lo, w)}))")
    lines.append(f"(assert (bvule c {bv(hi, w)}))")
    if rng.random() < 0.5:
        cmp = rng.choice(["bvuge", "bvsge", "bvsle"])
        lines.append(f"(assert ({cmp} a {bv(rng.randrange(m + 1), w)}))")
    return "(set-logic QF_BV)\n" + "\n".join(lines) + "\n(check-sat)\n"


def gen_const(rng: random.Random) -> str:
    w = rng.choice([1, 2, 3, 5, 8, 16, 33, 63, 64])
    d = (1 << rng.randrange(w)) * rng.choice([1, -1])
    if rng.random() < 0.1:
        d = -(1 << (w - 1))
    op = rng.choice(["bvsdiv", "bvsrem", "bvsmod"])
    m = (1 << w) - 1
    lines = [f"(declare-fun a () (_ BitVec {w}))", f"(declare-fun c () (_ BitVec {w}))",
             f"(assert (= c ({op} a {bv(d, w)})))"]
    for v, cmp in (("a", rng.choice(["bvsge", "bvsle", "bvuge", None])),
                   ("c", rng.choice(["bvsge", "bvsle", "=", None]))):
        if cmp:
            lines.append(f"(assert ({cmp} {v} {bv(rng.randrange(m + 1), w)}))")
    if rng.random() < 0.3:
        pick = rng.choice([0, 1, m, 1 << (w - 1), (1 << (w - 1)) - 1, rng.randrange(m + 1)])
        lines.append(f"(assert (= a {bv(pick, w)}))")
    return "(set-logic QF_BV)\n" + "\n".join(lines) + "\n(check-sat)\n"


def gen_pair(rng: random.Random) -> str:
    w = rng.choice([4, 8, 16, 32, 63, 64])
    m = (1 << w) - 1
    lines = [f"(declare-fun {v} () (_ BitVec {w}))" for v in ("a", "b", "k", "q", "r", "x", "p")]
    style = rng.choice(["shl", "shl", "clearlow", "var", "var0"])
    if style == "shl":
        lines.append(f"(assert (= b (bvshl {bv(1, w)} k)))")
        lines.append(f"(assert (bvule k {bv(rng.randrange(w + 2), w)}))")
    elif style == "clearlow":
        lines.append(f"(assert (= (bvand b (bvsub b {bv(1, w)})) {bv(0, w)}))")
        if rng.random() < 0.5:
            lines.append(f"(assert (not (= b {bv(0, w)})))")
    elif style == "var":
        lines.append(f"(assert (bvuge b {bv(1, w)}))")
    sg = rng.random() < 0.5
    dv, rm = ("bvsdiv", "bvsrem") if sg else ("bvudiv", "bvurem")
    # The divisor written again (not shared) half the time: `1 << k` twice.
    b2 = f"(bvshl {bv(1, w)} k)" if style == "shl" and rng.random() < 0.5 else "b"
    lines.append(f"(assert (= q ({dv} a b)))")
    if rng.random() < 0.8:
        lines.append(f"(assert (= r ({rm} a {b2})))")
    else:
        lines.append(f"(assert (= ({rm} a {b2}) {bv(rng.randrange(m + 1), w)}))")
    if rng.random() < 0.5:
        lines.append(f"(assert (= p (bvmul x {rng.choice(['b', b2])})))")
        lines.append(f"(assert (bvuge p {bv(rng.randrange(m + 1), w)}))")
    for v in ("q", "r", "a", "p"):
        if rng.random() < 0.4:
            cmp = rng.choice(["bvuge", "bvule", "bvsge", "bvsle", "="])
            lines.append(f"(assert ({cmp} {v} {bv(rng.randrange(m + 1), w)}))")
    return "(set-logic QF_BV)\n" + "\n".join(lines) + "\n(check-sat)\n"


def gen_slice(rng: random.Random) -> str:
    w = rng.choice([8, 16, 32, 63, 64])
    m = (1 << w) - 1
    lines = [f"(declare-fun {v} () (_ BitVec {w}))" for v in ("a", "b", "k", "r")]
    style = rng.choice(["shl", "shl", "const", "var", "var0"])
    if style == "shl":
        lines.append(f"(assert (= b (bvshl {bv(1, w)} k)))")
        lines.append(f"(assert (bvule k {bv(rng.randrange(w + 2), w)}))")
    elif style == "const":
        lines.append(f"(assert (= b {bv(1 << rng.randrange(w), w)}))")
    elif style == "var":
        lines.append(f"(assert (bvuge b {bv(1, w)}))")
        lines.append(f"(assert (bvule b {bv(rng.randrange(1, 1 << min(w, 13)), w)}))")
    if rng.random() < 0.7:
        lines.append(f"(assert (= r (bvurem a b)))")
    else:
        lines.append(f"(assert (= (bvurem a b) {bv(rng.randrange(1 << min(w, 6)), w)}))")
    for _ in range(rng.randint(1, 3)):
        j = rng.randrange(1, w)
        mask = (1 << j) - 1
        sl = (f"((_ extract {j - 1} 0) a)" if rng.random() < 0.5 else None)
        c = rng.randrange(mask + 1)
        if rng.random() < 0.3:
            c &= ~((1 << rng.randrange(j + 1)) - 1)   # aligned low bits
        if sl:
            cmp = rng.choice(["=", "=", "bvuge", "bvule"])
            lines.append(f"(assert ({cmp} {sl} {bv(c, j)}))")
        else:
            lines.append(f"(assert (= (bvand a {bv(mask, w)}) {bv(c, w)}))")
    for v in ("r", "a"):
        if rng.random() < 0.4:
            cmp = rng.choice(["bvuge", "bvule", "="])
            lines.append(f"(assert ({cmp} {v} {bv(rng.randrange(m + 1), w)}))")
    return "(set-logic QF_BV)\n" + "\n".join(lines) + "\n(check-sat)\n"


def run(cmd: list[str], text: str, env: dict | None = None) -> str:
    return subprocess.run(cmd, input=text, capture_output=True, text=True, timeout=60,
                          env=env).stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", choices=["var", "const", "pair", "slice"], default="var")
    ap.add_argument("--engine", choices=["bitblast", "cdcl"], default="bitblast")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--n", type=int, default=300)
    ap.add_argument("--bin", default=str(_EXE))
    args = ap.parse_args()
    gen = {"var": gen_var, "const": gen_const, "pair": gen_pair, "slice": gen_slice}[args.gen]
    names = {"var": "(a b c p)", "const": "(a c)", "pair": "(a b k q r x p)", "slice": "(a b k r)"}[args.gen]
    env = dict(os.environ, DV_NO_BITBLAST="1") if args.engine == "cdcl" else None
    rng = random.Random(args.seed)
    bad = unknown = 0
    for i in range(args.n):
        t = gen(rng)
        z = run(["z3", "-in", "-T:20"], t).split()[0]
        try:
            d = run([args.bin, f"--engine={args.engine}"], t + f"(get-value {names})\n", env)
        except subprocess.TimeoutExpired:
            d = "unknown"
        dr = d.split()[0] if d else "?"
        if dr == "unknown" and args.engine == "cdcl":
            unknown += 1
            continue
        if dr != z:
            bad += 1
            Path(f"pow2_bad_{args.gen}_{args.seed}_{i}.smt2").write_text(t)
            print("MISMATCH", i, dr, z)
            continue
        if dr == "sat":
            vals = re.findall(r"\((\w) (#[xb][0-9a-fA-F]+|\(_ bv\d+ \d+\)|true|false)\)", d)
            chk = (t.replace("(check-sat)\n", "")
                   + "".join(f"(assert (= {n} {v}))\n" for n, v in vals) + "(check-sat)\n")
            if not vals or run(["z3", "-in"], chk).split()[0] != "sat":
                bad += 1
                Path(f"pow2_badmodel_{args.gen}_{args.seed}_{i}.smt2").write_text(chk)
                print("BAD MODEL", i)
    print(f"{args.n} cases, {bad} bad, {unknown} unknown")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
