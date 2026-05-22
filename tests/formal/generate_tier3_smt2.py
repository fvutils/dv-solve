#!/usr/bin/env python3
"""Generate Tier 3 SMT2 benchmarks exercising QF_AUFBV array theory.

Each benchmark is written as a direct SMT-LIB2 assertion problem (no
external model generator required).  The "depth" parameter controls how
many state transitions are unrolled, mirroring the Tier 2 convention.

No ``let`` bindings are used (dv-solve-smt2 does not support them);
all expressions are fully inlined.

Usage:
    python tests/formal/generate_tier3_smt2.py

Output: tests/formal/smt2/tier3/<name>_bmc_d<D>.smt2
"""
from __future__ import annotations

from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent / "smt2" / "tier3"
OUT_DIR.mkdir(parents=True, exist_ok=True)

BMC_DEPTHS = [1, 2, 4, 8]


def _make_or_assert(clauses: list[str]) -> str:
    """Return an (assert ...) line. If single clause, skip (or ...) wrapper."""
    if len(clauses) == 1:
        return f"(assert {clauses[0]})"
    inner = "\n".join(f"  {c}" for c in clauses)
    return f"(assert (or\n{inner}\n))"


def _write(name: str, content: str) -> None:
    p = OUT_DIR / f"{name}.smt2"
    p.write_text(content)
    print(f"  {p.name}")


# ---------------------------------------------------------------------------
# 1. regfile_simple  (8×8 register file, state-machine style)
#    Property: write-then-read at same address returns written value.
#    Exercises: declare-fun returning (Array BV BV), store, select, array =.
#    Expected: UNSAT at all depths.
# ---------------------------------------------------------------------------

def _regfile_simple_bmc(depth: int) -> str:
    state_decls = "\n".join(
        f"(declare-const |state_{i}| |RegfileSimple_s|)" for i in range(depth + 1)
    )
    trans_asserts = "\n".join(
        f"(assert (|RegfileSimple_t| |state_{i}| |state_{i+1}|))"
        for i in range(depth)
    )
    d = depth
    return f"""; BMC d={depth} for RegfileSimple -- expects UNSAT
; Property: write-then-read at the same address returns the written value.
; Exercises: declare-fun returning (Array BV BV), store, select, array equality.
(set-logic QF_AUFBV)

(declare-sort |RegfileSimple_s| 0)

; 8-entry x 8-bit register file as an array field of the state
(declare-fun |RegfileSimple#mem|     (|RegfileSimple_s|) (Array (_ BitVec 3) (_ BitVec 8)))
(declare-fun |RegfileSimple#wr_en|   (|RegfileSimple_s|) Bool)
(declare-fun |RegfileSimple#wr_addr| (|RegfileSimple_s|) (_ BitVec 3))
(declare-fun |RegfileSimple#wr_data| (|RegfileSimple_s|) (_ BitVec 8))
(declare-fun |RegfileSimple#rd_addr| (|RegfileSimple_s|) (_ BitVec 3))
(declare-fun |RegfileSimple#rd_data| (|RegfileSimple_s|) (_ BitVec 8))

; Transition: if wr_en, update mem at wr_addr; rd_data reflects updated mem
(define-fun |RegfileSimple_t| ((s |RegfileSimple_s|) (n |RegfileSimple_s|)) Bool
  (and
    (= (|RegfileSimple#mem| n)
       (ite (|RegfileSimple#wr_en| s)
            (store (|RegfileSimple#mem| s)
                   (|RegfileSimple#wr_addr| s)
                   (|RegfileSimple#wr_data| s))
            (|RegfileSimple#mem| s)))
    (= (|RegfileSimple#rd_data| n)
       (select (|RegfileSimple#mem| n) (|RegfileSimple#rd_addr| s)))))

{state_decls}

{trans_asserts}

; Negate property at the last transition:
; wr_en at state_{d-1}, rd_addr_{d-1} == wr_addr_{d-1}, but rd_data_{d} != wr_data_{d-1}
(assert (|RegfileSimple#wr_en|   |state_{d-1}|))
(assert (= (|RegfileSimple#rd_addr| |state_{d-1}|) (|RegfileSimple#wr_addr| |state_{d-1}|)))
(assert (not (= (|RegfileSimple#rd_data| |state_{d}|)
               (|RegfileSimple#wr_data| |state_{d-1}|))))

(check-sat)
"""


