"""Generator for large / wide-address memory BMC fixtures (QF_ABV).

These are the workload where the DV_ARRAY word-level lazy engine is the *only*
option: a memory with a 12-24 bit address space (65K-16M entries) that the dense
mux-forest cannot materialize (it returns ``unknown``). They are deliberately
**non-extensional** -- a single memory value threaded through a store chain, with
NO array equality -- which is exactly the theory boundary where lazy
read-over-write with hit-skip is complete (see docs/large_memory_array_design.md).

Shape (``unsat`` variant), depth ``d`` over address width ``aw``:

    mem1 = store(mem,  a0, v)          ; write v at a0
    memk = store(mem_{k-1}, a_k, w_k)  ; d further writes, each a_k != a0
    assert  select(mem_{d+1}, a0) != v ; must still read v  => UNSAT

The read at ``a0`` must be pushed the full depth of the chain (every later store
is a *miss* at ``a0``) before hitting the ``a0`` store, so it exercises the
exact read-over-write pushdown that drives the cost. The ``sat`` variant drops
the ``a_k != a0`` guards so a later write may legitimately clobber ``a0``.

Used both to emit committed fixtures (``python gen_large_mem.py``) and as a
shape source for the differential fuzzer.
"""
from __future__ import annotations

import sys
from pathlib import Path


def generate(addr_width: int, depth: int, unsat: bool = True) -> str:
    """Return SMT2 text for a wide-address memory BMC of the given size."""
    dw = 32
    L: list[str] = []
    kind = "UNSAT" if unsat else "SAT"
    L.append(f"; large-memory BMC: {addr_width}-bit address, depth {depth} -- expects {kind}")
    L.append("; Single memory threaded through a store chain, NO array equality")
    L.append("; (non-extensional): the read-back at a0 pushes through the full chain.")
    L.append("(set-logic QF_ABV)")
    L.append(f"(declare-fun mem () (Array (_ BitVec {addr_width}) (_ BitVec {dw})))")
    L.append(f"(declare-fun a0 () (_ BitVec {addr_width}))")
    L.append(f"(declare-fun v  () (_ BitVec {dw}))")
    for k in range(1, depth + 1):
        L.append(f"(declare-fun a{k} () (_ BitVec {addr_width}))")
        L.append(f"(declare-fun w{k} () (_ BitVec {dw}))")
    # Store chain: mem1 = store(mem, a0, v); memk = store(mem_{k-1}, a_k, w_k)
    L.append("(define-fun mem1 () (Array (_ BitVec %d) (_ BitVec %d))"
             " (store mem a0 v))" % (addr_width, dw))
    for k in range(1, depth + 1):
        prev = "mem1" if k == 1 else f"mem{k}"
        L.append(f"(define-fun mem{k + 1} () (Array (_ BitVec {addr_width})"
                 f" (_ BitVec {dw})) (store {prev} a{k} w{k}))")
    final = f"mem{depth + 1}"
    if unsat:
        # Every later write is at a distinct address from a0, so a0 keeps v.
        for k in range(1, depth + 1):
            L.append(f"(assert (not (= a{k} a0)))")
        L.append(f"(assert (not (= (select {final} a0) v)))")
    else:
        # No guards: a later write may clobber a0 -> read-back can differ from v.
        L.append(f"(assert (not (= (select {final} a0) v)))")
    L.append("(check-sat)")
    return "\n".join(L) + "\n"


# Grid committed under tests/formal/smt2/tier4/.
_ADDR = (12, 16, 20, 24)
_DEPTH = (16, 32, 64, 128)


def _emit_fixtures(out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for aw in _ADDR:
        for d in _DEPTH:
            (out_dir / f"mem_a{aw}_d{d}_unsat.smt2").write_text(
                generate(aw, d, unsat=True))
            n += 1
    # A couple of SAT witnesses for coverage (small, cheap).
    for aw in (16, 20):
        for d in (16, 32):
            (out_dir / f"mem_a{aw}_d{d}_sat.smt2").write_text(
                generate(aw, d, unsat=False))
            n += 1
    return n


if __name__ == "__main__":
    dest = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).parent / "smt2" / "tier4")
    count = _emit_fixtures(dest)
    print(f"wrote {count} fixtures to {dest}")
