"""Reduced-width problem families for the uniformity harness.

Each family keeps the constraint shape of a simbench bench (or of a sampler
layer in the 1.0 plan) at widths small enough for z3 to enumerate every
solution. They complement the corpus (the real per-bench SMT-LIB, at full
width), which is often too large to enumerate.

Plain QF_BV, written the way a person would; the corpus has Verilator's
encoding. Routing in the front end depends on the op mix, so both matter.
"""
from __future__ import annotations

from uniformity import Problem


def _bv(name: str, w: int) -> str:
    return f"(declare-fun {name} () (_ BitVec {w}))"


def _c(v: int, w: int) -> str:
    return f"(_ bv{v} {w})"


def _zx(e: str, by: int) -> str:
    return f"((_ zero_extend {by}) {e})"


def _popcount(x: str, w: int, ow: int) -> str:
    bits = [_zx(f"((_ extract {i} {i}) {x})", ow - 1) for i in range(w)]
    acc = bits[0]
    for b in bits[1:]:
        acc = f"(bvadd {acc} {b})"
    return acc


def _distinct(xs: list[str]) -> str:
    return "(distinct " + " ".join(xs) + ")"


def _P(name, decls, asserts, names, note, bench=""):
    return Problem(name=f"fam.{name}", body=["(set-logic QF_BV)", *decls,
                                             *[f"(assert {a})" for a in asserts]],
                   names=names, note=note, source=f"families:{bench or name}")


