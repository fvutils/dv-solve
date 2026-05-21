"""Python wrapper for the C SolveProblem API.

Usage::

    sp = SolveProblem()
    v0 = sp.add_var(0, width=32, is_signed=False, lo=0, hi=10)
    v1 = sp.add_var(1, width=32, is_signed=False, lo=0, hi=10)
    r  = sp.add_var(2, width=32, is_signed=False, lo=7, hi=7)
    sp.add_constraint(sp.expr_binary(BIN_ADD, sp.expr_var(0), sp.expr_var(1)))  # add = v0+v1
    # fix r == v0+v1 via another constraint...
"""
from __future__ import annotations

import ctypes
from typing import Sequence

from .lib import _load_lib

# ------------------------------------------------------------------ #
# BinOp / UnaryOp constants (must match zsp_problem.h)               #
# ------------------------------------------------------------------ #
BIN_ADD    = 0
BIN_SUB    = 1
BIN_MUL    = 2
BIN_DIV    = 3
BIN_MOD    = 4
BIN_BAND   = 5
BIN_BOR    = 6
BIN_BXOR   = 7
BIN_LSHIFT = 8
BIN_RSHIFT = 9
BIN_EQ     = 10
BIN_NEQ    = 11
BIN_LT     = 12
BIN_LTE    = 13
BIN_GT     = 14
BIN_GTE    = 15
BIN_AND    = 16
BIN_OR     = 17

UN_NEG    = 0
UN_NOT    = 1
UN_INVERT = 2

EXPR_NULL = 0xFFFF_FFFF
EXPR_CONCAT = 9
EXPR_SUM = 10
EXPR_COUNTONES = 11
EXPR_CLOG2 = 12
EXPR_ARRAY_SELECT = 13

# Default buffer size for a SolveProblem (64 KiB)
_SP_BUF_SIZE = 65536


class DistEntry(ctypes.Structure):
    """ctypes mirror of the C DistEntry struct."""
    _fields_ = [
        ("lo",           ctypes.c_int64),
        ("hi",           ctypes.c_int64),
        ("weight",       ctypes.c_uint32),
        ("is_per_value", ctypes.c_uint8),
        ("_dpad",        ctypes.c_uint8 * 3),
    ]


