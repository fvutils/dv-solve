"""Fidelity of the swizzle driver (tests/perf/vlt_protocol.py) to real Verilator.

The `<solver>-swizzle` arms of the results pages only mean something if the
driver says to the solver what Verilator 5.046 says. Two checks, each against
a real simulation compiled with the pinned Verilator and run with z3 behind a
logging wrapper:

- **Commands.** Every randomize() sends the same commands in the same order,
  with each XOR-hash assert reduced to its shape (the hash bit and the number
  of bits it covers: about half of all rand bits). Constraint text is reduced
  to "an assert": Verilator's encoding of an expression is its own.
- **Distribution.** For `a < b`, which both encode as `(bvult a b)`, the values
  real Verilator + z3 returns and the values the driver + z3 returns are the
  same distribution (two-sample chi-square on each variable's marginal).

Needs the bundled Verilator, a C++ compiler and z3; skips otherwise.
"""
from __future__ import annotations

import os
import random
import re
import shutil
import subprocess
from collections import Counter

import pytest

# tests.perf measures child CPU time with the POSIX-only resource module.
pytest.importorskip("resource")

from tests.perf import tools, vlt_protocol
from tests.perf.rand_benches import Bench

N = 2000

_A_LT_B_SV = """
class C;
  rand bit [5:0] a;
  rand bit [5:0] b;
  constraint c { a < b; }
endclass
module t;
  initial begin
    C c = new;
    for (int i = 0; i < %d; i++) begin
      if (!c.randomize()) $display("FAIL");
      $display("V %%0d %%0d", c.a, c.b);
    end
    $finish;
  end
endmodule
"""

_ADDR_SV = """
class C;
  rand bit [31:0] addr;
  constraint c { addr[1:0] == 0; addr >= 32'h1000_0000; addr < 32'h5000_0000; }
endclass
module t;
  initial begin
    C c = new;
    for (int i = 0; i < %d; i++) begin
      if (!c.randomize()) $display("FAIL");
      $display("V %%0d", c.addr);
    end
    $finish;
  end
endmodule
"""


def _tool(name):
    try:
        return tools.path(name)
    except Exception:
        return None


_VLT, _Z3 = _tool("verilator"), _tool("z3")
_HAVE = bool(_VLT and _Z3 and shutil.which("c++"))
# The perf job sets DVS_REQUIRE_FIDELITY: there a skip would let unverified
# swizzle numbers through, so a missing tool is a failure instead.
pytestmark = pytest.mark.skipif(
    not _HAVE and not os.environ.get("DVS_REQUIRE_FIDELITY"),
    reason="needs the bundled Verilator, a C++ compiler and z3")


@pytest.fixture(autouse=True)
def _tools_present():
    assert _HAVE, "the fidelity test needs Verilator, a C++ compiler and z3"


def _real(tmp_path, sv: str, n: int) -> tuple:
    """Compile and run `sv` with real Verilator + z3: (transcript, value rows)."""
    (tmp_path / "t.sv").write_text(sv % n)
    subprocess.run([str(_VLT), "--binary", "-Wno-fatal", "-Wno-lint", "--Mdir",
                    str(tmp_path / "obj"), "-o", "sim", str(tmp_path / "t.sv")],
                   check=True, capture_output=True)
    log = tmp_path / "trans.smt2"
    wrap = tmp_path / "solver.sh"
    wrap.write_text(f'#!/bin/sh\ntee -a "{log}" | "{_Z3}" -in\n')
    wrap.chmod(0o755)
    out = subprocess.run([str(tmp_path / "obj" / "sim")], capture_output=True, text=True,
                         env={**os.environ, "VERILATOR_SOLVER": str(wrap)}, check=True)
    rows = [tuple(int(x) for x in ln.split()[1:]) for ln in out.stdout.splitlines()
            if ln.startswith("V ")]
    assert "FAIL" not in out.stdout
    return log.read_text(encoding="utf-8"), rows


_HASH = re.compile(r"\(assert \(= #b[01] \(bvxor((?: \(\(_ extract \d+ \d+\) [\w.]+\))+)\)\)\)")


def _shape(transcript: str) -> list:
    """Per randomize(): its commands, constraint text and hash bits abstracted."""
    calls, cur = [], []
    for ln in transcript.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        m = _HASH.fullmatch(ln)
        if m:
            cur.append(f"HASH/{m.group(1).count('extract')}")
        elif ln.startswith("(assert "):
            cur.append("ASSERT")
        else:
            cur.append(ln)
        if ln == "(reset)":
            calls.append(tuple(cur))
            cur = []
    if "(check-sat)" in cur:
        # Verilator's last (reset) is still in its stream buffer when the
        # simulation exits, so the last call ends without one.
        calls.append(tuple(cur) + ("(reset)",))
    return calls[1:]                     # [0] is getSolver()'s liveness probe


def _driver(bench: Bench, n: int) -> tuple:
    log, rows = [], []
    s = vlt_protocol.Session([_Z3, "-in"], bench.widths, bench.smt_vlt(), log=log)
    rng = random.Random(1)
    try:
        for _ in range(n):
            m, _ = s.randomize(rng)
            rows.append(tuple(m[k] for k in bench.names))
    finally:
        s.close()
    return "".join(log), rows


def _same_commands(real: str, ours: str, n_asserts: tuple):
    rs, os_ = _shape(real), _shape(ours)
    assert len(rs) == len(os_)
    for r, o in zip(rs, os_):
        # The constraint asserts differ in number (each side's encoding);
        # everything else, including each hash's width, must match. The number
        # of hash rounds depends on z3's answers, so it may differ per call.
        def norm(c, k):
            out, seen = [], 0
            for x in c:
                if x == "ASSERT":
                    seen += 1
                    if seen == 1:
                        out.append("ASSERTS")
                    continue
                out.append(x)
            assert seen == k
            return out
        rn, on = norm(r, n_asserts[0]), norm(o, n_asserts[1])
        cut = lambda c: c[:c.index("(check-sat)") + 1]        # noqa: E731
        assert cut(rn) == cut(on)
        hashes = lambda c: {x for x in c if x.startswith("HASH/")}   # noqa: E731
        assert hashes(rn) == hashes(on)
        assert rn[-1] == on[-1] == "(reset)"


def _two_sample_p(xs: list, ys: list) -> float:
    from scipy.stats import chi2_contingency
    keys = sorted(set(xs) | set(ys))
    cx, cy = Counter(xs), Counter(ys)
    return float(chi2_contingency([[cx[k] for k in keys], [cy[k] for k in keys]])[1])


def test_a_lt_b_commands_and_distribution(tmp_path):
    real_log, real_rows = _real(tmp_path, _A_LT_B_SV, N)
    ours_log, our_rows = _driver(Bench("a_lt_b"), N)
    _same_commands(real_log, ours_log, (1, 1))
    assert len(real_rows) == len(our_rows) == N
    for i in range(2):                   # a, b marginals
        p = _two_sample_p([r[i] for r in real_rows], [r[i] for r in our_rows])
        assert p > 0.001, f"variable {i}: real Verilator and the driver differ (p={p:.2g})"


def test_wide_hash_commands(tmp_path):
    real_log, _ = _real(tmp_path, _ADDR_SV, 20)
    ours_log, _ = _driver(Bench("addr_aligned"), 20)
    # Verilator splits `addr[1:0] == 0, lo <= addr < hi` into three asserts,
    # as the benchmark does.
    _same_commands(real_log, ours_log, (3, 3))
    assert "HASH/16" in "".join(_shape(ours_log)[0])