# ---------------------------------------------------------------------------
# 2. regfile_addr_alias  (8×8 register file, address non-aliasing)
#    Property: writing address A then reading address B (A != B) returns old B.
#    Exercises: symbolic store/select with different indices.
#    Expected: UNSAT at all depths.
# ---------------------------------------------------------------------------

def _regfile_addr_alias_bmc(depth: int) -> str:
    state_decls = "\n".join(
        f"(declare-const |state_{i}| |RegfileAlias_s|)" for i in range(depth + 1)
    )
    trans_asserts = "\n".join(
        f"(assert (|RegfileAlias_t| |state_{i}| |state_{i+1}|))"
        for i in range(depth)
    )
    d = depth
    return f"""; BMC d={depth} for RegfileAddrAlias -- expects UNSAT
; Property: writing address A then reading address B (A != B) returns old B value.
; Exercises: store/select non-aliasing proof.
(set-logic QF_AUFBV)

(declare-sort |RegfileAlias_s| 0)

(declare-fun |RegfileAlias#mem|     (|RegfileAlias_s|) (Array (_ BitVec 3) (_ BitVec 8)))
(declare-fun |RegfileAlias#wr_en|   (|RegfileAlias_s|) Bool)
(declare-fun |RegfileAlias#wr_addr| (|RegfileAlias_s|) (_ BitVec 3))
(declare-fun |RegfileAlias#wr_data| (|RegfileAlias_s|) (_ BitVec 8))
(declare-fun |RegfileAlias#rd_addr| (|RegfileAlias_s|) (_ BitVec 3))
(declare-fun |RegfileAlias#rd_data| (|RegfileAlias_s|) (_ BitVec 8))

(define-fun |RegfileAlias_t| ((s |RegfileAlias_s|) (n |RegfileAlias_s|)) Bool
  (and
    (= (|RegfileAlias#mem| n)
       (ite (|RegfileAlias#wr_en| s)
            (store (|RegfileAlias#mem| s)
                   (|RegfileAlias#wr_addr| s)
                   (|RegfileAlias#wr_data| s))
            (|RegfileAlias#mem| s)))
    (= (|RegfileAlias#rd_data| n)
       (select (|RegfileAlias#mem| n) (|RegfileAlias#rd_addr| s)))))

{state_decls}

{trans_asserts}

; Negate property at last transition:
; wr_en, wr_addr != rd_addr, but rd_data != old mem[rd_addr]
(assert (|RegfileAlias#wr_en|   |state_{d-1}|))
(assert (not (= (|RegfileAlias#rd_addr| |state_{d-1}|)
               (|RegfileAlias#wr_addr| |state_{d-1}|))))
; rd_data in next state should equal old mem[rd_addr]; negate:
(assert (not (= (|RegfileAlias#rd_data| |state_{d}|)
               (select (|RegfileAlias#mem| |state_{d-1}|)
                       (|RegfileAlias#rd_addr| |state_{d-1}|)))))

(check-sat)
"""


# ---------------------------------------------------------------------------
# 3. fifo_8x16  (8-deep x 16-bit FIFO, memory-backed)
#    Property: count is always in [0, 8].
#    Exercises: store/select with 3-bit address, count arithmetic.
#    Note: no let bindings; full/empty/do_push/do_pop are inlined.
#    Expected: UNSAT at all depths.
# ---------------------------------------------------------------------------

