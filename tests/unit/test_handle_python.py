"""dv_solve.handle.SolveHandle: problems with arrays sized per call, from Python."""
from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import RANDOMIZE_OK, RANDOMIZE_UNSAT, SolveHandle
from dv_solve.problem import (BIN_BOR, BIN_EQ, BIN_LT, VAR_RAND, VAR_STATE)

_I32 = (-(1 << 31), (1 << 31) - 1)


def _inside_state_array():
    """x inside {a}, a a STATE array of int."""
    b = SolveProblemBuilder()
    b.add_var(0, width=32, is_signed=True, lo=_I32[0], hi=_I32[1])
    b.add_array(0, 32, True, VAR_STATE)
    eq = b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_elem(0, b.expr_loop_index(0)))
    b.add_constraint(b.expr_reduce(BIN_BOR, 0, 0, eq))
    return SolveHandle(b.finalize_bytes(), vars=[(32, True)], arrays=[(32, True)])


def test_inside_a_state_array_takes_only_its_values():
    h = _inside_state_array()
    seen = set()
    for seed in range(40):
        rc, vals, _ = h.randomize(seed, arrays=[[3, -5, 7]])
        assert rc == RANDOMIZE_OK
        seen.add(vals[0])
    assert seen == {3, -5, 7}


def test_the_array_is_read_on_each_call():
    h = _inside_state_array()
    for seed, elems in enumerate([[11], [2, 4], [100]]):
        rc, vals, _ = h.randomize(seed, arrays=[elems])
        assert rc == RANDOMIZE_OK and vals[0] in elems


def test_inside_an_empty_array_is_unsat():
    rc, _, _ = _inside_state_array().randomize(1, arrays=[[]])
    assert rc == RANDOMIZE_UNSAT


def test_size_bounds_a_var():
    b = SolveProblemBuilder()
    b.add_var(0, width=32, is_signed=True, lo=0, hi=_I32[1])
    b.add_array(0, 8, False, VAR_STATE)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_array_size(0)))
    h = SolveHandle(b.finalize_bytes(), vars=[(32, True)], arrays=[(8, False)])
    seen = set()
    for seed in range(30):
        rc, vals, _ = h.randomize(seed, arrays=[[9, 9, 9]])
        assert rc == RANDOMIZE_OK
        seen.add(vals[0])
    assert seen == {0, 1, 2}


def test_state_var_is_pinned_and_rand_array_is_written():
    """foreach (a[i]) a[i] < n, n a STATE var; a RAND array of 4."""
    b = SolveProblemBuilder()
    b.add_var(0, width=8, is_signed=False, lo=0, hi=255)
    b.set_var_kind(0, VAR_STATE)
    b.add_array(0, 8, False, VAR_RAND)
    b.foreach_begin(0, 0)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_elem(0, b.expr_loop_index(0)),
                                   b.expr_var(0)))
    b.foreach_end()
    h = SolveHandle(b.finalize_bytes(), vars=[(8, False)], arrays=[(8, False)])
    rc, vals, elems = h.randomize(5, state={0: 3}, arrays=[[0, 0, 0, 0]])
    assert rc == RANDOMIZE_OK
    assert vals[0] == 3
    assert len(elems[0]) == 4 and all(0 <= v < 3 for v in elems[0])
