#!/usr/bin/env python3
"""Generate wide (>64-bit) bit-vector probe fixtures (Phases W1 + W2).

Phase W1 added >64-bit bit-vector *variables* + bit-level / arithmetic ops,
routed to the bitblast engine, with wide-limb model readback. Phase W2 adds
wide *constants*: ``#x…``/``#b…`` literals wider than 64 bits and
``(_ bvN W)`` with N >= 2^64 keep their full value (the ``w2_*`` fixtures put
one in every position: eq, arithmetic, compare, extract, concat, array index
and array data).

Every fixture must be answered and must agree with z3.
``w128_hexmask_eq_wideunk`` keeps its W1-era name (the sat-core perf suite
records it under that name) although W2 now answers it.

Regenerate:  python3 tests/formal/generate_wide_smt2.py
"""
from __future__ import annotations

import pathlib

OUT = pathlib.Path(__file__).resolve().parent / "smt2" / "wide"
HDR = "(set-logic QF_BV)\n"


def decl(name: str, w: int) -> str:
    return f"(declare-const {name} (_ BitVec {w}))\n"


FIXTURES: dict[str, str] = {}

# --- 128-bit equality with a small-valued (_ bvN 128) constant ---
FIXTURES["w128_eq_small_sat"] = (
    HDR + decl("x", 128) + "(assert (= x (_ bv42 128)))\n(check-sat)\n"
)
FIXTURES["w128_eq_conflict_unsat"] = (
    HDR + decl("x", 128)
    + "(assert (= x (_ bv42 128)))(assert (= x (_ bv43 128)))\n(check-sat)\n"
)

# --- extract low 64 bits of a 128-bit var ---
FIXTURES["w128_low_slice_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= ((_ extract 63 0) x) (_ bv255 64)))\n(check-sat)\n"
)
FIXTURES["w128_low_slice_unsat"] = (
    HDR + decl("x", 128)
    + "(assert (= ((_ extract 63 0) x) (_ bv0 64)))"
    + "(assert (= ((_ extract 63 0) x) (_ bv1 64)))\n(check-sat)\n"
)

# --- bitwise AND mask on a 128-bit var ---
FIXTURES["w128_and_mask_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= (bvand x (_ bv15 128)) (_ bv10 128)))\n(check-sat)\n"
)
FIXTURES["w128_and_mask_unsat"] = (  # bit4 can't survive an &15
    HDR + decl("x", 128)
    + "(assert (= (bvand x (_ bv15 128)) (_ bv16 128)))\n(check-sat)\n"
)

# --- concat forming a 128-bit value ---
FIXTURES["w128_concat_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= x (concat (_ bv1 64) (_ bv2 64))))\n(check-sat)\n"
)

# --- 65-bit add that wraps: x = 2^65 - 1 (model value itself exceeds 64 bits) ---
FIXTURES["w65_add_wrap_sat"] = (
    HDR + decl("x", 65)
    + "(assert (= (bvadd x (_ bv1 65)) (_ bv0 65)))\n(check-sat)\n"
)

# --- unsigned comparisons bounding a 128-bit var ---
FIXTURES["w128_range_sat"] = (
    HDR + decl("x", 128)
    + "(assert (bvult x (_ bv100 128)))(assert (bvugt x (_ bv50 128)))\n(check-sat)\n"
)
FIXTURES["w128_range_unsat"] = (
    HDR + decl("x", 128)
    + "(assert (bvult x (_ bv50 128)))(assert (bvugt x (_ bv100 128)))\n(check-sat)\n"
)

# --- top-bit of a 128-bit var ---
FIXTURES["w128_msb_set_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= ((_ extract 127 127) x) #b1))\n(check-sat)\n"
)

# --- a 64-bit #x literal compared against a high 64-bit slice (fits -> coverage) ---
FIXTURES["w128_hex_hibits_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= ((_ extract 127 64) x) #xffffffffffffffff))\n(check-sat)\n"
)