def _fifo_8x16_bmc(depth: int) -> str:
    state_decls = "\n".join(
        f"(declare-const |state_{i}| |Fifo_s|)" for i in range(depth + 1)
    )
    trans_asserts = "\n".join(
        f"(assert (|Fifo_t| |state_{i}| |state_{i+1}|))"
        for i in range(depth)
    )
    d = depth
    # Inlined: full = (count == 8), empty = (count == 0)
    # do_push = push_en AND (NOT full), do_pop = pop_en AND (NOT empty)
    return f"""; BMC d={depth} for Fifo8x16 -- expects UNSAT
; Property: FIFO count is always in [0, 8] (4-bit count, max value 8).
; Exercises: Array store/select with 3-bit pointer, 4-bit count arithmetic.
(set-logic QF_AUFBV)

(declare-sort |Fifo_s| 0)

; 8-entry x 16-bit data storage (3-bit address)
(declare-fun |Fifo#data|    (|Fifo_s|) (Array (_ BitVec 3) (_ BitVec 16)))
; 3-bit write and read pointers (mod 8)
(declare-fun |Fifo#wr_ptr|  (|Fifo_s|) (_ BitVec 3))
(declare-fun |Fifo#rd_ptr|  (|Fifo_s|) (_ BitVec 3))
; 4-bit count (range 0..8)
(declare-fun |Fifo#count|   (|Fifo_s|) (_ BitVec 4))
; Push / pop enables from the environment
(declare-fun |Fifo#push_en| (|Fifo_s|) Bool)
(declare-fun |Fifo#pop_en|  (|Fifo_s|) Bool)
(declare-fun |Fifo#din|     (|Fifo_s|) (_ BitVec 16))

; Transition (guarded push/pop; no let bindings -- all inlined)
; do_push = push_en AND NOT full   where full  = (count == 8)
; do_pop  = pop_en  AND NOT empty  where empty = (count == 0)
(define-fun |Fifo_t| ((s |Fifo_s|) (n |Fifo_s|)) Bool
  (and
    ; data array updated on push
    (= (|Fifo#data| n)
       (ite (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))
            (store (|Fifo#data| s)
                   (|Fifo#wr_ptr| s)
                   (|Fifo#din| s))
            (|Fifo#data| s)))
    ; wr_ptr advances on push
    (= (|Fifo#wr_ptr| n)
       (ite (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))
            (bvadd (|Fifo#wr_ptr| s) #b001)
            (|Fifo#wr_ptr| s)))
    ; rd_ptr advances on pop
    (= (|Fifo#rd_ptr| n)
       (ite (and (|Fifo#pop_en| s) (not (= (|Fifo#count| s) #b0000)))
            (bvadd (|Fifo#rd_ptr| s) #b001)
            (|Fifo#rd_ptr| s)))
    ; count update: push-only +1, pop-only -1, both/neither stay
    (= (|Fifo#count| n)
       (ite (and (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))
                 (not (and (|Fifo#pop_en| s) (not (= (|Fifo#count| s) #b0000)))))
            (bvadd (|Fifo#count| s) #b0001)
            (ite (and (and (|Fifo#pop_en| s) (not (= (|Fifo#count| s) #b0000)))
                      (not (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))))
                 (bvsub (|Fifo#count| s) #b0001)
                 (|Fifo#count| s))))))

{state_decls}

; Initial state: count = 0, pointers = 0
(assert (= (|Fifo#count|  |state_0|) #b0000))
(assert (= (|Fifo#wr_ptr| |state_0|) #b000))
(assert (= (|Fifo#rd_ptr| |state_0|) #b000))

{trans_asserts}

; Property (negated): count > 8 at any reachable state
{_make_or_assert(
    [f'(bvugt (|Fifo#count| |state_{i}|) #b1000)' for i in range(1, d+1)]
)}

(check-sat)
"""


# ---------------------------------------------------------------------------
# 4. cache_direct_1way  (16-line direct-mapped cache)
#    Property: after writing a cache line, data array contains written value.
#    Exercises: 4-bit address arrays for data (8-bit), valid (1-bit), tag (4-bit).
#    No let bindings; index/tag extracts are inlined.
#    Expected: UNSAT at all depths.
# ---------------------------------------------------------------------------

