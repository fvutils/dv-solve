"""--mode=verilator regressions found running riscv-dv under Verilator 5.052.

Each test replays the command shape Verilator 5.052 sends, reduced from
recorded riscv-dv sessions:

- **Stale CDCL route after (reset).** Verilator opens every session with
  `(set-logic QF_ABV)(check-sat)(reset)`. That empty problem was probed on CDCL,
  and the remembered route then forced the next problem onto CDCL even when it
  declared a >64-bit variable (riscv-dv's `vector_cfg.vtype` packed struct). The
  compile declined it, so every `cfg.randomize()` answered `unknown`.
- **Width cap.** At XLEN=64 that struct is 154 bits, past the old 128-bit cap.
- **Bit-blast diversify blow-up.** Flip-checking the bits of the wide struct
  through a 32-bit `bvudiv` of one of its fields never finished: the fanout cone
  was re-propagated in LIFO order, which is exponential on reconvergent logic.
- **UniGen2 enumeration on CDCL.** Verilator 5.052 samples a constraint set it
  has seen before by enumerating a hash cell: `push`, XOR constraints, then
  check-sat / get-value / blocking clause, up to 61 times. On the CDCL engine
  each query took longer than the last (up to the 10 s cap); these sessions
  now go to bit-blast.
"""
from __future__ import annotations

import random
import re
import subprocess
import time
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")

_HANDSHAKE = "(set-logic QF_ABV)\n(check-sat)\n(reset)\n"
_PRELUDE = (
    "(set-option :produce-models true)\n"
    "(set-logic {logic})\n"
    "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
    "(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))\n"
)
_VAL = re.compile(r"\(([\w.]+) #b([01]+)\)")


def _run(script: str, timeout: float = 30.0, args=("--interactive", "--mode=verilator")):
    r = subprocess.run([str(_EXE), *args], input=script, capture_output=True, text=True,
                       timeout=timeout)
    verdicts = [l for l in r.stdout.split() if l in ("sat", "unsat", "unknown")]
    return verdicts, dict((n, int(v, 2)) for n, v in _VAL.findall(r.stdout)), r


def _vtype_script(width: int, handshake: bool) -> str:
    # Shape of riscv-dv's vector_cfg constraints on vtype (vediv/vsew/vlmul fields).
    s = _HANDSHAKE if handshake else ""
    s += _PRELUDE.format(logic="ALL")
    s += f"(declare-fun vector_cfg.vl () (_ BitVec 32))\n"
    s += f"(declare-fun vector_cfg.vtype () (_ BitVec {width}))\n"
    s += ("(assert (= #b1 (__Vbv (= vector_cfg.vl "
          "(bvudiv #x00000200 ((_ extract 63 32) vector_cfg.vtype))))))\n")
    s += "(assert (= #b1 (__Vbv (= ((_ extract 95 64) vector_cfg.vtype) #x00000001))))\n"
    s += "(push 1)\n(check-sat)\n(get-value (vector_cfg.vl vector_cfg.vtype))\n"
    return s


