"""Python wrapper for the C problem-handle API (``dvs_handle_*``, dv_solve.h).

A handle compiles one problem buffer once and solves it on every call, the
way a SystemVerilog ``randomize()`` call site does. Unlike
:class:`dv_solve.ctx.SolveCtx`, it solves problems with arrays
(:meth:`SolveProblemBuilder.add_array`), whose sizes are given per call.

Usage::

    b = SolveProblemBuilder()
    b.add_var(0, width=32, is_signed=True, lo=-2**31, hi=2**31 - 1)
    b.add_array(0, 32, True, VAR_STATE)
    level = 0
    eq = b.expr_binary(BIN_EQ, b.expr_var(0),
                       b.expr_elem(0, b.expr_loop_index(level)))
    b.add_constraint(b.expr_reduce(BIN_BOR, 0, level, eq))  # x inside {a}
    h = SolveHandle(b.finalize_bytes(), vars=[(32, True)], arrays=[(32, True)])
    rc, values, _ = h.randomize(seed=1, arrays=[[3, 5, 7]])
"""
from __future__ import annotations

import ctypes
from typing import Dict, List, Optional, Sequence, Tuple

from .lib import _load_lib

RANDOMIZE_OK = 0
RANDOMIZE_UNSAT = 1
RANDOMIZE_ERROR = -1
RANDOMIZE_TOO_LARGE = -2
RANDOMIZE_NULL = -3

REF_SCALAR = 0   # DVS_REF_SCALAR: one word of 1, 2, 4 or 8 bytes
REF_WORDS = 1    # DVS_REF_WORDS: ceil(width / 32) 32-bit words, low first

_MASK64 = (1 << 64) - 1


class _Ref(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_void_p), ("width", ctypes.c_uint32),
                ("is_signed", ctypes.c_uint8), ("layout", ctypes.c_uint8),
                ("_pad", ctypes.c_uint16)]


class _ArrayRef(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("n_elems", ctypes.c_uint32),
                ("_pad", ctypes.c_uint32)]


def _word(width: int) -> int:
    """Bytes per value: the smallest of 1, 2, 4 or 8 that holds *width* bits."""
    for n in (1, 2, 4, 8):
        if width <= 8 * n:
            return n
    raise ValueError("width %d is over 64 bits" % width)


def _ref_bytes(width: int, layout: int) -> int:
    """Bytes a ref of *width* bits and *layout* reads and writes."""
    if layout == REF_WORDS:
        return 4 * ((width + 31) // 32)
    return _word(width)


def _var_spec(spec) -> Tuple[int, bool, int]:
    """``(width, signed[, layout])``: the layout defaults to REF_WORDS over
    64 bits, else REF_SCALAR."""
    width, signed = spec[0], spec[1]
    layout = spec[2] if len(spec) > 2 else (REF_WORDS if width > 64 else REF_SCALAR)
    return width, signed, layout


def _to_word(value: int, nbytes: int) -> bytes:
    return (int(value) & ((1 << (8 * nbytes)) - 1)).to_bytes(nbytes, "little")


def _from_word(raw: bytes, width: int, signed: bool) -> int:
    v = int.from_bytes(raw, "little") & ((1 << width) - 1)
    if signed and v >> (width - 1):
        v -= 1 << width
    return v


class SolveHandle:
    """One compiled problem, solved per call.

    Args:
        buf: The problem buffer (:meth:`SolveProblemBuilder.finalize_bytes`).
        vars: ``(width, signed)`` of each variable id, in id order
            (``dvs_handle_n_vars`` entries; ``None`` for an id with no ref),
            or ``(width, signed, layout)`` to choose the ref's layout
            (REF_SCALAR or REF_WORDS; by default REF_WORDS over 64 bits).
        arrays: ``(elem_width, signed)`` of each array id, in id order.
    """

    def __init__(self, buf: bytes, vars: Sequence[Optional[Tuple[int, bool]]],
                 arrays: Sequence[Tuple[int, bool]] = (), lib=None) -> None:
        self._lib = lib or _load_lib()
        self._buf = ctypes.create_string_buffer(bytes(buf), len(buf))
        self._h = self._lib.dvs_handle_new(self._buf, len(buf))
        if not self._h:
            raise ValueError("dvs_handle_new refused the problem buffer")
        n = self._lib.dvs_handle_n_vars(self._h)
        self._vars = list(vars) + [None] * max(0, n - len(vars))
        self._vars = self._vars[:n]
        self._arrays = list(arrays)
        if len(self._arrays) != self._lib.dvs_handle_n_arrays(self._h):
            raise ValueError("the problem declares %d arrays, %d given"
                             % (self._lib.dvs_handle_n_arrays(self._h),
                                len(self._arrays)))

    def close(self) -> None:
        if self._h:
            self._lib.dvs_handle_free(self._h)
            self._h = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    @property
    def n_vars(self) -> int:
        return len(self._vars)

    @property
    def n_arrays(self) -> int:
        return len(self._arrays)

    def randomize(self, seed: int, state: Optional[Dict[int, int]] = None,
                  arrays: Optional[Sequence[Sequence[int]]] = None,
                  ) -> Tuple[int, Dict[int, int], List[List[int]]]:
        """Solve once. *state*: the value of each STATE variable, by id.
        *arrays*: the elements of each array, by id (a RAND array's are
        only its size). Returns ``(rc, values, elems)``: the value of every
        variable with a ref, and each array's elements, as of the call
        (solved ones on ``RANDOMIZE_OK``)."""
        state = state or {}
        refs = (_Ref * max(1, len(self._vars)))()
        cells = []
        for vid, spec in enumerate(self._vars):
            if spec is None:
                continue
            width, signed, layout = _var_spec(spec)
            nb = _ref_bytes(width, layout)
            cell = ctypes.create_string_buffer(_to_word(state.get(vid, 0), nb), nb)
            cells.append((vid, cell, width, signed))
            refs[vid].addr = ctypes.cast(cell, ctypes.c_void_p)
            refs[vid].width = width
            refs[vid].is_signed = 1 if signed else 0
            refs[vid].layout = layout
        seed &= _MASK64
        if not self._arrays:
            rc = self._lib.dvs_randomize(self._h, refs, None, seed)
            datas = []
        else:
            given = list(arrays or [])
            given += [[]] * (len(self._arrays) - len(given))
            arefs = (_ArrayRef * len(self._arrays))()
            datas = []
            for aid, ((width, signed), elems) in enumerate(zip(self._arrays, given)):
                nb = _ref_bytes(width, REF_WORDS if width > 64 else REF_SCALAR)
                raw = b"".join(_to_word(v, nb) for v in elems)
                data = ctypes.create_string_buffer(raw, max(1, len(raw)))
                datas.append((data, len(elems), nb, width, signed))
                arefs[aid].data = ctypes.cast(data, ctypes.c_void_p) if elems else None
                arefs[aid].n_elems = len(elems)
            rc = self._lib.dvs_randomize_arrays(self._h, refs, arefs, None, seed)
        values = {vid: _from_word(cell.raw, width, signed)
                  for vid, cell, width, signed in cells}
        elems_out = [[_from_word(data.raw[i * nb:(i + 1) * nb], width, signed)
                      for i in range(n)]
                     for data, n, nb, width, signed in datas]
        return rc, values, elems_out
