"""Regression test for BUG-3: the search's domain-bisection must explore the
*upper* half of a variable's domain.

The two-phase bisection previously kept its phase flag on the volatile decision
record, so phase 2 (upper half) never ran and a satisfiable problem whose only
solutions sat in a variable's upper half was reported UNSAT. This only bites when
a constraint is *not* bounds-consistent (propagation can't prune the wrong half
at the root), so the search alone must find the solution.

Every production propagator is now bounds-consistent, which masks the search
behavior — so this test deliberately forces an under-propagating scenario via the
`DV_NO_BITSLICE_BACKWARD` test knob (disables backward bit-slice pruning, leaving
it forward-only). With that knob set, `field == extract(x, hi, lo)` with the
field pinned and `x` free can only be solved by a *complete* search. The knob is
read once per process, so the check runs in a subprocess with the env set.
"""
from __future__ import annotations

import os
import subprocess
import sys
import textwrap

import pytest

from dv_solve.lib import _find_library


_CHILD = textwrap.dedent(
    """
    import sys
    sys.path.insert(0, %(src)r)
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.problem import BIN_EQ
    from dv_solve.ctx import SolveCtx

    def solve(fv, hi, lo):
        b = SolveProblemBuilder()
        b.add_var(0, 16, False, 0, 0xFFFF)   # operand free over full domain
        b.add_var(1, 8, False, fv, fv)       # slice pinned
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(1), b.expr_extract(b.expr_var(0), hi, lo)))
        buf, _ = b.finalize()
        c = SolveCtx(buf)
        rc = c.solve(seed=1)
        x = c.get_value(0) if rc == 0 else None
        c.destroy()
        return rc, x

    fails = []
    # Field values span the operand's lower AND upper halves; the upper-half
    # ones (>=0x80 for the top slice) are the exact BUG-3 trigger.
    cases = [(v, 15, 8) for v in (0x00, 0x40, 0x80, 0xAB, 0xFF)]
    cases += [(v, 7, 0) for v in (0x00, 0xCD, 0xFF)]
    for fv, hi, lo in cases:
        rc, x = solve(fv, hi, lo)
        ok = rc == 0 and ((x >> lo) & 0xFF) == fv
        if not ok:
            fails.append((hex(fv), hi, lo, rc, None if x is None else hex(x)))
    if fails:
        print("FAILS", fails)
        sys.exit(1)
    print("OK")
    """
)


def test_upper_half_search_completeness():
    lib = _find_library()
    if lib is None:
        pytest.skip("libdv_solve.so not found for subprocess")
    src = str((__import__("pathlib").Path(__file__).parent.parent.parent / "src"))
    env = dict(os.environ)
    env["DV_NO_BITSLICE_BACKWARD"] = "1"   # force the under-propagating path
    env["ZSP_SOLVER_PATH"] = str(lib)
    proc = subprocess.run(
        [sys.executable, "-c", _CHILD % {"src": src}],
        env=env, capture_output=True, text=True, timeout=120,
    )
    assert proc.returncode == 0, (
        "search failed to find upper-half solutions (BUG-3 regression):\n"
        "stdout: %s\nstderr: %s" % (proc.stdout, proc.stderr)
    )
    assert "OK" in proc.stdout