class SolveProblem:
    """Thin ctypes wrapper around the C ``SolveProblem`` API.

    The buffer backing the problem is kept alive for the lifetime of
    this object.  Call ``reset()`` to reuse the buffer for a fresh
    problem.
    """

    def __init__(self, buf_size: int = _SP_BUF_SIZE) -> None:
        lib = _load_lib()
        if lib is None:
            raise RuntimeError("libzsp_solver.so not found — native solver unavailable")
        self._lib = lib
        self._buf = (ctypes.c_uint8 * buf_size)()
        sp = lib.solve_problem_init(self._buf, buf_size)
        if sp is None:
            raise RuntimeError("solve_problem_init failed")
        self._sp = sp  # c_void_p value (integer address)

    def reset(self) -> None:
        """Reset the problem in place (clears all vars/constraints)."""
        self._lib.solve_problem_reset(self._sp)

    # ------------------------------------------------------------------ #
    # Variable / constraint builders                                       #
    # ------------------------------------------------------------------ #

    def add_var(
        self,
        var_id: int,
        width: int,
        is_signed: bool,
        lo: int,
        hi: int,
    ) -> int:
        """Declare a variable; returns its ExprRef (EXPR_NULL on overflow)."""
        return self._lib.problem_add_var(
            self._sp,
            ctypes.c_uint32(var_id),
            ctypes.c_uint8(width),
            ctypes.c_uint8(1 if is_signed else 0),
            ctypes.c_int64(lo),
            ctypes.c_int64(hi),
        )

    def add_constraint(self, root: int) -> int:
        """Add a constraint expression; returns ConstraintSpec ExprRef."""
        return self._lib.problem_add_constraint(self._sp, ctypes.c_uint32(root))

    def add_source(self, var_ids: Sequence[int]) -> int:
        """Add a source group."""
        arr = (ctypes.c_uint32 * len(var_ids))(*var_ids)
        return self._lib.problem_add_source(
            self._sp, ctypes.c_uint32(len(var_ids)), arr
        )


    def add_all_different(self, var_ids: Sequence[int]) -> int:
        """Add an AllDifferent constraint over the given variable IDs."""
        arr = (ctypes.c_uint32 * len(var_ids))(*var_ids)
        return self._lib.problem_add_all_different(
            self._sp, ctypes.c_uint32(len(var_ids)), arr
        )

    def add_at_least_one(self, var_ids: Sequence[int]) -> int:
        """Assert that at least one of *var_ids* is non-zero.

        Encodes the constraint ``(v0 != 0) OR (v1 != 0) OR ...`` as a
        disjunction: OR over (var_i != 0) expressions.  Equivalent to
        ``sum(var_ids) >= 1`` for boolean variables.

        Args:
            var_ids: Variable IDs; at least one must be non-zero.

        Returns:
            ExprRef of the top-level OR constraint, or EXPR_NULL on overflow.
        """
        if not var_ids:
            return EXPR_NULL
        # Build OR over (var_i != 0) terms.
        # BIN_NEQ has a native solver bug; use (v < 0) OR (v > 0) instead.
        terms = [
            self.expr_binary(BIN_OR,
                self.expr_binary(BIN_LT, self.expr_var(v), self.expr_const(0)),
                self.expr_binary(BIN_GT, self.expr_var(v), self.expr_const(0)))
            for v in var_ids
        ]
        root = terms[0]
        for t in terms[1:]:
            root = self.expr_binary(BIN_OR, root, t)
        return self.add_constraint(root)
    def expr_const(self, value: int, is_signed: bool = False) -> int:
        return self._lib.expr_const(
            self._sp, ctypes.c_int64(value), ctypes.c_uint8(1 if is_signed else 0)
        )

    def expr_var(self, var_id: int) -> int:
        return self._lib.expr_var(self._sp, ctypes.c_uint32(var_id))

    def expr_binary(self, op: int, lhs: int, rhs: int) -> int:
        return self._lib.expr_binary(
            self._sp,
            ctypes.c_int32(op),
            ctypes.c_uint32(lhs),
            ctypes.c_uint32(rhs),
        )

    def expr_unary(self, op: int, operand: int) -> int:
        return self._lib.expr_unary(
            self._sp, ctypes.c_int32(op), ctypes.c_uint32(operand)
        )

    def expr_ite(self, cond: int, then_e: int, else_e: int) -> int:
        return self._lib.expr_ite(
            self._sp,
            ctypes.c_uint32(cond),
            ctypes.c_uint32(then_e),
            ctypes.c_uint32(else_e),
        )

    def expr_in_range(self, value: int, lo: int, hi: int) -> int:
        return self._lib.expr_in_range(
            self._sp,
            ctypes.c_uint32(value),
            ctypes.c_uint32(lo),
            ctypes.c_uint32(hi),
        )

    def expr_in_set(self, value: int, elems: Sequence[int]) -> int:
        arr = (ctypes.c_uint32 * len(elems))(*elems)
        return self._lib.expr_in_set(
            self._sp,
            ctypes.c_uint32(value),
            ctypes.c_uint32(len(elems)),
            arr,
        )

    def expr_extend(
        self,
        operand: int,
        from_bits: int,
        to_bits: int,
        sign_extend: bool = False,
    ) -> int:
        return self._lib.expr_extend(
            self._sp,
            ctypes.c_uint32(operand),
            ctypes.c_uint8(from_bits),
            ctypes.c_uint8(to_bits),
            ctypes.c_uint8(1 if sign_extend else 0),
        )

    def expr_extract(self, operand: int, hi_bit: int, lo_bit: int) -> int:
        return self._lib.expr_extract(
            self._sp,
            ctypes.c_uint32(operand),
            ctypes.c_uint8(hi_bit),
            ctypes.c_uint8(lo_bit),
        )

    def expr_concat(self, hi: int, lo: int, lo_width: int) -> int:
        return self._lib.expr_concat(
            self._sp,
            ctypes.c_uint32(hi),
            ctypes.c_uint32(lo),
            ctypes.c_uint8(lo_width),
        )

    def add_soft_constraint(self, root: int, priority: int = 0) -> int:
        """Add a soft (relaxable) constraint with a priority.
        Higher priority value = lower priority (relaxed first on conflict)."""
        return self._lib.problem_add_soft_constraint(
            self._sp, ctypes.c_uint32(root), ctypes.c_uint32(priority)
        )

    def add_dist(self, var_id: int, entries: Sequence[dict]) -> int:
        """Add a distribution constraint on a variable.

        Each entry is a dict with keys: lo, hi, weight, is_per_value.
        is_per_value=True means := (weight per value),
        is_per_value=False means :/ (weight divided across range).
        """
        arr = (DistEntry * len(entries))()
        for i, e in enumerate(entries):
            arr[i].lo = e["lo"]
            arr[i].hi = e["hi"]
            arr[i].weight = e["weight"]
            arr[i].is_per_value = 1 if e.get("is_per_value", True) else 0
        return self._lib.problem_add_dist(
            self._sp,
            ctypes.c_uint32(var_id),
            ctypes.c_uint32(len(entries)),
            arr,
        )
