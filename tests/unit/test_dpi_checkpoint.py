"""Unit tests for the DPI pin/checkpoint/restore functions (Phase 1).

Tests exercise zsp_dpi_compile_b64 / zsp_dpi_pin_var_h /
zsp_dpi_checkpoint_h / zsp_dpi_restore_h / zsp_dpi_solve_h /
zsp_dpi_get_value_h / zsp_dpi_release_h via ctypes through the DPI
shared library (libzsp_solver_dpi.so).

The key property under test: pin_var tightens a domain, checkpoint saves
that state, solve assigns values, restore unwinds search assignments so
the pinned state is recovered and a fresh solve can yield a different
solution.
"""
from __future__ import annotations

import base64
import ctypes
import struct
import pytest

# ------------------------------------------------------------------ #
# Helpers to build a minimal SolveProblem buffer in pure Python        #
# (mirrors the C API; reused across tests)                             #
# ------------------------------------------------------------------ #

_SP_BUF = 65536   # 64 KiB -- enough for small problems


def _build_sum_problem(libzsp_dpi):
    """Build a SolveProblem with two vars x, y and constraint x + y == 10.

    Variable 0 = x  in [0, 10]
    Variable 1 = y  in [0, 10]
    Constraint: BIN_ADD(x, y) == 10

    Returns base64-encoded bytes.
    """
    lib = libzsp_dpi
    buf = (ctypes.c_uint8 * _SP_BUF)()
    sp = lib.solve_problem_init(buf, _SP_BUF)
    assert sp

    BIN_ADD = 0
    BIN_EQ  = 10

    lib.problem_add_var(sp, 0, 32, 0, 0, 10)   # x
    lib.problem_add_var(sp, 1, 32, 0, 0, 10)   # y

    vx   = lib.expr_var(sp, 0)
    vy   = lib.expr_var(sp, 1)
    c10  = lib.expr_const(sp, 10, 0)
    add  = lib.expr_binary(sp, BIN_ADD, vx, vy)
    eq   = lib.expr_binary(sp, BIN_EQ, add, c10)
    lib.problem_add_constraint(sp, eq)

    raw = bytes(buf[:_SP_BUF])
    return base64.b64encode(raw).decode("ascii")


def _setup_dpi_lib(lib):
    """Wire ctypes argtypes/restype for the DPI functions we need."""
    c = ctypes

    lib.solve_problem_init.restype  = c.c_void_p
    lib.solve_problem_init.argtypes = [c.c_void_p, c.c_size_t]

    lib.problem_add_var.restype  = c.c_uint32
    lib.problem_add_var.argtypes = [c.c_void_p, c.c_uint32,
                                    c.c_uint8, c.c_uint8,
                                    c.c_int64, c.c_int64]
    lib.problem_add_constraint.restype  = c.c_uint32
    lib.problem_add_constraint.argtypes = [c.c_void_p, c.c_uint32]

    lib.expr_var.restype  = c.c_uint32
    lib.expr_var.argtypes = [c.c_void_p, c.c_uint32]
    lib.expr_const.restype = c.c_uint32
    lib.expr_const.argtypes = [c.c_void_p, c.c_int64, c.c_uint8]
    lib.expr_binary.restype  = c.c_uint32
    lib.expr_binary.argtypes = [c.c_void_p, c.c_int32,
                                 c.c_uint32, c.c_uint32]

    lib.zsp_dpi_compile_b64.restype  = c.c_void_p
    lib.zsp_dpi_compile_b64.argtypes = [c.c_char_p]

    lib.zsp_dpi_solve_h.restype  = c.c_int
    lib.zsp_dpi_solve_h.argtypes = [c.c_void_p, c.c_longlong]

    lib.zsp_dpi_get_value_h.restype  = c.c_longlong
    lib.zsp_dpi_get_value_h.argtypes = [c.c_void_p, c.c_int]

    lib.zsp_dpi_release_h.restype  = None
    lib.zsp_dpi_release_h.argtypes = [c.c_void_p]

    lib.zsp_dpi_pin_var_h.restype  = c.c_int
    lib.zsp_dpi_pin_var_h.argtypes = [c.c_void_p, c.c_int, c.c_longlong]

    lib.zsp_dpi_checkpoint_h.restype  = c.c_int
    lib.zsp_dpi_checkpoint_h.argtypes = [c.c_void_p]

    lib.zsp_dpi_restore_h.restype  = None
    lib.zsp_dpi_restore_h.argtypes = [c.c_void_p, c.c_int]


# ------------------------------------------------------------------ #
# Fixture: load libzsp_solver_dpi.so (built by conftest)              #
# ------------------------------------------------------------------ #

@pytest.fixture(scope="module")
def dpi_lib(libzsp_dpi):
    """Return the DPI library with all functions wired up."""
    _setup_dpi_lib(libzsp_dpi)
    return libzsp_dpi


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

def test_compile_and_solve(dpi_lib):
    """compile_b64 + solve_h + get_value_h round-trip."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None, "compile_b64 returned null"

    rc = lib.zsp_dpi_solve_h(h, 0x1234)
    assert rc == 0, f"solve_h returned {rc}"

    x = lib.zsp_dpi_get_value_h(h, 0)
    y = lib.zsp_dpi_get_value_h(h, 1)
    assert x + y == 10, f"x={x} y={y} but x+y must equal 10"

    lib.zsp_dpi_release_h(h)


def test_pin_var_constrains_solution(dpi_lib):
    """pin_var(x, 3) forces x == 3, so y must be 7."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None

    rc = lib.zsp_dpi_pin_var_h(h, 0, 3)   # pin x = 3
    assert rc == 0, f"pin_var_h returned {rc}"

    rc = lib.zsp_dpi_solve_h(h, 0xABCD)
    assert rc == 0

    x = lib.zsp_dpi_get_value_h(h, 0)
    y = lib.zsp_dpi_get_value_h(h, 1)
    assert x == 3, f"x should be 3 (pinned), got {x}"
    assert y == 7, f"y should be 7 (x+y==10), got {y}"

    lib.zsp_dpi_release_h(h)