def _cache_direct_1way_bmc(depth: int) -> str:
    state_decls = "\n".join(
        f"(declare-const |state_{i}| |Cache_s|)" for i in range(depth + 1)
    )
    trans_asserts = "\n".join(
        f"(assert (|Cache_t| |state_{i}| |state_{i+1}|))"
        for i in range(depth)
    )
    d = depth
    # idx = extract(3,0)(addr), tag = extract(7,4)(addr)
    # Transition: on write request, update data/valid/tag arrays at idx
    return f"""; BMC d={depth} for CacheDirectOneWay -- expects UNSAT
; Property: after writing a cache line, the data array contains the written value.
; Exercises: 4-bit index arrays for data (8-bit), valid (1-bit), tag (4-bit).
(set-logic QF_AUFBV)

(declare-sort |Cache_s| 0)

; 16-line cache: 4-bit index -> data/valid/tag
(declare-fun |Cache#data|  (|Cache_s|) (Array (_ BitVec 4) (_ BitVec 8)))
(declare-fun |Cache#valid| (|Cache_s|) (Array (_ BitVec 4) (_ BitVec 1)))
(declare-fun |Cache#tag|   (|Cache_s|) (Array (_ BitVec 4) (_ BitVec 4)))

; Processor request signals
(declare-fun |Cache#req_en|    (|Cache_s|) Bool)
(declare-fun |Cache#req_we|    (|Cache_s|) Bool)
(declare-fun |Cache#req_addr|  (|Cache_s|) (_ BitVec 8))   ; 4 tag + 4 index
(declare-fun |Cache#req_data|  (|Cache_s|) (_ BitVec 8))

; Transition: on write, fill the addressed line
; idx = extract(3,0)(req_addr), tag = extract(7,4)(req_addr)
(define-fun |Cache_t| ((s |Cache_s|) (n |Cache_s|)) Bool
  (and
    (= (|Cache#data| n)
       (ite (and (|Cache#req_en| s) (|Cache#req_we| s))
            (store (|Cache#data| s)
                   ((_ extract 3 0) (|Cache#req_addr| s))
                   (|Cache#req_data| s))
            (|Cache#data| s)))
    (= (|Cache#valid| n)
       (ite (and (|Cache#req_en| s) (|Cache#req_we| s))
            (store (|Cache#valid| s)
                   ((_ extract 3 0) (|Cache#req_addr| s))
                   #b1)
            (|Cache#valid| s)))
    (= (|Cache#tag| n)
       (ite (and (|Cache#req_en| s) (|Cache#req_we| s))
            (store (|Cache#tag| s)
                   ((_ extract 3 0) (|Cache#req_addr| s))
                   ((_ extract 7 4) (|Cache#req_addr| s)))
            (|Cache#tag| s)))))

{state_decls}

; Initially all lines invalid
(assert (= (|Cache#valid| |state_0|) ((as const (Array (_ BitVec 4) (_ BitVec 1))) #b0)))

{trans_asserts}

; Property (negated): write at state_{d-1}, but data array at state_{d} has wrong value
(assert (|Cache#req_en|  |state_{d-1}|))
(assert (|Cache#req_we|  |state_{d-1}|))
(assert (not (= (select (|Cache#data| |state_{d}|)
                        ((_ extract 3 0) (|Cache#req_addr| |state_{d-1}|)))
               (|Cache#req_data| |state_{d-1}|))))

(check-sat)
"""


# ---------------------------------------------------------------------------
# 5. wide_datapath_64  (64-bit ALU, no arrays)
#    Property: identity laws hold for 64-bit BV operations.
#    Exercises: wide BV arithmetic under the array-aware translator.
#    dv-solve supports BV up to 64 bits; 64 is the widest usable width.
#    Expected: UNSAT at all depths (the property is a pure BV tautology).
# ---------------------------------------------------------------------------

def _wide_datapath_128_bmc(depth: int) -> str:
    state_decls = "\n".join(
        f"(declare-const |state_{i}| |Wide_s|)" for i in range(depth + 1)
    )
    trans_asserts = "\n".join(
        f"(assert (|Wide_t| |state_{i}| |state_{i+1}|))"
        for i in range(depth)
    )
    d = depth
    zero64 = "#x" + "0" * 16  # 64-bit zero in hex
    return f"""; BMC d={depth} for WideDatapath64 -- expects UNSAT
; Property: 64-bit ALU identity: (a + 0) = a for all a.
; No arrays -- ensures the array-aware translator handles wide BV (max 64 bits) correctly.
(set-logic QF_AUFBV)

(declare-sort |Wide_s| 0)
(declare-fun |Wide#a|      (|Wide_s|) (_ BitVec 64))
(declare-fun |Wide#result| (|Wide_s|) (_ BitVec 64))

(define-fun |Wide_t| ((s |Wide_s|) (n |Wide_s|)) Bool
  (= (|Wide#result| n)
     (bvadd (|Wide#a| s) {zero64})))

{state_decls}

{trans_asserts}

; Negate: result at last step != a at previous step
(assert (not (= (|Wide#result| |state_{d}|) (|Wide#a| |state_{d-1}|))))

(check-sat)
"""


# ---------------------------------------------------------------------------
# 6. dma_engine_small  (4-channel scratch memory, 2-bit channel, 8-bit data)
#    Property: write then read same channel returns written value.
#    d=1: UNSAT (write at step0 → read at step1 same channel → get wr_data)
#    d=2: SAT   (two writes to same channel; assert rd_data equals first write,
#                but model returns second write → satisfiable contradiction)
#    d=4, d=8: UNSAT (using the d=1 property structure at the last step)
# ---------------------------------------------------------------------------