@pytest.mark.parametrize("width", [122, 154])
@pytest.mark.parametrize("handshake", [False, True])
def test_wide_packed_struct_solves(width, handshake):
    verdicts, m, r = _run(_vtype_script(width, handshake))
    assert verdicts == (["sat"] if handshake else []) + ["sat"], r.stdout + r.stderr
    vtype, vl = m["vector_cfg.vtype"], m["vector_cfg.vl"]
    assert vtype < (1 << width)
    vsew = (vtype >> 32) & 0xFFFFFFFF
    assert vl == (0x200 // vsew if vsew else 0xFFFFFFFF)   # SMT-LIB: x udiv 0 = all ones
    assert (vtype >> 64) & 0xFFFFFFFF == 1


def test_unconstrained_wide_var_after_handshake():
    """The minimal form: the handshake, then one free 122-bit variable."""
    s = _HANDSHAKE + _PRELUDE.format(logic="ALL")
    s += "(declare-fun v () (_ BitVec 122))\n(push 1)\n(check-sat)\n"
    verdicts, _, r = _run(s, timeout=10)
    assert verdicts == ["sat", "sat"], r.stdout + r.stderr


def test_diversify_wide_struct_through_divider_is_fast():
    """With a nonzero seed every primary-input bit is flip-checked; through a
    bit-blasted divider this used to run for minutes per solve."""
    t0 = time.monotonic()
    for seed in range(1, 6):
        s = _vtype_script(154, handshake=False).replace(
            "(push 1)", f"(set-option :seed {seed})\n(push 1)")
        verdicts, _, r = _run(s, timeout=20, args=("--interactive",))
        assert verdicts == ["sat"], r.stdout + r.stderr
    assert time.monotonic() - t0 < 10


# riscv-dv riscv_int_numeric_corner_stream: an operand that is 0, 0x80000000, or in
# one of three ranges (verbatim from the recorded session).
_CORNER = ("(assert (= #b1 (bvor (bvor (bvor (bvor (bvand #b1 (__Vbv (= x #x00000000))) "
           "(bvand #b1 (__Vbv (= x #x80000000)))) (bvand #b1 (bvand (__Vbv (bvuge x #x00000001)) "
           "(__Vbv (bvule x #x0000000f))))) (bvand #b1 (bvand (__Vbv (bvuge x #x00000010)) "
           "(__Vbv (bvule x #xefffffff))))) (bvand #b1 (bvand (__Vbv (bvuge x #xf0000000)) "
           "(__Vbv (bvule x #xffffffff)))))))\n")


@pytest.mark.parametrize("nhash", [8, 10])
def test_unigen2_cell_enumeration(nhash):
    """Drive one UniGen2 cell enumeration the way Verilator 5.052 does, live:
    every model must satisfy the hash, differ from all earlier ones, and the
    whole cell must enumerate quickly. On CDCL the cost grew exponentially with
    the hash count (8 XORs: 30 s per cell; 10 XORs: the 10 s per-query cap);
    on bit-blast a cell takes about 0.1 s."""
    rng = random.Random(5052)
    rows = [[b for b in range(32) if rng.random() < 0.5] for _ in range(nhash)]
    target = [rng.getrandbits(1) for _ in range(nhash)]
    xors = " ".join("(bvxor #b0" + "".join(f" ((_ extract {b} {b}) x)" for b in row) + ")"
                    for row in rows)
    hash_assert = "(assert (= #b%s (concat %s)))\n" % ("".join(map(str, target)), xors)

    p = subprocess.Popen([str(_EXE), "--interactive", "--mode=verilator"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True, bufsize=1)

    def send(cmd):
        p.stdin.write(cmd)
        p.stdin.flush()

    def line():
        l = p.stdout.readline()
        assert l, "solver exited"
        return l.strip()

    try:
        send(_HANDSHAKE)
        assert line() == "sat"
        send(_PRELUDE.format(logic="QF_ABV") + "(declare-fun x () (_ BitVec 32))\n" + _CORNER)
        send("(push 1)\n" + hash_assert)
        seen, t0 = set(), time.monotonic()
        for _ in range(61):
            send("(check-sat)\n")
            v = line()
            assert v in ("sat", "unsat"), v
            if v == "unsat":
                break
            send("(get-value (x))\n")
            val = int(_VAL.search(line()).group(2), 2)
            for row, t in zip(rows, target):
                assert sum((val >> b) & 1 for b in row) % 2 == t, hex(val)
            assert val not in seen
            seen.add(val)
            send("(assert (not (and true (= x #b%s))))\n" % format(val, "032b"))
            assert time.monotonic() - t0 < 5, (len(seen), time.monotonic() - t0)
        assert len(seen) >= 20
        send("(pop 1)\n(exit)\n")
    finally:
        p.kill()
        p.wait()


_SEL = re.compile(r"\(\(select (\w+) #x([0-9a-f]+)\) #b([01]+)\)")


@pytest.mark.parametrize("hash_mode", ["honor", "ignore"])
def test_array_xor_diversity_rounds(hash_mode):
    """Verilator's diversity for array rand vars (solveDiversityXor): after the
    base solve, up to four `(= #bX (bvxor <element bits>))` rounds over a random
    half of all element bits, each followed by (check-sat). Every round must
    answer, and each model must keep the element ranges and (in honor mode)
    every XOR so far. test_recorded_array_xor_rounds pins the instance that
    answered `unknown` when such rounds were routed to CDCL."""
    n, rng = 30, random.Random(7)
    s = _HANDSHAKE + _PRELUDE.format(logic="QF_ABV")
    s += "(declare-fun A () (Array (_ BitVec 32) (_ BitVec 32)))\n"
    for i in range(n):
        s += ("(assert (= #b1 (bvand (__Vbv (bvuge (select A #x%08x) #x00000001)) "
              "(__Vbv (bvule (select A #x%08x) #x00000014)))))\n" % (i, i))
    getv = "(get-value (" + "".join("(select A #x%08x)" % i for i in range(n)) + "))\n"
    p = subprocess.Popen([str(_EXE), "--interactive", "--mode=verilator",
                          f"--verilator-hash={hash_mode}"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def send(cmd):
        p.stdin.write(cmd)
        p.stdin.flush()

    def model():
        text = ""
        while text.count("(") == 0 or text.count("(") != text.count(")"):
            text += p.stdout.readline()
        return {int(i, 16): int(v, 2) for _, i, v in _SEL.findall(text)}

    try:
        send(s + "(check-sat)\n")
        assert p.stdout.readline().strip() == "sat"            # handshake
        assert p.stdout.readline().strip() == "sat"
        send(getv)
        m = model()
        hashes = []
        for _ in range(4):
            # randomConstraint(): XOR of a random half of all rand bits
            bits = rng.sample([(i, b) for i in range(n) for b in range(32)], n * 32 // 2)
            tgt = rng.getrandbits(1)
            send("(assert (= #b%d (bvxor %s)))\n(check-sat)\n" % (tgt, " ".join(
                "((_ extract %d %d)(select A #x%08x))" % (b, b, i) for i, b in bits)))
            v = p.stdout.readline().strip()
            assert v in ("sat", "unsat"), v
            if v == "unsat":
                break                                           # Verilator stops here too
            if hash_mode == "honor":
                hashes.append((bits, tgt))
            send(getv)
            m = model()
            assert all(1 <= m[i] <= 20 for i in range(n)), m
            for hb, ht in hashes:
                assert sum((m[i] >> b) & 1 for i, b in hb) % 2 == ht
        send("(exit)\n")
    finally:
        p.kill()
        p.wait()


_XOR = re.compile(r"\(assert \(= #b([01]) \(bvxor ((?:\(\(_ extract \d+ \d+\)\(select __Varg1 #x[0-9a-f]+\)\) ?)+)\)\)\)")
_BIT = re.compile(r"\(\(_ extract (\d+) \d+\)\(select __Varg1 #x([0-9a-f]+)\)\)")


def test_recorded_array_xor_rounds():
    """The recorded session (tests/unit/data): every round answers, and each
    model keeps the element ranges and every XOR asserted so far."""
    src = (Path(__file__).parent / "data" / "vlt5052_array_xor_rounds.smt2").read_text(encoding="utf-8")
    getv = "(get-value (" + "".join("(select __Varg1 #x%08x)" % i for i in range(30)) + "))\n"
    verdicts, _, r = _run(src.replace("(exit)", getv + "(exit)"), timeout=60)
    assert "unknown" not in verdicts, r.stdout + r.stderr
    assert verdicts[:2] == ["sat", "sat"], verdicts
    xors = [(int(t), [(int(i, 16), int(b)) for b, i in _BIT.findall(body)])
            for t, body in _XOR.findall(src)]
    assert len(xors) == 4
    models = []
    for block in r.stdout.split("(((select")[1:]:           # one get-value reply each
        models.append({int(i, 16): int(v, 2) for _, i, v in _SEL.findall("((select" + block)})
    assert len(models) == verdicts[1:].count("sat"), (len(models), verdicts)
    # model k (k >= 1) follows XOR round k; Verilator stops at the first unsat
    for k, m in enumerate(models):
        assert all(1 <= m[i] <= 20 for i in range(30)), (k, m)
        for t, bits in xors[:k]:
            assert sum((m[i] >> b) & 1 for i, b in bits) % 2 == t, k
