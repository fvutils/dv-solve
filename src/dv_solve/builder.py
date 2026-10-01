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
        """Create an empty problem builder.

        Args:
            block_size: Size of the internal allocation blocks, in bytes.
            lib: Native library handle. Leave as ``None`` to load it
                automatically.

        Raises:
            RuntimeError: The native library could not be found.
        """
        if lib is None:
            lib = _load_lib()
        if lib is None:
            raise RuntimeError(
                "libdv_solve.so not found -- native solver unavailable"
            )
        self._lib = lib
        self._b = lib.dvs_builder_create(block_size, None)
        if self._b is None:
            raise RuntimeError("dvs_builder_create returned NULL")
        self._finalized_bufs: list = []  # prevent GC of finalized buffers

    def reset(self) -> None:
        """Discard everything added so far and start a new problem."""
        self._lib.dvs_builder_reset(self._b)

    def destroy(self) -> None:
        """Free the builder's native memory.

        Called automatically when the builder is garbage-collected.
        """
        if self._b is not None:
            self._lib.dvs_builder_destroy(self._b)
            self._b = None

    def __del__(self) -> None:
        if hasattr(self, "_b"):
            self.destroy()

    @property
    def virtual_used(self) -> int:
        """Bytes allocated in the virtual address space so far."""
        return self._lib.dvs_builder_virtual_used(self._b)

    # ------------------------------------------------------------------ #
    # Finalize                                                             #
    # ------------------------------------------------------------------ #

    def finalize(self) -> Tuple[ctypes.Array, int]:
        """Pack the problem into a buffer that :class:`~dv_solve.ctx.SolveCtx` compiles.

        The builder is unchanged, so it can be finalized again after more
        variables or constraints are added.

        Returns:
            ``(buffer, size)``: the problem buffer and its size in bytes.
        """
        size = ctypes.c_size_t(0)
        sp_ptr = self._lib.dvs_builder_finalize(self._b, ctypes.byref(size))
        if sp_ptr is None or sp_ptr == 0:
            raise RuntimeError("dvs_builder_finalize returned NULL")

        sz = size.value
        # Copy the C-allocated buffer into a ctypes-managed array so Python
        # owns the memory and it stays alive as long as needed.
        buf = (ctypes.c_uint8 * sz)()
        ctypes.memmove(buf, sp_ptr, sz)
        # Free the C-allocated buffer
        self._lib.dvs_builder_free_problem(self._b, sp_ptr, sz)
        return buf, sz

    def finalize_bytes(self) -> bytes:
        """Like :meth:`finalize`, but return the problem as ``bytes``."""
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
        """Declare a variable.

        Args:
            var_id: The variable's id, which expressions and
                :meth:`SolveCtx.get_value` use to refer to it. A problem with
                n variables uses the ids 0 to n-1, each declared once.
            width: Width in bits, 1 to 64.
            is_signed: Whether the variable holds signed values.
            lo: Smallest value the variable may take.
            hi: Largest value the variable may take.

        Returns:
            A reference to the declaration (rarely needed; use
            :meth:`expr_var` to refer to the variable in expressions).
        """
        ref = self._lib.dvs_builder_add_var(
            self._b,
            ctypes.c_uint32(var_id),
            ctypes.c_uint8(width),
            ctypes.c_uint8(1 if is_signed else 0),
            ctypes.c_int64(lo),
            ctypes.c_int64(hi),
        )
        if ref == EXPR_NULL:
            raise RuntimeError("dvs_builder_add_var returned EXPR_NULL (malloc failure)")
        return ref

    def add_constraint(self, root: int) -> int:
        """Require an expression to hold.

        Args:
            root: A Boolean-valued expression, such as a comparison.
        """
        ref = self._lib.dvs_builder_add_constraint(
            self._b, ctypes.c_uint32(root)
        )
        if ref == EXPR_NULL:
            raise RuntimeError("dvs_builder_add_constraint returned EXPR_NULL")
        return ref

    def add_source(self, var_ids: Sequence[int]) -> int:
        """Add a source group."""
        arr = (ctypes.c_uint32 * len(var_ids))(*var_ids)
        ref = self._lib.dvs_builder_add_source(
            self._b, ctypes.c_uint32(len(var_ids)), arr
        )
        if ref == EXPR_NULL:
            raise RuntimeError("dvs_builder_add_source returned EXPR_NULL")
        return ref


    def add_all_different(self, var_ids: Sequence[int]) -> int:
        """Require the given variables to take pairwise different values.

        Args:
            var_ids: Variable ids (not expressions).
        """
        arr = (ctypes.c_uint32 * len(var_ids))(*var_ids)
        ref = self._lib.dvs_builder_add_all_different(
            self._b, ctypes.c_uint32(len(var_ids)), arr
        )
        if ref == EXPR_NULL:
            raise RuntimeError("dvs_builder_add_all_different returned EXPR_NULL")
        return ref
    # ------------------------------------------------------------------ #
    # Expression builders                                                  #
    # ------------------------------------------------------------------ #

    def expr_const(self, value: int, is_signed: bool = False,
                   width: int = 0) -> int:
        """A constant.

        Args:
            value: The value. Must fit in a signed 64-bit integer.
            is_signed: Set for a negative constant.
        """
        if width:
            return self._lib.dvs_builder_expr_const_sized(
                self._b, ctypes.c_int64(value),
                ctypes.c_uint8(1 if is_signed else 0), ctypes.c_uint8(width),
            )
        return self._lib.dvs_builder_expr_const(
            self._b, ctypes.c_int64(value),
            ctypes.c_uint8(1 if is_signed else 0),
        )

    def expr_var(self, var_id: int) -> int:
        """The value of variable ``var_id``."""
        return self._lib.dvs_builder_expr_var(
            self._b, ctypes.c_uint32(var_id)
        )

    def expr_binary(self, op: int, lhs: int, rhs: int) -> int:
        """A binary operation.

        Args:
            op: One of the ``BIN_*`` constants in :mod:`dv_solve.problem`.
            lhs: Left operand expression.
            rhs: Right operand expression.
        """
        return self._lib.dvs_builder_expr_binary(
            self._b,
            ctypes.c_uint32(op),
            ctypes.c_uint32(lhs),
            ctypes.c_uint32(rhs),
        )

    def expr_unary(self, op: int, operand: int) -> int:
        """A unary operation.

        Args:
            op: One of the ``UN_*`` constants in :mod:`dv_solve.problem`.
            operand: Operand expression.
        """
        return self._lib.dvs_builder_expr_unary(
            self._b, ctypes.c_uint32(op), ctypes.c_uint32(operand)
        )

    def expr_ite(self, cond: int, then_e: int, else_e: int) -> int:
        """If-then-else: ``then_e`` when ``cond`` holds, otherwise ``else_e``.

        With Boolean ``then_e`` and ``else_e`` this expresses an implication
        with an alternative, such as SystemVerilog's ``if (...) ... else ...``
        inside a constraint.
        """
        return self._lib.dvs_builder_expr_ite(
            self._b,
            ctypes.c_uint32(cond),
            ctypes.c_uint32(then_e),
            ctypes.c_uint32(else_e),
        )

    def expr_in_range(self, value: int, lo: int, hi: int) -> int:
        """True when ``lo <= value <= hi``.

        Args:
            value: Expression to test.
            lo: Lower bound expression (inclusive).
            hi: Upper bound expression (inclusive).
        """
        return self._lib.dvs_builder_expr_in_range(
            self._b,
            ctypes.c_uint32(value),
            ctypes.c_uint32(lo),
            ctypes.c_uint32(hi),
        )

    def expr_in_set(self, value: int, elems: Sequence[int]) -> int:
        """True when ``value`` equals one of ``elems``.

        Args:
            value: Expression to test.
            elems: Candidate expressions, typically constants.
        """
        arr = (ctypes.c_uint32 * len(elems))(*elems)
        return self._lib.dvs_builder_expr_in_set(
            self._b,
            ctypes.c_uint32(value),
            ctypes.c_uint32(len(elems)),
            arr,
        )

    def expr_in_ranges(self, value: int, ranges) -> int:
        """True when ``value`` lies in any of several inclusive ranges.

        Args:
            value: Expression to test.
            ranges: Sequence of ``(lo, hi)`` pairs of expressions.
        """
        n = len(ranges)
        los = (ctypes.c_uint32 * n)(*[r[0] for r in ranges])
        his = (ctypes.c_uint32 * n)(*[r[1] for r in ranges])
        return self._lib.dvs_builder_expr_in_ranges(
            self._b,
            ctypes.c_uint32(value),
            ctypes.c_uint32(n),
            los,
            his,
        )

    def expr_extend(
        self,
        operand: int,
        from_bits: int,
        to_bits: int,
        sign_extend: bool = False,
    ) -> int:
        """Widen ``operand`` from ``from_bits`` to ``to_bits`` bits.

        Args:
            operand: Expression to widen.
            from_bits: Width of ``operand``.
            to_bits: Width of the result.
            sign_extend: Replicate the sign bit instead of filling with zeros.
        """
        return self._lib.dvs_builder_expr_extend(
            self._b,
            ctypes.c_uint32(operand),
            ctypes.c_uint8(from_bits),
            ctypes.c_uint8(to_bits),
            ctypes.c_uint8(1 if sign_extend else 0),
        )

    def expr_extract(self, operand: int, hi_bit: int, lo_bit: int) -> int:
        """Bits ``hi_bit`` down to ``lo_bit`` of ``operand`` (inclusive)."""
        return self._lib.dvs_builder_expr_extract(
            self._b,
            ctypes.c_uint32(operand),
            ctypes.c_uint8(hi_bit),
            ctypes.c_uint8(lo_bit),
        )

    def expr_concat(self, hi: int, lo: int, lo_width: int) -> int:
        """Concatenation: ``hi`` in the upper bits, ``lo`` in the lower ``lo_width`` bits."""
        return self._lib.dvs_builder_expr_concat(
            self._b,
            ctypes.c_uint32(hi),
            ctypes.c_uint32(lo),
            ctypes.c_uint8(lo_width),
        )

    def expr_array_select(self, base_var_id: int, n_elems: int,
                          result: int, index: int) -> int:
        """Build an array-select expression: result = base[index]."""
        return self._lib.dvs_builder_expr_array_select(
            self._b,
            ctypes.c_uint32(base_var_id),
            ctypes.c_uint32(n_elems),
            ctypes.c_uint32(result),
            ctypes.c_uint32(index),
        )

    def expr_sum(self, result: int, var_refs: list) -> int:
        """Constraint: ``result`` equals the sum of ``var_refs``.

        Pass the returned expression to :meth:`add_constraint`.

        Args:
            result: Expression for the total, typically a variable.
            var_refs: Expressions to add up.
        """
        n = len(var_refs)
        arr_t = ctypes.c_uint32 * n
        arr = arr_t(*var_refs)
        return self._lib.dvs_builder_expr_sum(
            self._b, ctypes.c_uint32(result),
            ctypes.c_uint32(n), ctypes.cast(arr, ctypes.c_void_p),
        )

    def expr_countones(self, result: int, operand: int) -> int:
        """Constraint: ``result`` equals the number of 1 bits in ``operand``.

        Pass the returned expression to :meth:`add_constraint`.
        """
        return self._lib.dvs_builder_expr_countones(
            self._b, ctypes.c_uint32(result), ctypes.c_uint32(operand),
        )

    def expr_clog2(self, result: int, operand: int) -> int:
        """Constraint: ``result`` equals the ceiling of log2 of ``operand``.

        Pass the returned expression to :meth:`add_constraint`.
        """
        return self._lib.dvs_builder_expr_clog2(
            self._b, ctypes.c_uint32(result), ctypes.c_uint32(operand),
        )

    def add_soft_constraint(self, root: int, priority: int = 0) -> int:
        """Add a constraint that may be dropped if it conflicts with others.

        Hard constraints always hold. Soft constraints are kept when they can
        be; when they conflict, the ones with the highest ``priority`` number
        are dropped first.

        Args:
            root: A Boolean-valued expression.
            priority: 0 is the most important.
        """
        ref = self._lib.dvs_builder_add_soft_constraint(
            self._b, ctypes.c_uint32(root), ctypes.c_uint32(priority)
        )
        if ref == EXPR_NULL:
            raise RuntimeError("dvs_builder_add_soft_constraint returned EXPR_NULL")
        return ref

    def add_dist(self, var_id: int, entries) -> int:
        """Give a variable a weighted distribution, like SystemVerilog's ``dist``.

        Args:
            var_id: The variable.
            entries: Sequence of dicts with keys ``lo``, ``hi`` and
                ``weight``, and optionally ``is_per_value``. The range
                ``lo``..``hi`` is chosen in proportion to ``weight``. With
                ``is_per_value`` true (the default, SystemVerilog ``:=``) every
                value in the range has that weight; false (``:/``) spreads the
                weight across the range.
        """
        from .problem import DistEntry
        arr = (DistEntry * len(entries))()
        for i, e in enumerate(entries):
            arr[i].lo = e["lo"]
            arr[i].hi = e["hi"]
            arr[i].weight = e["weight"]
            arr[i].is_per_value = 1 if e.get("is_per_value", True) else 0
        ref = self._lib.dvs_builder_add_dist(
            self._b,
            ctypes.c_uint32(var_id),
            ctypes.c_uint32(len(entries)),
            arr,
        )
        if ref == EXPR_NULL:
            raise RuntimeError("dvs_builder_add_dist returned EXPR_NULL")
        return ref
