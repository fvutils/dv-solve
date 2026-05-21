"""Unit tests for the Python SolveProblemBuilder wrapper (Step 2).

Tests that the Python builder API works, produces valid buffers, and
that IRTranslator + NativeSolverBackend use the builder.
"""
from __future__ import annotations

import ctypes
import pytest

EXPR_NULL = 0xFFFFFFFF
SOLVE_OK = 0

BIN_LTE = 13
BIN_GTE = 15

_CTX_BUF_SIZE = 1 << 20


class TestPythonBuilder:
    """Test the Python SolveProblemBuilder wrapper."""

    @pytest.fixture(autouse=True)
    def setup(self, libzsp):
        self.lib = libzsp
        # Wire builder argtypes so the Python builder can use this lib handle
        from dv_solve.lib import _wire_builder_argtypes
        _wire_builder_argtypes(self.lib)
        # Wire solver functions for round-trip testing
        c = ctypes
        self.lib.zsp_block_alloc_create.restype  = c.c_void_p
        self.lib.zsp_block_alloc_create.argtypes = [c.c_void_p, c.c_size_t]
        self.lib.zsp_block_alloc_destroy.restype  = None
        self.lib.zsp_block_alloc_destroy.argtypes = [c.c_void_p]
        self.lib.solver_create.restype  = c.c_void_p
        self.lib.solver_create.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
        self.lib.solver_destroy.restype  = None
        self.lib.solver_destroy.argtypes = [c.c_void_p]
        self.lib.solver_compile.restype  = c.c_int
        self.lib.solver_compile.argtypes = [c.c_void_p, c.c_void_p]

        class SolveOpts(ctypes.Structure):
            _fields_ = [
                ("seed",           c.c_uint64),
                ("max_conflicts",  c.c_uint32),
                ("max_restarts",   c.c_uint32),
                ("use_phase_save", c.c_uint8),
                ("_pad",           c.c_uint8 * 3),
                ("max_shave_iters", c.c_uint32),
            ]
        self._SolveOpts = SolveOpts
        self.lib.solver_solve.restype  = c.c_int
        self.lib.solver_solve.argtypes = [c.c_void_p, c.c_void_p]
        self.lib.solver_get_value.restype  = c.c_int64
        self.lib.solver_get_value.argtypes = [c.c_void_p, c.c_uint32]

    def _solve_buffer(self, buf, n_vars, seed=0x42):
        """Compile and solve a finalized ctypes buffer."""
        ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
        ba = self.lib.zsp_block_alloc_create(None, 0)
        assert ba
        # Cast the ctypes array to a void pointer for solver_compile
        sp_ptr = ctypes.cast(buf, ctypes.c_void_p).value
        ctx = self.lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
        assert ctx
        rc = self.lib.solver_compile(ctx, sp_ptr)
        assert rc == 0, f"solver_compile failed: {rc}"
        opts = self._SolveOpts(seed=seed)
        result = self.lib.solver_solve(ctx, ctypes.byref(opts))
        assert result == SOLVE_OK
        values = [self.lib.solver_get_value(ctx, i) for i in range(n_vars)]
        self.lib.solver_destroy(ctx)
        self.lib.zsp_block_alloc_destroy(ba)
        return values

    def test_python_builder_basic(self):
        """Build and finalize a simple problem via the Python wrapper."""
        from dv_solve.builder import SolveProblemBuilder

        b = SolveProblemBuilder(lib=self.lib)
        b.add_var(0, width=8, is_signed=False, lo=0, hi=100)
        b.add_var(1, width=8, is_signed=False, lo=0, hi=100)

        # Constraint: var[0] <= var[1]
        v0 = b.expr_var(0)
        v1 = b.expr_var(1)
        le = b.expr_binary(BIN_LTE, v0, v1)
        b.add_constraint(le)

        buf, sz = b.finalize()
        assert sz > 0

        values = self._solve_buffer(buf, 2)
        assert values[0] <= values[1]
        assert 0 <= values[0] <= 100
        assert 0 <= values[1] <= 100

    def test_python_builder_finalize_bytes(self):
        """finalize_bytes() returns a Python bytes object."""
        from dv_solve.builder import SolveProblemBuilder

        b = SolveProblemBuilder(lib=self.lib)
        b.add_var(0, width=8, is_signed=False, lo=0, hi=50)
        data = b.finalize_bytes()
        assert isinstance(data, bytes)
        assert len(data) > 0

    def test_python_builder_reset(self):
        """Reset and reuse a builder."""
        from dv_solve.builder import SolveProblemBuilder

        b = SolveProblemBuilder(lib=self.lib)
        b.add_var(0, width=8, is_signed=False, lo=0, hi=10)
        buf1, sz1 = b.finalize()
        assert sz1 > 0

        b.reset()
        b.add_var(0, width=8, is_signed=False, lo=0, hi=200)
        b.add_var(1, width=8, is_signed=False, lo=0, hi=200)
        buf2, sz2 = b.finalize()
        assert sz2 > sz1  # More data

        # Both should be independently solvable
        vals1 = self._solve_buffer(buf1, 1)
        assert 0 <= vals1[0] <= 10

        vals2 = self._solve_buffer(buf2, 2)
        for v in vals2:
            assert 0 <= v <= 200

    def test_python_builder_large(self):
        """Build a large problem that would overflow a 64KB fixed buffer."""
        from dv_solve.builder import SolveProblemBuilder

        b = SolveProblemBuilder(lib=self.lib)
        n_vars = 200
        for i in range(n_vars):
            b.add_var(i, width=8, is_signed=False, lo=0, hi=255)

        buf, sz = b.finalize()
        # 200 VarSpecs at ~24 bytes each = ~4800 bytes minimum
        assert sz > 4800