# --- a >64-bit-VALUE #x literal: W1 answered unknown (lexer-truncated); W2
# answers it. Name kept for the sat-core perf suite record. ---
FIXTURES["w128_hexmask_eq_wideunk"] = (
    HDR + decl("x", 128)
    + "(assert (= x #xffffffffffffffffffffffffffffffff))\n(check-sat)\n"
)

# --- W2: wide literals in every position ---
_K1 = "#x8000000000000001ffffffffffffffff"     # high limb nonzero (concat)
_K0 = "#x0000000000000000fffffffffffffffe"     # high limb zero (zero_extend)
# eq: high limb pins the high slice; contradicting slice -> unsat
FIXTURES["w2_eq_hi_sat"] = (
    HDR + decl("x", 128) + f"(assert (= x {_K1}))"
    + "(assert (= ((_ extract 127 64) x) #x8000000000000001))\n(check-sat)\n"
)
FIXTURES["w2_eq_hi_unsat"] = (
    HDR + decl("x", 128) + f"(assert (= x {_K1}))"
    + "(assert (= ((_ extract 127 64) x) #x8000000000000000))\n(check-sat)\n"
)
# zero-high-limb literal must not alias a value with the high limb set
FIXTURES["w2_eq_lo_only_unsat"] = (
    HDR + decl("x", 128) + f"(assert (= x {_K0}))"
    + "(assert (distinct ((_ extract 127 64) x) #x0000000000000000))\n(check-sat)\n"
)
# arithmetic: x + 1 carries into the high limb
FIXTURES["w2_add_carry_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= (bvadd x #x00000000000000000000000000000001)"
    + " #x00000000000000010000000000000000))"
    + "(assert (= ((_ extract 63 0) x) #xffffffffffffffff))\n(check-sat)\n"
)
FIXTURES["w2_add_carry_unsat"] = (
    HDR + decl("x", 128)
    + "(assert (= (bvadd x #x00000000000000000000000000000001)"
    + " #x00000000000000010000000000000000))"
    + "(assert (= ((_ extract 127 64) x) #x0000000000000001))\n(check-sat)\n"
)
# bvmul / bvand / bvor against wide constants on an odd (100-bit) width
FIXTURES["w2_mul_odd_width_sat"] = (
    HDR + decl("x", 100)
    + "(assert (= (bvmul x #x0000000000000000000000003) #x00000000000000000000000ff))"
    + "\n(check-sat)\n"
)
FIXTURES["w2_and_or_mask_unsat"] = (
    HDR + decl("x", 128)
    + "(assert (= (bvand x #xffff0000000000000000000000000000) #x00010000000000000000000000000000))"
    + "(assert (= (bvor x #x00000000000000000000000000000000) #x00000000000000000000000000000000))"
    + "\n(check-sat)\n"
)
# unsigned compares bracketing 2^127 with wide bounds
FIXTURES["w2_range_sat"] = (
    HDR + decl("x", 128)
    + "(assert (bvult #x7fffffffffffffffffffffffffffffff x))"
    + "(assert (bvult x #x80000000000000000000000000000002))"
    + "(assert (distinct x #x80000000000000000000000000000000))\n(check-sat)\n"
)
FIXTURES["w2_range_unsat"] = (
    HDR + decl("x", 128)
    + "(assert (bvult #x7fffffffffffffffffffffffffffffff x))"
    + "(assert (bvult x #x80000000000000000000000000000001))"
    + "(assert (distinct x #x80000000000000000000000000000000))\n(check-sat)\n"
)
# extract out of a wide literal (both limbs) and of a 72-bit #b literal
FIXTURES["w2_extract_lit_sat"] = (
    HDR + decl("y", 8)
    + "(assert (= y ((_ extract 71 64) #x0000000000000012ffffffffffffffff)))"
    + "(assert (= y #x12))\n(check-sat)\n"
)
FIXTURES["w2_extract_lit_unsat"] = (
    HDR + decl("y", 8)
    + "(assert (= y ((_ extract 71 64) #x0000000000000012ffffffffffffffff)))"
    + "(assert (= y #xff))\n(check-sat)\n"
)
FIXTURES["w2_bin72_unsat"] = (
    HDR + decl("x", 72)
    + "(assert (bvugt x #b" + "1" * 72 + "))\n(check-sat)\n"
)
# (_ bvN W) with N >= 2^64, and N >= 2^W (reduced mod 2^W, as z3 does)
FIXTURES["w2_bvN_big_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= x (_ bv340282366920938463463374607431768211455 128)))"
    + "(assert (= ((_ extract 0 0) x) #b1))\n(check-sat)\n"
)
FIXTURES["w2_bvN_eq_hex_unsat"] = (
    HDR + decl("x", 128)
    + "(assert (= x (_ bv18446744073709551616 128)))"
    + "(assert (distinct x #x00000000000000010000000000000000))\n(check-sat)\n"
)
FIXTURES["w2_bvN_wrap_narrow_sat"] = (
    HDR + decl("x", 8)
    + "(assert (= x (_ bv18446744073709551621 8)))(assert (= x #x05))\n(check-sat)\n"
)
FIXTURES["w2_bvN_wrap_8_sat"] = (
    HDR + decl("x", 8) + "(assert (= x (_ bv300 8)))(assert (= x #x2c))\n(check-sat)\n"
)
# shift amount solved against a wide literal
FIXTURES["w2_shl_sat"] = (
    HDR + decl("x", 128)
    + "(assert (= (bvshl #x00000000000000000000000000000001 x)"
    + " #x80000000000000000000000000000000))\n(check-sat)\n"
)
# array data and store value wide literals (narrow index)
_ABV = "(set-logic QF_ABV)\n"
FIXTURES["w2_array_data_sat"] = (
    _ABV + "(declare-const a (Array (_ BitVec 8) (_ BitVec 128)))\n" + decl("i", 8)
    + f"(assert (= (select (store a i {_K1}) #x05) {_K1}))"
    + f"(assert (distinct (select a #x05) {_K1}))\n(check-sat)\n"
)
FIXTURES["w2_array_data_unsat"] = (
    _ABV + "(declare-const a (Array (_ BitVec 8) (_ BitVec 128)))\n" + decl("i", 8)
    + f"(assert (= (select (store a i {_K1}) #x05) {_K1}))"
    + f"(assert (distinct (select a #x05) {_K1}))"
    + "(assert (distinct i #x05))\n(check-sat)\n"
)