def _dma_engine_small_bmc(depth: int, expected_sat: bool) -> str:
    tag = "SAT" if expected_sat else "UNSAT"
    state_decls = "\n".join(
        f"(declare-const |state_{i}| |DMA_s|)" for i in range(depth + 1)
    )
    trans_asserts = "\n".join(
        f"(assert (|DMA_t| |state_{i}| |state_{i+1}|))"
        for i in range(depth)
    )
    d = depth

    if not expected_sat:
        # UNSAT: write to ch at step d-1, read same ch at step d, rd_data != wr_data
        negated_prop = f"""; Negate: write to ch0 and read same ch0 but rd_data != wr_data
(assert (|DMA#wr_en|  |state_{d-1}|))
(assert (= (|DMA#wr_ch|  |state_{d-1}|) #b00))
(assert (= (|DMA#rd_ch|  |state_{d-1}|) #b00))
(assert (not (= (|DMA#rd_data| |state_{d}|) (|DMA#wr_data| |state_{d-1}|))))"""
    else:
        # SAT at d=2: "ch0 cannot change without a write."
        # Negate: no write at step_0, but ch0 value at step_2 differs from ch0 at step_0.
        # A write at step_1 to ch0 provides the counterexample.
        negated_prop = f"""; d=2 SAT: "ch0 content never changes without a write at step 0."
; Negate: no write at step_0, but select(mem_2, ch0) != select(mem_0, ch0).
; Satisfiable: choose wr_en_1=true, wr_ch_1=0, wr_data_1 != initial mem_0[0].
(assert (not (|DMA#wr_en| |state_0|)))
(assert (not (= (select (|DMA#mem| |state_{d}|) #b00)
               (select (|DMA#mem| |state_0|) #b00))))"""

    return f"""; BMC d={depth} for DmaEngineSmall -- expects {tag}
; 4-channel scratch memory: Bool wr_en, 2-bit channel, 8-bit data.
; d=1: UNSAT (write-then-read same channel returns written value).
; d=2: SAT   (two sequential writes; assert stale value -- satisfiable).
(set-logic QF_AUFBV)

(declare-sort |DMA_s| 0)

; 4-channel scratch memory (2-bit channel select as array address)
(declare-fun |DMA#mem|     (|DMA_s|) (Array (_ BitVec 2) (_ BitVec 8)))
(declare-fun |DMA#wr_en|   (|DMA_s|) Bool)
(declare-fun |DMA#wr_ch|   (|DMA_s|) (_ BitVec 2))
(declare-fun |DMA#wr_data| (|DMA_s|) (_ BitVec 8))
(declare-fun |DMA#rd_ch|   (|DMA_s|) (_ BitVec 2))
(declare-fun |DMA#rd_data| (|DMA_s|) (_ BitVec 8))

(define-fun |DMA_t| ((s |DMA_s|) (n |DMA_s|)) Bool
  (and
    (= (|DMA#mem| n)
       (ite (|DMA#wr_en| s)
            (store (|DMA#mem| s) (|DMA#wr_ch| s) (|DMA#wr_data| s))
            (|DMA#mem| s)))
    (= (|DMA#rd_data| n)
       (select (|DMA#mem| n) (|DMA#rd_ch| s)))))

{state_decls}

{trans_asserts}

{negated_prop}

(check-sat)
"""


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> None:
    print(f"Generating Tier 3 SMT2 benchmarks in {OUT_DIR}/ ...")

    # 1. regfile_simple
    for d in BMC_DEPTHS:
        _write(f"regfile_simple_bmc_d{d}", _regfile_simple_bmc(d))

    # 2. regfile_addr_alias
    for d in BMC_DEPTHS:
        _write(f"regfile_addr_alias_bmc_d{d}", _regfile_addr_alias_bmc(d))

    # 3. fifo_8x16
    for d in BMC_DEPTHS:
        _write(f"fifo_8x16_bmc_d{d}", _fifo_8x16_bmc(d))

    # 4. cache_direct_1way
    for d in BMC_DEPTHS:
        _write(f"cache_direct_1way_bmc_d{d}", _cache_direct_1way_bmc(d))

    # 5. wide_datapath_128
    for d in BMC_DEPTHS:
        _write(f"wide_datapath_128_bmc_d{d}", _wide_datapath_128_bmc(d))

    # 6. dma_engine_small  (d=1 UNSAT, d=2 SAT, d=4/8 UNSAT for same d=1 property)
    _write("dma_engine_small_bmc_d1", _dma_engine_small_bmc(1, expected_sat=False))
    _write("dma_engine_small_bmc_d2", _dma_engine_small_bmc(2, expected_sat=True))
    for d in [4, 8]:
        _write(f"dma_engine_small_bmc_d{d}", _dma_engine_small_bmc(d, expected_sat=False))

    print("Done.")


if __name__ == "__main__":
    main()
