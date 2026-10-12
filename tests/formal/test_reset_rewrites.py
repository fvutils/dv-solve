"""Soft reset and the s-expression arena (B120, B121).

B120: after `(reset)` the per-session s-expression arena restarted at its
first block, and when that filled it linked a new block over the rest of the
chain: every block past the first leaked, about 80 KB per Verilator
randomize (800 resets peaked at 17 MB instead of 5 MB).

B121: the cache of operator rewrites (rotate_left, bvnand, ...) is kept
across a soft reset but pointed into that arena, which the reset reuses: a
later declaration overwrote a cached template, and an operator then
translated as garbage (unknown, spurious parse errors). The leak hid it
(leaked blocks are never overwritten); fixing the leak exposed it, so both
are fixed together. Found by the malloc lifecycle survey
(docs/malloc_lifecycle_analysis.md).
"""
import os
import random
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get("DV_SOLVE_SMT2", REPO / "build" / "dv-solve-smt2"))

pytestmark = pytest.mark.skipif(not EXE.exists(), reason="dv-solve-smt2 not built")

M = 0xFF


def _rol(x, k):
    k %= 8
    return ((x << k) | (x >> (8 - k))) & M


OPS = [
    ("((_ rotate_left {k}) x)", lambda x, k: _rol(x, k)),
    ("((_ rotate_right {k}) x)", lambda x, k: _rol(x, 8 - k % 8)),
    ("(bvnand x #x{k:02x})", lambda x, k: ~(x & k) & M),
    ("(bvnor x #x{k:02x})", lambda x, k: ~(x | k) & M),
    ("(bvxnor x #x{k:02x})", lambda x, k: ~(x ^ k) & M),
]


def _session(seed, rounds=30):
    """Rounds of padding declarations then rewritten operators, each round
    satisfiable by construction, separated by (reset)."""
    rng = random.Random(seed)
    out = ["(set-logic QF_BV)"]
    for _ in range(rounds):
        x = rng.randrange(256)
        out.append("(declare-fun x () (_ BitVec 8))")
        for i in range(rng.randrange(0, 200)):
            out.append(f"(declare-fun p{i} () (_ BitVec 8))")
            out.append(f"(assert (bvult p{i} (bvadd p{i} #x01)))")
        for _ in range(3):
            t, f = rng.choice(OPS)
            k = rng.randrange(1, 255)
            out.append(f"(assert (= {t.format(k=k)} #x{f(x, k):02x}))")
        out.append("(check-sat)")
        out.append("(reset)")
    return "\n".join(out) + "\n"


@pytest.mark.parametrize("seed", range(1, 25))
def test_rewrites_survive_reset(seed):
    r = subprocess.run([str(EXE), "--interactive", "--mode=verilator"],
                       input=_session(seed), capture_output=True, text=True, timeout=120)
    lines = r.stdout.split()
    assert lines == ["sat"] * 30, r.stdout[-2000:] + r.stderr[-2000:]


@pytest.mark.skipif(not Path("/proc/self/status").exists(), reason="needs /proc")
def test_reset_does_not_leak_arena_blocks():
    # The solver's own peak RSS (VmHWM), read while it is still running.
    def peak_kb(rounds):
        p = subprocess.Popen([str(EXE), "--interactive", "--mode=verilator"],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True)
        try:
            for _ in range(rounds):
                for i in range(60):
                    p.stdin.write(f"(declare-fun v{i} () (_ BitVec 16))\n")
                for i in range(59):
                    p.stdin.write(f"(assert (bvult v{i} (bvadd v{i + 1} #x0003)))\n")
                p.stdin.write("(check-sat)\n")
                p.stdin.flush()
                assert p.stdout.readline().strip() == "sat"
                p.stdin.write("(reset)\n")
            p.stdin.write("(check-sat)\n")
            p.stdin.flush()
            p.stdout.readline()
            status = Path(f"/proc/{p.pid}/status").read_text()
            return int(next(ln.split()[1] for ln in status.splitlines()
                            if ln.startswith("VmHWM:")))
        finally:
            p.kill()
            p.wait()

    small, large = peak_kb(100), peak_kb(800)
    # Leaking ~80 KB per reset grew the peak by over 10 MB across 700 resets.
    assert large - small < 2 * 1024, (small, large)