# wide literal as an array INDEX: a 128-bit address array is large-address
# (abstract), which is correctness-gap Step 3 -- until then dv-solve may answer
# unknown (`*_arrunk`), but the literal must never alias its truncated low limb.
_A128 = "(declare-const a (Array (_ BitVec 128) (_ BitVec 8)))\n"
FIXTURES["w2_array_index_alias_sat_arrunk"] = (
    _ABV + _A128
    + "(assert (= (select a #x00000000000000010000000000000005) #x01))"
    + "(assert (= (select a #x00000000000000000000000000000005) #x02))\n(check-sat)\n"
)
FIXTURES["w2_array_index_bvN_unsat_arrunk"] = (
    _ABV + _A128
    + "(assert (= (select a #x00000000000000010000000000000005) #x01))"
    + "(assert (= (select a (_ bv18446744073709551621 128)) #x02))\n(check-sat)\n"
)
FIXTURES["w2_array_store_index_unsat_arrunk"] = (
    _ABV + _A128
    + "(assert (= (select (store a #x00000000000000010000000000000005 #x07)"
    + " #x00000000000000010000000000000005) #x02))\n(check-sat)\n"
)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, body in FIXTURES.items():
        (OUT / f"{name}.smt2").write_text(body)
    print(f"wrote {len(FIXTURES)} fixtures to {OUT}")


if __name__ == "__main__":
    main()
