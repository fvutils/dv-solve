"""Unit tests for SolveProblemBuilder (Step 1).

Tests:
1. Basic build + finalize: 2-var problem compiles and solves.
2. Multi-block: small block_size forces multiple blocks; result is valid.
3. Alignment: ExprRef values match fixed-buffer API for same sequence.
4. Reset + reuse: build, finalize, reset, build different problem.
5. Large problem: 100 vars, 50 constraints, no overflow.
"""
from __future__ import annotations

import ctypes
import pytest

# ------------------------------------------------------------------ #
# Constants                                                            #
# ------------------------------------------------------------------ #

EXPR_NULL = 0xFFFFFFFF

SOLVE_OK      = 0
SOLVE_UNSAT   = 1
SOLVE_TIMEOUT = 2

# BinOp
BIN_ADD = 0
BIN_EQ  = 10
BIN_LT  = 12
BIN_LTE = 13
BIN_GTE = 15
BIN_AND = 16

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 1048576


# ------------------------------------------------------------------ #
# ctypes struct mirrors                                                #
# ------------------------------------------------------------------ #

class SolveProblemHead(ctypes.Structure):
    _fields_ = [
        ("n_vars",           ctypes.c_uint32),
        ("n_constraints",    ctypes.c_uint32),
        ("n_sources",        ctypes.c_uint32),
        ("vars_head",        ctypes.c_uint32),
        ("constraints_head", ctypes.c_uint32),
        ("sources_head",     ctypes.c_uint32),
        ("_pad0",            ctypes.c_uint32),
        ("_pad1",            ctypes.c_uint32),
    ]


class SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",           ctypes.c_uint64),
        ("max_conflicts",  ctypes.c_uint32),
        ("max_restarts",   ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad",           ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


# ------------------------------------------------------------------ #
# Library wiring                                                       #
# ------------------------------------------------------------------ #

def _setup(lib: ctypes.CDLL):
    """Wire argtypes/restype for builder + solver functions."""
    # Builder lifecycle
    lib.builder_create.restype  = ctypes.c_void_p
    lib.builder_create.argtypes = [ctypes.c_uint32, ctypes.c_void_p]

    lib.builder_reset.restype  = None
    lib.builder_reset.argtypes = [ctypes.c_void_p]

    lib.builder_destroy.restype  = None
    lib.builder_destroy.argtypes = [ctypes.c_void_p]

    lib.builder_virtual_used.restype  = ctypes.c_uint32
    lib.builder_virtual_used.argtypes = [ctypes.c_void_p]

    # Builder finalize
    lib.builder_finalize.restype  = ctypes.c_void_p
    lib.builder_finalize.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]

    lib.builder_free_problem.restype  = None
    lib.builder_free_problem.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]

    # Builder allocation
    lib.builder_alloc.restype  = ctypes.c_uint32
    lib.builder_alloc.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]

    # Builder expression builders
    lib.builder_expr_const.restype  = ctypes.c_uint32
    lib.builder_expr_const.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8]

    lib.builder_expr_var.restype  = ctypes.c_uint32
    lib.builder_expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.builder_expr_binary.restype  = ctypes.c_uint32
    lib.builder_expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                        ctypes.c_uint32, ctypes.c_uint32]

    lib.builder_expr_unary.restype  = ctypes.c_uint32
    lib.builder_expr_unary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.c_uint32]

    lib.builder_expr_ite.restype  = ctypes.c_uint32
    lib.builder_expr_ite.argtypes = [ctypes.c_void_p,
                                     ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]

    lib.builder_expr_in_range.restype  = ctypes.c_uint32
    lib.builder_expr_in_range.argtypes = [ctypes.c_void_p,
                                          ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]

    lib.builder_expr_in_set.restype  = ctypes.c_uint32
    lib.builder_expr_in_set.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                        ctypes.c_uint32, ctypes.c_void_p]

    lib.builder_expr_extend.restype  = ctypes.c_uint32
    lib.builder_expr_extend.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                        ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]

    lib.builder_expr_extract.restype  = ctypes.c_uint32
    lib.builder_expr_extract.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.c_uint8, ctypes.c_uint8]

    # Builder problem builders
    lib.builder_add_var.restype  = ctypes.c_uint32
    lib.builder_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]

    lib.builder_add_constraint.restype  = ctypes.c_uint32
    lib.builder_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.builder_add_source.restype  = ctypes.c_uint32
    lib.builder_add_source.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]

    # Existing solver/problem API (for comparison and round-trip tests)
    lib.solve_problem_init.restype  = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    lib.problem_add_var.restype  = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]

    lib.expr_const.restype  = ctypes.c_uint32
    lib.expr_const.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8]

    lib.expr_var.restype  = ctypes.c_uint32
    lib.expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.expr_binary.restype  = ctypes.c_uint32
    lib.expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint32]

    lib.problem_add_constraint.restype  = ctypes.c_uint32
    lib.problem_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.zsp_block_alloc_create.restype  = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [ctypes.c_void_p]
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.solver_solve.restype  = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.solver_get_value.restype  = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

