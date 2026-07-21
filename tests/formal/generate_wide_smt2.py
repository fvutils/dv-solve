#!/usr/bin/env python3
"""Generate wide (>64-bit) bit-vector probe fixtures (Phase W1).

Phase W1 adds >64-bit bit-vector *variables* + bit-level / arithmetic ops,
routed to the bitblast engine, with wide-limb model readback. Wide *constants*
are supported only through the ``(_ bvN W)`` decimal form whose value fits in
64 bits (e.g. masks, small bounds); a ``#x…`` literal wider than 64 bits is
truncated at the lexer, so W1 must answer ``unknown`` on it rather than risk a
wrong answer — those fixtures are named ``*_wideunk`` and checked for soundness
only (dv-solve may punt, but must never disagree with z3).

Filename convention (matches generate_gaps_smt2.py):
  * ``*_wideunk.smt2`` — dv-solve is allowed to answer ``unknown`` (documented
    W1 limitation); the test asserts only that it never *disagrees* with z3.
  * everything else must be answered and must agree with z3.

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

# --- W1 soundness boundary: a >64-bit-VALUE #x literal (lexer-truncated) ---
# z3 says sat; dv-solve must answer unknown (never a wrong sat/unsat).
FIXTURES["w128_hexmask_eq_wideunk"] = (
    HDR + decl("x", 128)
    + "(assert (= x #xffffffffffffffffffffffffffffffff))\n(check-sat)\n"
)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, body in FIXTURES.items():
        (OUT / f"{name}.smt2").write_text(body)
    print(f"wrote {len(FIXTURES)} fixtures to {OUT}")


if __name__ == "__main__":
    main()