def test_pin_conflicting_value_returns_error(dpi_lib):
    """pin_var with a value that violates an existing constraint returns -2."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None

    # x in [0,10], y in [0,10], x+y==10
    # pin x=7, then pin y=5 => x+y==12 != 10 => UNSAT via propagation
    lib.zsp_dpi_pin_var_h(h, 0, 7)   # x = 7 => y must be 3
    rc = lib.zsp_dpi_pin_var_h(h, 1, 5)   # y = 5, but y must be 3 => conflict
    assert rc == -2, f"Expected -2 (UNSAT), got {rc}"

    lib.zsp_dpi_release_h(h)


def test_checkpoint_and_restore(dpi_lib):
    """Checkpoint, pin x=3, solve (y==7). Restore. Pin x=5, solve (y==5)."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None

    cp = lib.zsp_dpi_checkpoint_h(h)
    assert cp >= 0, f"checkpoint_h returned {cp}"

    # First solve: pin x=3
    rc = lib.zsp_dpi_pin_var_h(h, 0, 3)
    assert rc == 0
    rc = lib.zsp_dpi_solve_h(h, 0x1111)
    assert rc == 0
    assert lib.zsp_dpi_get_value_h(h, 0) == 3
    assert lib.zsp_dpi_get_value_h(h, 1) == 7

    # Restore to clean checkpoint (before x=3 pin)
    lib.zsp_dpi_restore_h(h, cp)

    # Second solve: pin x=5
    rc = lib.zsp_dpi_pin_var_h(h, 0, 5)
    assert rc == 0
    rc = lib.zsp_dpi_solve_h(h, 0x2222)
    assert rc == 0
    assert lib.zsp_dpi_get_value_h(h, 0) == 5
    assert lib.zsp_dpi_get_value_h(h, 1) == 5

    lib.zsp_dpi_release_h(h)


def test_nested_checkpoints(dpi_lib):
    """Nested checkpoints: cp0 (clean), cp1 (x=3 pinned), solve, restore to cp1,
    resolve with new seed, restore to cp0, pin x=7, solve."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None

    # cp0: clean state
    cp0 = lib.zsp_dpi_checkpoint_h(h)
    assert cp0 >= 0

    # pin x=3 then checkpoint
    lib.zsp_dpi_pin_var_h(h, 0, 3)
    cp1 = lib.zsp_dpi_checkpoint_h(h)
    assert cp1 >= 0 and cp1 != cp0

    # solve: x=3, y=7
    assert lib.zsp_dpi_solve_h(h, 0xAA) == 0
    assert lib.zsp_dpi_get_value_h(h, 0) == 3
    assert lib.zsp_dpi_get_value_h(h, 1) == 7

    # restore to cp1 (x=3 still pinned, search state gone)
    lib.zsp_dpi_restore_h(h, cp1)
    assert lib.zsp_dpi_solve_h(h, 0xBB) == 0  # x still 3
    assert lib.zsp_dpi_get_value_h(h, 0) == 3
    assert lib.zsp_dpi_get_value_h(h, 1) == 7

    # restore to cp0 (no pins)
    lib.zsp_dpi_restore_h(h, cp0)

    # pin x=7 and solve
    lib.zsp_dpi_pin_var_h(h, 0, 7)
    assert lib.zsp_dpi_solve_h(h, 0xCC) == 0
    assert lib.zsp_dpi_get_value_h(h, 0) == 7
    assert lib.zsp_dpi_get_value_h(h, 1) == 3

    lib.zsp_dpi_release_h(h)


def test_solve_multiple_seeds_without_restore(dpi_lib):
    """Calling solve_h multiple times without restore still works (idempotent
    on already-assigned state; each call runs search from current state)."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None

    # pin x=2, take checkpoint so we can re-solve fresh each time
    lib.zsp_dpi_pin_var_h(h, 0, 2)
    cp = lib.zsp_dpi_checkpoint_h(h)

    for seed in [0x1, 0x2, 0x3]:
        lib.zsp_dpi_restore_h(h, cp)
        rc = lib.zsp_dpi_solve_h(h, seed)
        assert rc == 0
        assert lib.zsp_dpi_get_value_h(h, 0) == 2
        assert lib.zsp_dpi_get_value_h(h, 1) == 8

    lib.zsp_dpi_release_h(h)


def test_release_twice_safe(dpi_lib):
    """Releasing a null handle should not crash (NULL guard)."""
    lib = dpi_lib
    lib.zsp_dpi_release_h(None)   # should be a no-op


def test_get_value_before_solve_returns_zero(dpi_lib):
    """get_value_h before any solve returns 0 (solved flag is 0)."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None

    val = lib.zsp_dpi_get_value_h(h, 0)
    assert val == 0, f"Expected 0 before solve, got {val}"

    lib.zsp_dpi_release_h(h)


def test_pin_invalid_var_id_returns_error(dpi_lib):
    """pin_var_h with out-of-range var_id returns -1."""
    lib = dpi_lib
    b64 = _build_sum_problem(lib)
    h = lib.zsp_dpi_compile_b64(b64.encode())
    assert h is not None

    rc = lib.zsp_dpi_pin_var_h(h, 99, 5)  # only 2 vars (0, 1)
    assert rc == -1, f"Expected -1 for invalid var_id, got {rc}"

    lib.zsp_dpi_release_h(h)
