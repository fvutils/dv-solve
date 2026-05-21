"""Python wrapper for the C SolveProblemBuilder API.

Growable alternative to SolveProblem that never overflows.  Same API
surface (add_var, expr_const, expr_binary, ...) but backed by the C
builder which uses linked blocks and produces exact-sized buffers.

Usage::

    b = SolveProblemBuilder()
    b.add_var(0, width=8, is_signed=False, lo=0, hi=255)
    b.add_var(1, width=8, is_signed=False, lo=0, hi=255)
    b.add_constraint(b.expr_binary(BIN_LTE, b.expr_var(0), b.expr_var(1)))
    problem_bytes, size = b.finalize()
    # problem_bytes is a ctypes buffer containing a valid SolveProblem
"""
from __future__ import annotations

import ctypes
from typing import Optional, Sequence, Tuple

from .lib import _load_lib
from .problem import EXPR_NULL


class SolveProblemBuilder:
    """Growable problem builder backed by the C ``SolveProblemBuilder``.

    Unlike ``SolveProblem``, this never returns ``EXPR_NULL`` due to
    buffer overflow.  Allocation failure (malloc OOM) raises RuntimeError.
    """

    def __init__(self, block_size: int = 4096, lib=None) -> None:
        if lib is None:
            lib = _load_lib()
        if lib is None:
            raise RuntimeError(
                "libzsp_solver.so not found -- native solver unavailable"
            )
        self._lib = lib
        self._b = lib.builder_create(block_size, None)
        if self._b is None:
            raise RuntimeError("builder_create returned NULL")
        self._finalized_bufs: list = []  # prevent GC of finalized buffers

    def reset(self) -> None:
        """Reset to empty state, reusing block memory."""
        self._lib.builder_reset(self._b)

    def destroy(self) -> None:
        """Free all native resources."""
        if self._b is not None:
            self._lib.builder_destroy(self._b)
            self._b = None

    def __del__(self) -> None:
        if hasattr(self, "_b"):
            self.destroy()

    @property
    def virtual_used(self) -> int:
        """Bytes allocated in the virtual address space so far."""
        return self._lib.builder_virtual_used(self._b)

    # ------------------------------------------------------------------ #
    # Finalize                                                             #
    # ------------------------------------------------------------------ #

    def finalize(self) -> Tuple[ctypes.Array, int]:
        """Produce a contiguous SolveProblem buffer.

        Returns (ctypes_buffer, size_bytes).  The buffer contains a valid
        ``SolveProblem`` that can be passed to ``solver_compile``.
        """
        size = ctypes.c_size_t(0)
        sp_ptr = self._lib.builder_finalize(self._b, ctypes.byref(size))
        if sp_ptr is None or sp_ptr == 0:
            raise RuntimeError("builder_finalize returned NULL")

        sz = size.value
        # Copy the C-allocated buffer into a ctypes-managed array so Python
        # owns the memory and it stays alive as long as needed.
        buf = (ctypes.c_uint8 * sz)()
        ctypes.memmove(buf, sp_ptr, sz)
        # Free the C-allocated buffer
        self._lib.builder_free_problem(self._b, sp_ptr, sz)
        return buf, sz

    def finalize_bytes(self) -> bytes:
        """Produce the SolveProblem buffer as a Python bytes object."""
        buf, sz = self.finalize()
        return bytes(buf)

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
        """Declare a variable; returns its ExprRef."""
        ref = self._lib.builder_add_var(
            self._b,
            ctypes.c_uint32(var_id),
            ctypes.c_uint8(width),
            ctypes.c_uint8(1 if is_signed else 0),
            ctypes.c_int64(lo),
            ctypes.c_int64(hi),
        )
        if ref == EXPR_NULL:
            raise RuntimeError("builder_add_var returned EXPR_NULL (malloc failure)")
        return ref

    def add_constraint(self, root: int) -> int:
        """Add a constraint expression; returns ConstraintSpec ExprRef."""
        ref = self._lib.builder_add_constraint(
            self._b, ctypes.c_uint32(root)
        )
        if ref == EXPR_NULL:
            raise RuntimeError("builder_add_constraint returned EXPR_NULL")
        return ref

    def add_source(self, var_ids: Sequence[int]) -> int:
        """Add a source group."""
        arr = (ctypes.c_uint32 * len(var_ids))(*var_ids)
        ref = self._lib.builder_add_source(
            self._b, ctypes.c_uint32(len(var_ids)), arr
        )
        if ref == EXPR_NULL:
            raise RuntimeError("builder_add_source returned EXPR_NULL")
        return ref


    def add_all_different(self, var_ids: Sequence[int]) -> int:
        """Add an AllDifferent constraint over the given variable IDs."""
        arr = (ctypes.c_uint32 * len(var_ids))(*var_ids)
        ref = self._lib.builder_add_all_different(
            self._b, ctypes.c_uint32(len(var_ids)), arr
        )
        if ref == EXPR_NULL:
            raise RuntimeError("builder_add_all_different returned EXPR_NULL")
        return ref
    # ------------------------------------------------------------------ #
    # Expression builders                                                  #
    # ------------------------------------------------------------------ #

    def expr_const(self, value: int, is_signed: bool = False) -> int:
        return self._lib.builder_expr_const(
            self._b, ctypes.c_int64(value),
            ctypes.c_uint8(1 if is_signed else 0),
        )

    def expr_var(self, var_id: int) -> int:
        return self._lib.builder_expr_var(
            self._b, ctypes.c_uint32(var_id)
        )

    def expr_binary(self, op: int, lhs: int, rhs: int) -> int:
        return self._lib.builder_expr_binary(
            self._b,
            ctypes.c_uint32(op),
            ctypes.c_uint32(lhs),
            ctypes.c_uint32(rhs),
        )

    def expr_unary(self, op: int, operand: int) -> int:
        return self._lib.builder_expr_unary(
            self._b, ctypes.c_uint32(op), ctypes.c_uint32(operand)
        )

    def expr_ite(self, cond: int, then_e: int, else_e: int) -> int:
        return self._lib.builder_expr_ite(
            self._b,
            ctypes.c_uint32(cond),
            ctypes.c_uint32(then_e),
            ctypes.c_uint32(else_e),
        )

    def expr_in_range(self, value: int, lo: int, hi: int) -> int:
        return self._lib.builder_expr_in_range(
            self._b,
            ctypes.c_uint32(value),
            ctypes.c_uint32(lo),
            ctypes.c_uint32(hi),
        )

    def expr_in_set(self, value: int, elems: Sequence[int]) -> int:
        arr = (ctypes.c_uint32 * len(elems))(*elems)
        return self._lib.builder_expr_in_set(
            self._b,
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
        return self._lib.builder_expr_extend(
            self._b,
            ctypes.c_uint32(operand),
            ctypes.c_uint8(from_bits),
            ctypes.c_uint8(to_bits),
            ctypes.c_uint8(1 if sign_extend else 0),
        )

    def expr_extract(self, operand: int, hi_bit: int, lo_bit: int) -> int:
        return self._lib.builder_expr_extract(
            self._b,
            ctypes.c_uint32(operand),
            ctypes.c_uint8(hi_bit),
            ctypes.c_uint8(lo_bit),
        )

    def expr_concat(self, hi: int, lo: int, lo_width: int) -> int:
        return self._lib.builder_expr_concat(
            self._b,
            ctypes.c_uint32(hi),
            ctypes.c_uint32(lo),
            ctypes.c_uint8(lo_width),
        )

    def expr_array_select(self, base_var_id: int, n_elems: int,
                          result: int, index: int) -> int:
        """Build an array-select expression: result = base[index]."""
        return self._lib.builder_expr_array_select(
            self._b,
            ctypes.c_uint32(base_var_id),
            ctypes.c_uint32(n_elems),
            ctypes.c_uint32(result),
            ctypes.c_uint32(index),
        )

    def expr_sum(self, result: int, var_refs: list) -> int:
        """Build an N-ary sum expression: result == sum of var_refs[]."""
        n = len(var_refs)
        arr_t = ctypes.c_uint32 * n
        arr = arr_t(*var_refs)
        return self._lib.builder_expr_sum(
            self._b, ctypes.c_uint32(result),
            ctypes.c_uint32(n), ctypes.cast(arr, ctypes.c_void_p),
        )

    def expr_countones(self, result: int, operand: int) -> int:
        """Build a countones (popcount) expression."""
        return self._lib.builder_expr_countones(
            self._b, ctypes.c_uint32(result), ctypes.c_uint32(operand),
        )

    def expr_clog2(self, result: int, operand: int) -> int:
        """Build a clog2 expression."""
        return self._lib.builder_expr_clog2(
            self._b, ctypes.c_uint32(result), ctypes.c_uint32(operand),
        )

    def add_soft_constraint(self, root: int, priority: int = 0) -> int:
        """Add a soft (relaxable) constraint with a priority."""
        ref = self._lib.builder_add_soft_constraint(
            self._b, ctypes.c_uint32(root), ctypes.c_uint32(priority)
        )
        if ref == EXPR_NULL:
            raise RuntimeError("builder_add_soft_constraint returned EXPR_NULL")
        return ref

    def add_dist(self, var_id: int, entries) -> int:
        """Add a distribution constraint on a variable.

        Each entry is a dict with keys: lo, hi, weight, is_per_value.
        """
        from .problem import DistEntry
        arr = (DistEntry * len(entries))()
        for i, e in enumerate(entries):
            arr[i].lo = e["lo"]
            arr[i].hi = e["hi"]
            arr[i].weight = e["weight"]
            arr[i].is_per_value = 1 if e.get("is_per_value", True) else 0
        ref = self._lib.builder_add_dist(
            self._b,
            ctypes.c_uint32(var_id),
            ctypes.c_uint32(len(entries)),
            arr,
        )
        if ref == EXPR_NULL:
            raise RuntimeError("builder_add_dist returned EXPR_NULL")
        return ref
