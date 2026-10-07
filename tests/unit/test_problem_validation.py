"""Problems the engines cannot read as written are refused, not guessed at.

Each case here used to compile and solve:

- an expression naming an undeclared variable read it as a zero-initialised
  slot, so ``x > v3`` with ``v3`` never declared solved with ``v3 == 0``;
- a duplicate or out-of-range variable id left a slot undeclared (or failed
  with the out-of-memory code);
- a problem the SystemVerilog-sizing elaborator could not handle (an
  expression wider than 255 bits, or nested too deeply) was compiled in its
  raw form, under different sizing rules from the ones it was written for.
"""
from __future__ import annotations

import threading

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import CompileUnsupportedError, SolveCtx, SOLVE_OK
from dv_solve.problem import BIN_AND, BIN_EQ, BIN_GT


def _problem(decl, refs):
    b = SolveProblemBuilder()
    for vid in decl:
        b.add_var(vid, 8, False, 0, 255)
    b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(refs[0]), b.expr_var(refs[1])))
    return b.finalize()[0]


def test_well_formed_problem_solves():
    with SolveCtx(_problem([0, 1], [0, 1])) as ctx:
        assert ctx.solve(seed=1) == SOLVE_OK
        assert ctx.get_value(0) > ctx.get_value(1)


@pytest.mark.parametrize("decl,refs", [
    ([0], [0, 3]),        # expression names an undeclared variable
    ([0, 0], [0, 0]),     # id declared twice (and id 1 never)
    ([0, 5], [0, 5]),     # ids not 0..n-1
], ids=["undeclared", "duplicate", "sparse"])
def test_bad_variable_ids_are_refused(decl, refs):
    with pytest.raises(ValueError, match="variable ids"):
        SolveCtx(_problem(decl, refs))


def test_expression_wider_than_255_bits_is_refused():
    b = SolveProblemBuilder()
    for i in range(4):
        b.add_var(i, 64, False, 0, 2**63 - 1)
    e = b.expr_var(0)
    for i in range(1, 4):
        e = b.expr_concat(e, b.expr_var(i), 64)       # 256 bits
    b.add_constraint(b.expr_binary(BIN_EQ, e, b.expr_const(5)))
    with pytest.raises(CompileUnsupportedError):
        SolveCtx(b.finalize()[0])


def _deep_problem(depth):
    b = SolveProblemBuilder()
    b.add_var(0, 8, False, 0, 255)
    gt3 = lambda: b.expr_binary(BIN_GT, b.expr_var(0), b.expr_const(3))
    e = gt3()
    for _ in range(depth):
        e = b.expr_binary(BIN_AND, e, gt3())
    b.add_constraint(e)
    return b.finalize()[0]


def test_too_deep_expression_is_refused():
    with pytest.raises(CompileUnsupportedError):
        SolveCtx(_deep_problem(25000))


def test_deep_expression_on_a_small_stack_is_refused_not_a_crash():
    # Under the depth limit, but too deep for a 512 KB stack (a Windows
    # thread has 1 MB, a macOS secondary thread 512 KB): compile must notice
    # the stack running out and refuse, not overflow it.
    problem = _deep_problem(15000)
    outcome = []

    def compile_it():
        try:
            SolveCtx(problem)
            outcome.append("compiled")
        except CompileUnsupportedError:
            outcome.append("refused")

    old = threading.stack_size(512 * 1024)
    try:
        t = threading.Thread(target=compile_it)
        t.start()
    finally:
        threading.stack_size(old)
    t.join()
    assert outcome == ["refused"]


def test_bitblaster_refuses_undeclared_variable():
    from dv_solve.bvsat import BVSatCtx
    with pytest.raises(RuntimeError):
        BVSatCtx(_problem([0], [0, 3]))
