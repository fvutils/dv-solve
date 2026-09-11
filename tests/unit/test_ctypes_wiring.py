"""Regression: ``_wire_argtypes`` must declare ctypes ``argtypes`` for every C
entry point the Python builder wrapper calls.

An *unwired* function (``argtypes is None``) makes ctypes coerce the 64-bit
``SolveProblemBuilder *`` as a C ``int``, **truncating the pointer to 32 bits**.
It is harmless only while the builder sits at a low heap address; at a high
address — which AddressSanitizer's allocator and CI both produce — the truncated
pointer is wild and ``builder_alloc``'s first field read segfaults. That is
exactly how an unwired ``builder_expr_concat`` crashed downstream CI on the
>64-bit-constant path.

Hermetic: it feeds ``_wire_argtypes`` a stand-in CDLL and inspects what got
wired, so it needs no native build / C toolchain and runs anywhere.
"""
from __future__ import annotations

import ctypes
import re
from pathlib import Path

from dv_solve import builder as _builder_mod
from dv_solve import lib as _lib_mod


class _FakeFunc:
    """A settable stand-in for a ctypes ``_FuncPtr`` (default-unwired)."""

    def __init__(self) -> None:
        self.restype = None
        self.argtypes = None


class _FakeLib:
    """Stand-in for ``ctypes.CDLL``: lazily materialises one cached ``_FakeFunc``
    per attribute — like CDLL's function-pointer caching — but with no library."""

    def __init__(self) -> None:
        object.__setattr__(self, "_funcs", {})

    def __getattr__(self, name: str) -> _FakeFunc:
        funcs = object.__getattribute__(self, "_funcs")
        if name not in funcs:
            funcs[name] = _FakeFunc()
        return funcs[name]


def _called_c_functions() -> list[str]:
    """Every C entry invoked as ``self._lib.<name>(`` in the builder wrapper —
    the exact ABI surface ``_wire_argtypes`` must cover."""
    src = Path(_builder_mod.__file__).read_text()
    return sorted(set(re.findall(r"self\._lib\.(\w+)\s*\(", src)))


def _wired_fake() -> _FakeLib:
    lib = _FakeLib()
    _lib_mod._wire_argtypes(lib)
    return lib


def test_wire_argtypes_covers_all_builder_calls():
    lib = _wired_fake()
    unwired = [n for n in _called_c_functions()
               if getattr(lib, n).argtypes is None]
    assert unwired == [], (
        "_wire_argtypes leaves these builder-wrapper calls unwired — ctypes will "
        "truncate the 64-bit builder pointer and crash under a high-address heap "
        "(ASAN / CI): %s" % unwired)


def test_builder_pointer_first_arg_is_void_p():
    lib = _wired_fake()
    bad = []
    for n in _called_c_functions():
        if not n.startswith("builder_"):
            continue
        argtypes = getattr(lib, n).argtypes
        if not argtypes or argtypes[0] is not ctypes.c_void_p:
            bad.append(n)
    assert bad == [], (
        "builder_* functions whose first (pointer) arg is not c_void_p "
        "(pointer-truncation risk): %s" % bad)


def test_concat_wired_canary():
    lib = _wired_fake()
    argtypes = lib.builder_expr_concat.argtypes
    assert argtypes is not None, "builder_expr_concat lost its ctypes wiring"
    assert argtypes[0] is ctypes.c_void_p
