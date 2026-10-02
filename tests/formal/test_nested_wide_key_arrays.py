"""Nested arrays and 128-bit keys, the shapes Verilator emits for queues of
queues, arrays in structs and string-keyed associative arrays.

dv-solve keeps these on the sparse path: a nested (Array I (Array J D)) is one
flat array keyed by I ++ J, and keys hold 128 bits. Both take constant indices
only; anything else (a symbolic index, store, array =, ite) answers `unknown`.

A seeded generator cross-checks every answer against z3, and every `sat` model
is read back with get-value in Verilator's form -- (select (select A #x..) #x..)
-- and pinned into the problem, which z3 must still find satisfiable.

Scale with NESTED_FUZZ_N (default 300).
"""
from __future__ import annotations

import os
import random
import re
import shutil
import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
_N = int(os.environ.get("NESTED_FUZZ_N", "300"))

pytestmark = [
    pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built"),
    pytest.mark.skipif(shutil.which("z3") is None, reason="z3 not on PATH"),
]


def _hex(v: int, w: int) -> str:
    return f"#x{v:0{(w + 3) // 4}x}"


def _gen(seed: int):
    """A problem over constant-index elements of nested and wide-key arrays.
    Returns (script without check-sat, element terms)."""
    r = random.Random(seed)
    dw = r.choice([4, 8, 32])
    lines = ["(set-logic QF_ABV)"]
    elems = []
    for a in range(r.randint(1, 3)):
        if r.random() < 0.5:     # nested
            iw, jw = r.choice([(4, 8), (32, 32), (64, 64), (8, 32)])
            lines.append(f"(declare-fun N{a} () (Array (_ BitVec {iw}) "
                         f"(Array (_ BitVec {jw}) (_ BitVec {dw}))))")
            # Keys that collide if the concatenation is wrong: (k, j) and (j, k).
            ks = [r.randrange(min(1 << iw, 4)) for _ in range(2)]
            js = [r.randrange(min(1 << jw, 4)) for _ in range(2)]
            for k in ks:
                for j in js:
                    elems.append(f"(select (select N{a} {_hex(k, iw)}) {_hex(j, jw)})")
        else:                    # wide or narrow flat key
            kw = r.choice([32, 64, 128])
            lines.append(f"(declare-fun W{a} () (Array (_ BitVec {kw}) (_ BitVec {dw})))")
            base = r.getrandbits(kw)
            for d in (0, 1 << (kw - 1), 1, 1 << 64 if kw > 64 else 2):
                elems.append(f"(select W{a} {_hex((base ^ d) % (1 << kw), kw)})")
    vals = [f"v{i}" for i in range(2)]
    for v in vals:
        lines.append(f"(declare-fun {v} () (_ BitVec {dw}))")
    elems = sorted(set(elems))
    for _ in range(r.randint(2, 6)):
        a, b = r.choice(elems), r.choice(elems + vals)
        p = f"(= {a} {b})" if r.random() < 0.6 else f"(bvult {a} {b})"
        lines.append(f"(assert {f'(not {p})' if r.random() < 0.4 else p})")
    return "\n".join(lines) + "\n", elems


def _keys(reply: str):
    """((key value) ...) pairs of a get-value reply, keys balanced."""
    out, i = [], 0
    while (i := reply.find("((select", i)) >= 0:
        depth = 0
        for j in range(i + 1, len(reply)):
            depth += reply[j] == "("
            depth -= reply[j] == ")"
            if depth == 0:
                break
        m = re.match(r"\s*(#b[01]+)", reply[j + 1:])
        out.append((reply[i + 1:j + 1], m.group(1)))
        i = j
    return out


@pytest.mark.parametrize("seed", range(_N))
def test_nested_and_wide_key_arrays_match_z3(seed):
    body, elems = _gen(seed)
    q = body + "(check-sat)\n(get-value (" + " ".join(elems) + "))\n"
    dv = subprocess.run([str(_EXE), "--interactive"], input=q, capture_output=True,
                        text=True, timeout=60).stdout
    z = subprocess.run(["z3", "-in"], input=body + "(check-sat)\n", capture_output=True,
                       text=True, timeout=60).stdout.split()[0]
    verdict = dv.split()[0]
    assert verdict == z, f"dv={verdict} z3={z}\n{body}"
    if verdict != "sat":
        return
    pairs = _keys(dv)
    assert len(pairs) == len(elems), dv
    assert {k for k, _ in pairs} == set(elems), dv     # Verilator's echo form
    pinned = body + "".join(f"(assert (= {k} {v}))\n" for k, v in pairs) + "(check-sat)\n"
    z2 = subprocess.run(["z3", "-in"], input=pinned, capture_output=True, text=True,
                        timeout=60).stdout.split()[0]
    assert z2 == "sat", f"model rejected by z3\n{pinned}"


@pytest.mark.parametrize("script", [
    # A symbolic index, a store or an equality on a nested array: unknown.
    "(declare-fun i () (_ BitVec 32))"
    "(assert (= (select (select N i) #x00000000) #x01))",
    "(declare-fun i () (_ BitVec 32))"
    "(assert (= (select (select N #x00000000) i) #x01))",
    "(assert (= (select (select (store N #x00000000 M) #x00000000) #x00000000) #x01))",
    "(assert (= N (store N #x00000001 M)))",
    "(declare-fun c () Bool)(assert (= (select (select (ite c N N) #x00000000) #x00000000) #x01))",
    # A symbolic index into a 128-bit-key array.
    "(declare-fun k () (_ BitVec 128))(assert (= (select W k) #x01))",
])
def test_unsupported_nested_shapes_are_unknown(script):
    head = ("(set-logic QF_ABV)"
            "(declare-fun N () (Array (_ BitVec 32) (Array (_ BitVec 32) (_ BitVec 8))))"
            "(declare-fun M () (Array (_ BitVec 32) (_ BitVec 8)))"
            "(declare-fun W () (Array (_ BitVec 128) (_ BitVec 8)))")
    out = subprocess.run([str(_EXE), "--interactive"], input=head + script + "(check-sat)",
                         capture_output=True, text=True, timeout=60).stdout.split()
    assert out[-1] == "unknown", out
