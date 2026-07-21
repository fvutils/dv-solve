#!/usr/bin/env python3
"""Generate the Verilator-checklist gap-probe SMT2 fixtures.

Each fixture exercises one solver-level shape from
``docs/verilator_constraint_feature_checklist.md`` that had no dedicated
dv-solve coverage. They are self-contained QF_BV / QF_ABV problems with a
definite sat/unsat answer, used by ``test_gap_coverage.py`` to differentially
check dv-solve (CDCL + bitblast engines) against z3.

Filename convention:
  * ``*_wideunk.smt2`` — a >64-bit shape dv-solve currently answers ``unknown``
    (documented incompleteness; the test asserts soundness only, i.e. dv-solve
    never *disagrees* with z3, and is allowed to punt).
  * everything else must be answered and must agree with z3.

Regenerate with:  python3 tests/formal/generate_gaps_smt2.py
"""
from __future__ import annotations

import pathlib

OUT = pathlib.Path(__file__).resolve().parent / "smt2" / "gaps"
HDR = "(set-logic QF_BV)\n"
AHDR = "(set-logic QF_ABV)\n"


def popcount(var: str, w: int, outw: int) -> str:
    terms = [f"((_ zero_extend {outw - 1}) ((_ extract {i} {i}) {var}))" for i in range(w)]
    e = terms[0]
    for t in terms[1:]:
        e = f"(bvadd {e} {t})"
    return e


FIXTURES: dict[str, str] = {}

# --- popcount / $countones / $onehot (gap 3) ---
FIXTURES["popcount8_eq3_sat"] = (
    HDR + "(declare-const x (_ BitVec 8))\n"
    f"(assert (= {popcount('x', 8, 4)} (_ bv3 4)))\n(check-sat)\n"
)
FIXTURES["popcount8_eq9_unsat"] = (
    HDR + "(declare-const x (_ BitVec 8))\n"
    f"(assert (= {popcount('x', 8, 4)} (_ bv9 4)))\n(check-sat)\n"
)
FIXTURES["onehot8_sat"] = (
    HDR + "(declare-const x (_ BitVec 8))\n"
    f"(assert (= {popcount('x', 8, 4)} (_ bv1 4)))\n"
    "(assert (not (= x (_ bv0 8))))\n(check-sat)\n"
)
FIXTURES["onehot_and_zero_unsat"] = (
    HDR + "(declare-const x (_ BitVec 8))\n"
    f"(assert (= {popcount('x', 8, 4)} (_ bv1 4)))\n"
    "(assert (= x (_ bv0 8)))\n(check-sat)\n"
)

# --- product / non-sum array reductions (gap 2) ---
FIXTURES["product_ff_ff_eq1_sat"] = (  # 255*255 mod 256 == 1
    HDR + "(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))\n"
    "(assert (= a #xff))(assert (= b #xff))(assert (= (bvmul a b) #x01))\n(check-sat)\n"
)
FIXTURES["product_ff_ff_ne1_unsat"] = (
    HDR + "(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))\n"
    "(assert (= a #xff))(assert (= b #xff))(assert (not (= (bvmul a b) #x01)))\n(check-sat)\n"
)
FIXTURES["product4_chain_sat"] = (
    HDR + "(declare-const a (_ BitVec 4))(declare-const b (_ BitVec 4))"
    "(declare-const c (_ BitVec 4))(declare-const d (_ BitVec 4))\n"
    "(assert (= (bvmul (bvmul a b) (bvmul c d)) (_ bv6 4)))\n"
    "(assert (bvuge a (_ bv1 4)))(assert (bvuge b (_ bv1 4)))\n(check-sat)\n"
)
FIXTURES["xor_reduce_sat"] = (
    HDR + "(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))(declare-const c (_ BitVec 8))\n"
    "(assert (= (bvxor a b c) #xff))(assert (= a #x0f))(assert (= b #xf0))\n(check-sat)\n"
)
FIXTURES["and_reduce_unsat"] = (
    HDR + "(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))\n"
    "(assert (= (bvand a b) #xff))(assert (= a #x0f))\n(check-sat)\n"
)
FIXTURES["or_reduce_sat"] = (
    HDR + "(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))\n"
    "(assert (= (bvor a b) #xff))(assert (= a #x0f))\n(check-sat)\n"
)

# --- sign_extend to 64 (backlog HIGH suspect) ---
FIXTURES["signext32to64_sat"] = (
    HDR + "(declare-const x (_ BitVec 32))(declare-const y (_ BitVec 64))\n"
    "(assert (= y ((_ sign_extend 32) x)))(assert (= ((_ extract 31 31) x) #b1))\n"
    "(assert (= ((_ extract 63 32) y) #xffffffff))\n(check-sat)\n"
)
FIXTURES["signext32to64_unsat"] = (
    HDR + "(declare-const x (_ BitVec 32))(declare-const y (_ BitVec 64))\n"
    "(assert (= y ((_ sign_extend 32) x)))(assert (= ((_ extract 31 31) x) #b1))\n"
    "(assert (= ((_ extract 63 32) y) #x00000000))\n(check-sat)\n"
)
FIXTURES["signext_neg_value_sat"] = (
    HDR + "(declare-const x (_ BitVec 32))(declare-const y (_ BitVec 64))\n"
    "(assert (= x #xffffffff))(assert (= y ((_ sign_extend 32) x)))"
    "(assert (= y #xffffffffffffffff))\n(check-sat)\n"
)