def _compile_and_solve(lib, sp_ptr, n_vars, seed=0x1234):
    """Compile a SolveProblem and solve it. Returns list of values."""
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ba = lib.zsp_block_alloc_create(None, 0)
    assert ba
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx

    rc = lib.solver_compile(ctx, sp_ptr)
    assert rc == 0, f"solver_compile failed: {rc}"

    opts = SolveOpts(seed=seed, max_conflicts=0, max_restarts=0,
                     use_phase_save=0, max_shave_iters=0)
    result = lib.solver_solve(ctx, ctypes.byref(opts))
    assert result == SOLVE_OK, f"solver_solve returned {result}"

    values = []
    for i in range(n_vars):
        values.append(lib.solver_get_value(ctx, i))

    lib.solver_destroy(ctx)
    lib.zsp_block_alloc_destroy(ba)
    return values


# ------------------------------------------------------------------ #
# Test class                                                           #
# ------------------------------------------------------------------ #

class TestBuilder:
    @pytest.fixture(autouse=True)
    def setup(self, libzsp):
        self.lib = libzsp
        _setup(self.lib)

    def _builder(self, block_size=0):
        b = self.lib.builder_create(block_size, None)
        assert b, "builder_create returned NULL"
        return b

    # -- basic build + finalize ------------------------------------ #

    def test_basic_build_finalize(self):
        """Build a 2-var problem with var-op-const constraints, finalize, compile, solve."""
        b = self._builder()

        # Variables: a=[0,255], b=[0,255]
        self.lib.builder_add_var(b, 0, 8, 0, 0, 255)
        self.lib.builder_add_var(b, 1, 8, 0, 0, 255)

        # Constraints: a <= b, a >= 3, b <= 50
        v0 = self.lib.builder_expr_var(b, 0)
        v1 = self.lib.builder_expr_var(b, 1)
        le_expr = self.lib.builder_expr_binary(b, BIN_LTE, v0, v1)
        self.lib.builder_add_constraint(b, le_expr)

        c3 = self.lib.builder_expr_const(b, 3, 0)
        ge_expr = self.lib.builder_expr_binary(b, BIN_GTE, v0, c3)
        self.lib.builder_add_constraint(b, ge_expr)

        c50 = self.lib.builder_expr_const(b, 50, 0)
        le2_expr = self.lib.builder_expr_binary(b, BIN_LTE, v1, c50)
        self.lib.builder_add_constraint(b, le2_expr)

        # Finalize
        size = ctypes.c_size_t(0)
        sp = self.lib.builder_finalize(b, ctypes.byref(size))
        assert sp, 'builder_finalize returned NULL'
        assert size.value > 0

        # Verify header
        head = SolveProblemHead.from_address(sp)
        assert head.n_vars == 2
        assert head.n_constraints == 3

        # Compile + solve
        values = _compile_and_solve(self.lib, sp, 2)
        assert values[0] <= values[1]
        assert 3 <= values[0] <= 255
        assert 0 <= values[1] <= 50

        self.lib.builder_free_problem(b, sp, size.value)
        self.lib.builder_destroy(b)


    # -- multi-block ----------------------------------------------- #

    def test_multi_block(self):
        """Force multiple blocks with small block_size, verify result."""
        b = self._builder(block_size=128)

        # Build enough data to span 3+ blocks
        n_vars = 10
        for i in range(n_vars):
            ref = self.lib.builder_add_var(b, i, 8, 0, 0, 100)
            assert ref != EXPR_NULL

        # Constraint: var[0] <= var[1] (var-op-var, supported by compiler)
        v0 = self.lib.builder_expr_var(b, 0)
        v1 = self.lib.builder_expr_var(b, 1)
        le_expr = self.lib.builder_expr_binary(b, BIN_LTE, v0, v1)
        self.lib.builder_add_constraint(b, le_expr)

        # Finalize + solve
        size = ctypes.c_size_t(0)
        sp = self.lib.builder_finalize(b, ctypes.byref(size))
        assert sp

        head = SolveProblemHead.from_address(sp)
        assert head.n_vars == n_vars
        assert head.n_constraints == 1

        values = _compile_and_solve(self.lib, sp, n_vars)
        assert values[0] <= values[1]

        self.lib.builder_free_problem(b, sp, size.value)
        self.lib.builder_destroy(b)


    # -- alignment ------------------------------------------------- #

    def test_alignment_matches_fixed_buffer(self):
        """ExprRef values from builder match those from the fixed-buffer API
        for the same allocation sequence."""
        # Build via builder
        b = self._builder()
        b_refs = []
        b_refs.append(self.lib.builder_add_var(b, 0, 8, 0, 0, 255))
        b_refs.append(self.lib.builder_add_var(b, 1, 16, 0, 0, 65535))
        b_refs.append(self.lib.builder_expr_var(b, 0))
        b_refs.append(self.lib.builder_expr_var(b, 1))
        b_refs.append(self.lib.builder_expr_binary(b, BIN_ADD, b_refs[2], b_refs[3]))
        b_refs.append(self.lib.builder_expr_const(b, 42, 0))
        b_refs.append(self.lib.builder_expr_binary(b, BIN_EQ, b_refs[4], b_refs[5]))
        b_refs.append(self.lib.builder_add_constraint(b, b_refs[6]))

        # Build via fixed-buffer API
        sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
        sp = self.lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
        assert sp
        f_refs = []
        f_refs.append(self.lib.problem_add_var(sp, 0, 8, 0, 0, 255))
        f_refs.append(self.lib.problem_add_var(sp, 1, 16, 0, 0, 65535))
        f_refs.append(self.lib.expr_var(sp, 0))
        f_refs.append(self.lib.expr_var(sp, 1))
        f_refs.append(self.lib.expr_binary(sp, BIN_ADD, f_refs[2], f_refs[3]))
        f_refs.append(self.lib.expr_const(sp, 42, 0))
        f_refs.append(self.lib.expr_binary(sp, BIN_EQ, f_refs[4], f_refs[5]))
        f_refs.append(self.lib.problem_add_constraint(sp, f_refs[6]))

        # All ExprRef values must match
        for i, (br, fr) in enumerate(zip(b_refs, f_refs)):
            assert br == fr, (
                f"ExprRef mismatch at step {i}: builder={br:#x}, fixed={fr:#x}"
            )

        self.lib.builder_destroy(b)

    # -- reset + reuse --------------------------------------------- #

    def test_reset_reuse(self):
        """Build, finalize, reset, build different problem, finalize again."""
        b = self._builder()

        # First problem: 1 var
        self.lib.builder_add_var(b, 0, 8, 0, 0, 100)
        size1 = ctypes.c_size_t(0)
        sp1 = self.lib.builder_finalize(b, ctypes.byref(size1))
        assert sp1
        head1 = SolveProblemHead.from_address(sp1)
        assert head1.n_vars == 1

        # Reset
        self.lib.builder_reset(b)

        # Second problem: 3 vars
        self.lib.builder_add_var(b, 0, 8, 0, 0, 50)
        self.lib.builder_add_var(b, 1, 8, 0, 0, 50)
        self.lib.builder_add_var(b, 2, 8, 0, 0, 50)
        size2 = ctypes.c_size_t(0)
        sp2 = self.lib.builder_finalize(b, ctypes.byref(size2))
        assert sp2
        head2 = SolveProblemHead.from_address(sp2)
        assert head2.n_vars == 3

        # Both should compile and solve independently
        vals1 = _compile_and_solve(self.lib, sp1, 1)
        assert 0 <= vals1[0] <= 100

        vals2 = _compile_and_solve(self.lib, sp2, 3)
        for v in vals2:
            assert 0 <= v <= 50

        self.lib.builder_free_problem(b, sp1, size1.value)
        self.lib.builder_free_problem(b, sp2, size2.value)
        self.lib.builder_destroy(b)

    # -- large problem --------------------------------------------- #

    def test_large_problem(self):
        """100 vars, 50 constraints. No overflow possible with builder."""
        b = self._builder()

        n_vars = 100
        n_constraints = 50

        for i in range(n_vars):
            ref = self.lib.builder_add_var(b, i, 8, 0, 0, 200)
            assert ref != EXPR_NULL

        # Each constraint: var[i] <= var[i+1]
        for i in range(n_constraints):
            vi = self.lib.builder_expr_var(b, i)
            vj = self.lib.builder_expr_var(b, i + 1)
            le = self.lib.builder_expr_binary(b, BIN_LTE, vi, vj)
            ref = self.lib.builder_add_constraint(b, le)
            assert ref != EXPR_NULL

        size = ctypes.c_size_t(0)
        sp = self.lib.builder_finalize(b, ctypes.byref(size))
        assert sp

        head = SolveProblemHead.from_address(sp)
        assert head.n_vars == n_vars
        assert head.n_constraints == n_constraints

        values = _compile_and_solve(self.lib, sp, n_vars)
        # Verify ordering: var[i] <= var[i+1]
        for i in range(n_constraints):
            assert values[i] <= values[i + 1], (
                f"Constraint violated: var[{i}]={values[i]} > var[{i+1}]={values[i+1]}"
            )

        self.lib.builder_free_problem(b, sp, size.value)
        self.lib.builder_destroy(b)

    # -- large problem multi-block --------------------------------- #

    def test_large_problem_small_blocks(self):
        """100 vars with block_size=64, forcing many blocks."""
        b = self._builder(block_size=64)

        n_vars = 100
        for i in range(n_vars):
            ref = self.lib.builder_add_var(b, i, 8, 0, 0, 255)
            assert ref != EXPR_NULL

        # Constraint: sum is unconstrained, just verify roundtrip
        size = ctypes.c_size_t(0)
        sp = self.lib.builder_finalize(b, ctypes.byref(size))
        assert sp

        head = SolveProblemHead.from_address(sp)
        assert head.n_vars == n_vars

        values = _compile_and_solve(self.lib, sp, n_vars)
        for v in values:
            assert 0 <= v <= 255

        self.lib.builder_free_problem(b, sp, size.value)
        self.lib.builder_destroy(b)

    # -- source spec ----------------------------------------------- #

    def test_source_spec(self):
        """Add a source group and verify it round-trips through finalize."""
        b = self._builder()

        self.lib.builder_add_var(b, 0, 8, 0, 0, 100)
        self.lib.builder_add_var(b, 1, 8, 0, 0, 100)

        var_ids = (ctypes.c_uint32 * 2)(0, 1)
        src_ref = self.lib.builder_add_source(b, 2, var_ids)
        assert src_ref != EXPR_NULL

        size = ctypes.c_size_t(0)
        sp = self.lib.builder_finalize(b, ctypes.byref(size))
        assert sp

        head = SolveProblemHead.from_address(sp)
        assert head.n_sources == 1
        assert head.n_vars == 2

        # Still solvable
        values = _compile_and_solve(self.lib, sp, 2)
        for v in values:
            assert 0 <= v <= 100

        self.lib.builder_free_problem(b, sp, size.value)
        self.lib.builder_destroy(b)

    # -- empty problem --------------------------------------------- #

    def test_empty_finalize(self):
        """Finalize an empty builder produces valid (but trivial) buffer."""
        b = self._builder()
        size = ctypes.c_size_t(0)
        sp = self.lib.builder_finalize(b, ctypes.byref(size))
        assert sp
        head = SolveProblemHead.from_address(sp)
        assert head.n_vars == 0
        assert head.n_constraints == 0
        assert head.n_sources == 0
        self.lib.builder_free_problem(b, sp, size.value)
        self.lib.builder_destroy(b)

    # -- virtual_used ---------------------------------------------- #

    def test_virtual_used_grows(self):
        """builder_virtual_used increases as allocations are made."""
        b = self._builder()
        assert self.lib.builder_virtual_used(b) == 0

        self.lib.builder_add_var(b, 0, 8, 0, 0, 100)
        used1 = self.lib.builder_virtual_used(b)
        assert used1 > 0

        self.lib.builder_add_var(b, 1, 8, 0, 0, 100)
        used2 = self.lib.builder_virtual_used(b)
        assert used2 > used1

        self.lib.builder_destroy(b)