def all_problems() -> list[Problem]:
    P = []

    # --- controls: every sampler should be uniform here -------------------
    P.append(_P("range_1000", [_bv("x", 10)], [f"(bvult x {_c(1000, 10)})"],
                ["x"], "plain contiguous range (control)", "construct.range"))
    P.append(_P("inside_holes", [_bv("x", 8)],
                [f"(or (and (bvuge x {_c(10, 8)}) (bvule x {_c(20, 8)})) "
                 f"(and (bvuge x {_c(100, 8)}) (bvule x {_c(150, 8)})))"],
                ["x"], "x inside {[10:20],[100:150]}: domain with a hole",
                "construct.inside_ranges"))
    P.append(_P("signed_range", [_bv("s", 6)],
                [f"(bvsge s (bvneg {_c(10, 6)}))", f"(bvslt s {_c(5, 6)})"],
                ["s"], "signed range [-10:4]", "construct.signed_mix"))

    # --- bit-level: today's bitblast + _diversify territory ---------------
    P.append(_P("bit_mask", [_bv("x", 8)], [f"(= (bvand x #x0f) #x05)"],
                ["x"], "(x & 0x0f) == 5: solutions spread by the high bits",
                "arith.bitslice"))
    P.append(_P("countones", [_bv("v", 8), _bv("w", 6)],
                [f"(= {_popcount('v', 8, 4)} {_c(3, 4)})",
                 f"(let ((c {_popcount('w', 6, 3)})) "
                 f"(and (bvuge c {_c(1, 3)}) (bvule c {_c(2, 3)})))",
                 "(= ((_ extract 0 0) w) #b0)"],
                ["v", "w"], "$countones(v)==3; $countones(w) inside {[1:2]}; w[0]==0",
                "arith.countones"))
    P.append(_P("xor_parity", [_bv("x", 8)],
                ["(= ((_ extract 0 0) (bvxor x (bvlshr x #x01) (bvlshr x #x02) "
                 "(bvlshr x #x03) (bvlshr x #x04) (bvlshr x #x05) (bvlshr x #x06) "
                 "(bvlshr x #x07))) #b1)",
                 "(distinct ((_ extract 7 4) x) #x0)"],
                ["x"], "^x == 1 and x[7:4] != 0", "construct.xor_parity"))

    # --- ordering / coupling ---------------------------------------------
    P.append(_P("a_lt_b", [_bv("a", 6), _bv("b", 6)], ["(bvult a b)"],
                ["a", "b"], "a < b: ordering coupling", "array.array_ordering"))
    P.append(_P("dep_range", [_bv("a", 7), _bv("b", 7)],
                [f"(bvule a {_c(80, 7)})", "(bvuge b a)",
                 f"(bvule b (bvadd a {_c(10, 7)}))"],
                ["a", "b"], "b in [a:a+10], a <= 80: value-dependent range",
                "control.implication_set"))
    P.append(_P("eq_chain", [_bv("a", 8), _bv("b", 8), _bv("c", 8)],
                [f"(= a (bvadd b {_c(1, 8)}))", f"(= b (bvadd c {_c(1, 8)}))",
                 f"(bvult c {_c(20, 8)})"],
                ["a", "b", "c"], "a==b+1, b==c+1: one free variable", "construct.eq_chain"))
    P.append(_P("sorted_gap", [_bv(f"d{i}", 4) for i in range(4)],
                [f"(bvuge {_zx(f'd{i}', 1)} (bvadd {_zx(f'd{i-1}', 1)} {_c(2, 5)}))"
                 for i in range(1, 4)],
                [f"d{i}" for i in range(4)], "d[i] >= d[i-1]+2 over 4 bits (sorted = k-subset)",
                "array.sorted_increasing"))
    P.append(_P("addr_chain",
                [_bv(n, 4) for n in ("b0", "s0", "b1", "s1")],
                [f"(bvuge s0 {_c(1, 4)})", f"(bvuge s1 {_c(1, 4)})",
                 f"(bvule (bvadd {_zx('b0', 1)} {_zx('s0', 1)}) {_zx('b1', 1)})",
                 f"(bvule (bvadd {_zx('b1', 1)} {_zx('s1', 1)}) {_c(12, 5)})"],
                ["b0", "s0", "b1", "s1"], "two ordered regions end_i <= base_{i+1}",
                "memmap.addr_map_64"))

    # --- arithmetic -------------------------------------------------------
    P.append(_P("sum_eq", [_bv(f"a{i}", 4) for i in range(4)],
                [f"(bvult a{i} {_c(10, 4)})" for i in range(4)] +
                [f"(= (bvadd {_zx('a0', 4)} {_zx('a1', 4)} {_zx('a2', 4)} {_zx('a3', 4)}) "
                 f"{_c(20, 8)})"],
                [f"a{i}" for i in range(4)], "a[i] < 10, a.sum() == 20", "array.array_sum"))
    P.append(_P("mul_small", [_bv("a", 8), _bv("b", 8), _bv("p", 8)],
                [f"(bvuge a {_c(1, 8)})", f"(bvule a {_c(15, 8)})",
                 f"(bvuge b {_c(1, 8)})", f"(bvule b {_c(15, 8)})",
                 "(= p (bvmul a b))", "(= (bvudiv p b) a)"],
                ["a", "b", "p"], "p == a*b, p/b == a (mul_div_64 shape)",
                "arith.mul_div_64"))
    P.append(_P("mod_eq", [_bv("x", 5), _bv("y", 5)],
                [f"(bvugt y {_c(0, 5)})", f"(= (bvurem x y) {_c(2, 5)})"],
                ["x", "y"], "x % y == 2: LRM-uniform puts little mass on large y",
                "arith.alignment"))
    P.append(_P("mod_align", [_bv("a", 8), _bv("m", 8)],
                [f"(or (= m {_c(3, 8)}) (= m {_c(5, 8)}) (= m {_c(7, 8)}) (= m {_c(12, 8)}))",
                 f"(= (bvurem a m) {_c(0, 8)})",
                 f"(= (bvurem a {_c(16, 8)}) (bvmul {_c(4, 8)} (bvurem m {_c(4, 8)})))",
                 f"(bvult a {_c(200, 8)})"],
                ["a", "m"], "mod_align at 8 bits", "arith.mod_align"))

    # --- unique / alldiff -------------------------------------------------
    P.append(_P("unique_4x3", [_bv(n, 3) for n in "abcd"],
                [_distinct(list("abcd"))], list("abcd"), "unique {a,b,c,d}, 3-bit",
                "array.unique_32"))
    P.append(_P("unique_lt6", [_bv(n, 4) for n in "abcd"],
                [_distinct(list("abcd"))] + [f"(bvult {n} {_c(6, 4)})" for n in "abcd"],
                list("abcd"), "unique, each < 6 (unique_32 shape: a[i] < 64)",
                "array.unique_32"))

    # --- control ----------------------------------------------------------
    P.append(_P("implication", [_bv("mode", 2), _bv("x", 4)],
                [f"(=> (= mode {_c(0, 2)}) (bvult x {_c(2, 4)}))",
                 f"(=> (distinct mode {_c(0, 2)}) (bvuge x {_c(8, 4)}))"],
                ["mode", "x"], "mode==0 -> x<2 else x>=8: mode is NOT uniform under LRM",
                "control.implication_chain_8"))
    P.append(_P("nqueens_5", [_bv(f"q{i}", 3) for i in range(5)],
                [f"(bvult q{i} {_c(5, 3)})" for i in range(5)] +
                [_distinct([f"q{i}" for i in range(5)])] +
                [f"(distinct (bvsub {_zx(f'q{i}', 1)} {_zx(f'q{j}', 1)}) {_c(j - i, 4)})"
                 for i in range(5) for j in range(i + 1, 5)] +
                [f"(distinct (bvsub {_zx(f'q{j}', 1)} {_zx(f'q{i}', 1)}) {_c(j - i, 4)})"
                 for i in range(5) for j in range(i + 1, 5)],
                [f"q{i}" for i in range(5)], "5-queens (10 solutions)", "hard.nqueens_8"))
    return P