# --- wildcard masked compare ($==?, inside wildcard) (gap 1) ---
FIXTURES["wildcard_mask_sat"] = (
    HDR + "(declare-const x (_ BitVec 8))\n"
    "(assert (= (bvand x #x0f) #x0a))\n(check-sat)\n"
)
FIXTURES["wildcard_mask_unsat"] = (
    HDR + "(declare-const x (_ BitVec 8))\n"
    "(assert (= (bvand x #x0f) #x0a))(assert (= (bvand x #x0f) #x0b))\n(check-sat)\n"
)

# --- symbolic array index (gap 7) ---
FIXTURES["array_symbolic_index_sat"] = (
    AHDR + "(declare-const a (Array (_ BitVec 4) (_ BitVec 8)))(declare-const i (_ BitVec 4))\n"
    "(assert (= (select a i) #x2a))(assert (bvult i (_ bv4 4)))\n(check-sat)\n"
)
FIXTURES["array_symbolic_index_unsat"] = (
    AHDR + "(declare-const a (Array (_ BitVec 4) (_ BitVec 8)))(declare-const i (_ BitVec 4))\n"
    "(assert (= (select a i) #x2a))(assert (= (select a i) #x2b))\n(check-sat)\n"
)

# --- wide >64-bit (gap 6): now COVERED by Phase W1 (small-valued (_ bvN 128)
# constants route to bitblast). See tests/formal/test_wide_bv.py for the full
# W1 corpus; these two stay here as the gap-6 regression anchors. ---
FIXTURES["wide128_add_sat"] = (
    HDR + "(declare-const x (_ BitVec 128))(declare-const y (_ BitVec 128))\n"
    "(assert (= x (_ bv1 128)))(assert (= (bvadd x y) (_ bv0 128)))\n(check-sat)\n"
)
FIXTURES["wide128_mul_unsat"] = (
    HDR + "(declare-const x (_ BitVec 128))\n"
    "(assert (= (bvmul x (_ bv2 128)) (_ bv1 128)))\n(check-sat)\n"
)


# --- CDCL 64-bit unsigned wrap/high-bit hazard (found by the fuzzer 2026-07-10) ---
# CDCL models a var's domain in int64, so a 64-bit ARITHMETIC/ite result in
# [2^63, 2^64) read as negative broke unsigned propagation -> wrong `unsat`.
# Fixed by routing width>=64 arith/unary/ite to bitblast (_flag_wide_arith).
FIXTURES["cdcl64_add_overflow_sat"] = (  # unsigned overflow check: (x+y) < x
    HDR + "(declare-const x (_ BitVec 64))(declare-const y (_ BitVec 64))\n"
    "(assert (bvult (bvadd x y) x))\n(check-sat)\n"
)
FIXTURES["cdcl64_ite_highbit_sat"] = (
    HDR + "(declare-const b (_ BitVec 1))(declare-const v (_ BitVec 64))\n"
    "(assert (bvuge (ite (= b (_ bv0 1)) (bvnot v) v) v))\n(check-sat)\n"
)
FIXTURES["cdcl64_xor_highbit_sat"] = (
    HDR + "(declare-const b (_ BitVec 1))(declare-const v (_ BitVec 64))\n"
    "(assert (bvuge (ite (= b (_ bv0 1)) "
    "(bvxor (_ bv18000000000000000000 64) (_ bv0 64)) v) v))\n(check-sat)\n"
)
# B6: extract of a 64-bit var's high bits — fixed at the CDCL level (stays on
# CDCL, not routed) by relaxing the _bit_slice_backward signed-bound guard for
# unsigned width>=64 sources. bit 63 = int64 sign, so ahi read as negative and
# the backward propagation used to bail -> v never forced to bit 63 -> wrong unsat.
FIXTURES["cdcl64_extract_hibit_sat"] = (
    HDR + "(declare-const v (_ BitVec 64))\n"
    "(assert (= ((_ extract 63 63) v) #b1))\n(check-sat)\n"
)
FIXTURES["cdcl64_extract_hi32_sat"] = (
    HDR + "(declare-const v (_ BitVec 64))\n"
    "(assert (= ((_ extract 63 32) v) #xffffffff))\n(check-sat)\n"
)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, body in FIXTURES.items():
        (OUT / f"{name}.smt2").write_text(body)
    print(f"wrote {len(FIXTURES)} fixtures to {OUT}")


if __name__ == "__main__":
    main()
