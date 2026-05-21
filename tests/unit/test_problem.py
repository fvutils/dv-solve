"""Unit tests for zsp_problem (Phase 3).

Coverage:
- solve_problem_init: basic init, too-small buffer
- 3-variable / 2-constraint problem: counts and linked-list traversal
- All 9 expression node types: construction and POOL_PTR access
- Overflow: pool sticky error flag after exhaustion
- solve_problem_reset: pool reusable, same base offset
- DAG sharing: same ExprRef as child of two parent nodes
- expr_in_set_elems / source_spec_vars helpers
"""
from __future__ import annotations

import ctypes
import pytest

# ------------------------------------------------------------------ #
# ctypes struct mirrors of C types                                    #
# ------------------------------------------------------------------ #

EXPR_NULL = 0xFFFFFFFF

# ExprKind values
EXPR_CONST    = 0
EXPR_VAR      = 1
EXPR_BINARY   = 2
EXPR_UNARY    = 3
EXPR_ITE      = 4
EXPR_IN_RANGE = 5
EXPR_IN_SET   = 6
EXPR_EXTEND   = 7
EXPR_EXTRACT  = 8

# BinOp values
BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD = 0, 1, 2, 3, 4
BIN_BAND, BIN_BOR, BIN_BXOR, BIN_LSHIFT, BIN_RSHIFT = 5, 6, 7, 8, 9
BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE = 10, 11, 12, 13
BIN_GT, BIN_GTE = 14, 15
BIN_AND, BIN_OR = 16, 17

# UnaryOp values
UN_NEG, UN_NOT, UN_INVERT = 0, 1, 2


class ExprConst(ctypes.Structure):
    _fields_ = [
        ("kind",      ctypes.c_uint32),
        ("is_signed", ctypes.c_uint8),
        ("_pad",      ctypes.c_uint8 * 3),
        ("value",     ctypes.c_int64),
    ]

class ExprVar(ctypes.Structure):
    _fields_ = [
        ("kind",   ctypes.c_uint32),
        ("var_id", ctypes.c_uint32),
    ]

class ExprBinary(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_uint32),
        ("op",   ctypes.c_uint32),
        ("lhs",  ctypes.c_uint32),
        ("rhs",  ctypes.c_uint32),
    ]

class ExprUnary(ctypes.Structure):
    _fields_ = [
        ("kind",    ctypes.c_uint32),
        ("op",      ctypes.c_uint32),
        ("operand", ctypes.c_uint32),
    ]

class ExprITE(ctypes.Structure):
    _fields_ = [
        ("kind",   ctypes.c_uint32),
        ("cond",   ctypes.c_uint32),
        ("then_e", ctypes.c_uint32),
        ("else_e", ctypes.c_uint32),
    ]

class ExprInRange(ctypes.Structure):
    _fields_ = [
        ("kind",  ctypes.c_uint32),
        ("value", ctypes.c_uint32),
        ("lo",    ctypes.c_uint32),
        ("hi",    ctypes.c_uint32),
    ]

class ExprInSet(ctypes.Structure):
    _fields_ = [
        ("kind",    ctypes.c_uint32),
        ("value",   ctypes.c_uint32),
        ("n_elems", ctypes.c_uint32),
    ]

class ExprExtend(ctypes.Structure):
    _fields_ = [
        ("kind",         ctypes.c_uint32),
        ("sign_extend",  ctypes.c_uint8),
        ("from_bits",    ctypes.c_uint8),
        ("to_bits",      ctypes.c_uint8),
        ("_pad",         ctypes.c_uint8),
        ("operand",      ctypes.c_uint32),
    ]

class ExprExtract(ctypes.Structure):
    _fields_ = [
        ("kind",    ctypes.c_uint32),
        ("hi_bit",  ctypes.c_uint8),
        ("lo_bit",  ctypes.c_uint8),
        ("_pad",    ctypes.c_uint8 * 2),
        ("operand", ctypes.c_uint32),
    ]

class VarSpec(ctypes.Structure):
    _fields_ = [
        ("next",      ctypes.c_uint32),
        ("var_id",    ctypes.c_uint32),
        ("width",     ctypes.c_uint8),
        ("is_signed", ctypes.c_uint8),
        ("_pad",      ctypes.c_uint8 * 2),
        ("lo",        ctypes.c_int64),
        ("hi",        ctypes.c_int64),
    ]

class ConstraintSpec(ctypes.Structure):
    _fields_ = [
        ("next", ctypes.c_uint32),
        ("root", ctypes.c_uint32),
    ]

class SourceSpec(ctypes.Structure):
    _fields_ = [
        ("next",   ctypes.c_uint32),
        ("n_vars", ctypes.c_uint32),
    ]

# SolveProblem head — we only access the first 7 uint32 fields + pool address
# (pool is embedded; we use solve_problem_pool_base to get &sp->pool)
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
        # pool header (zsp_pool_t = 4 × uint32) follows
    ]


# ------------------------------------------------------------------ #
# ctypes setup                                                        #
# ------------------------------------------------------------------ #

def _setup(lib: ctypes.CDLL):
    lib.solve_problem_init.restype  = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    lib.solve_problem_init_sized.restype  = ctypes.c_void_p
    lib.solve_problem_init_sized.argtypes = [
        ctypes.c_void_p, ctypes.c_size_t,
        ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]

    lib.solve_problem_reset.restype  = None
    lib.solve_problem_reset.argtypes = [ctypes.c_void_p]

    lib.solve_problem_destroy.restype  = None
    lib.solve_problem_destroy.argtypes = [ctypes.c_void_p]

    lib.solve_problem_pool_base.restype  = ctypes.c_void_p
    lib.solve_problem_pool_base.argtypes = [ctypes.c_void_p]

    lib.expr_const.restype  = ctypes.c_uint32
    lib.expr_const.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8]

    lib.expr_var.restype  = ctypes.c_uint32
    lib.expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.expr_binary.restype  = ctypes.c_uint32
    lib.expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint32]

    lib.expr_unary.restype  = ctypes.c_uint32
    lib.expr_unary.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]

    lib.expr_ite.restype  = ctypes.c_uint32
    lib.expr_ite.argtypes = [ctypes.c_void_p,
                             ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]

    lib.expr_in_range.restype  = ctypes.c_uint32
    lib.expr_in_range.argtypes = [ctypes.c_void_p,
                                  ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]

    lib.expr_in_set.restype  = ctypes.c_uint32
    lib.expr_in_set.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_void_p]

    lib.expr_extend.restype  = ctypes.c_uint32
    lib.expr_extend.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]

    lib.expr_extract.restype  = ctypes.c_uint32
    lib.expr_extract.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                 ctypes.c_uint8, ctypes.c_uint8]

    lib.problem_add_var.restype  = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]

    lib.problem_add_constraint.restype  = ctypes.c_uint32
    lib.problem_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.problem_add_source.restype  = ctypes.c_uint32
    lib.problem_add_source.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]

    lib.expr_in_set_elems.restype  = ctypes.c_void_p
    lib.expr_in_set_elems.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.source_spec_vars.restype  = ctypes.c_void_p
    lib.source_spec_vars.argtypes = [ctypes.c_void_p, ctypes.c_uint32]


# ------------------------------------------------------------------ #
# Helpers                                                             #
# ------------------------------------------------------------------ #

_BUF_SIZE = 65536  # 64 KiB — enough for all tests


def _make_buf():
    return (ctypes.c_uint8 * _BUF_SIZE)()


def _pool_ptr(pool_base: int, ref: int):
    """Compute POOL_PTR(sp, ref) as an integer address."""
    assert ref != EXPR_NULL
    return pool_base + ref


def _read_struct(cls, addr: int):
    return cls.from_address(addr)


# ------------------------------------------------------------------ #
# Test class                                                          #
# ------------------------------------------------------------------ #

class TestProblem:
    @pytest.fixture(autouse=True)
    def setup_lib(self, libzsp):
        _setup(libzsp)
        self.lib = libzsp

    # -- lifecycle -------------------------------------------------- #

    def test_init_returns_non_null(self):
        buf = _make_buf()
        sp = self.lib.solve_problem_init(buf, _BUF_SIZE)
        assert sp is not None

    def test_init_too_small_returns_null(self):
        # Buffer smaller than SolveProblem struct + pool header must fail.
        # SolveProblem is ~64 bytes; use 32 bytes to be safe.
        tiny = (ctypes.c_uint8 * 32)()
        sp = self.lib.solve_problem_init(tiny, 32)
        assert sp is None

    def test_pool_base_accessible(self):
        buf = _make_buf()
        sp = self.lib.solve_problem_init(buf, _BUF_SIZE)
        assert sp is not None
        base = self.lib.solve_problem_pool_base(sp)
        assert base is not None and base != 0

    # -- 3-variable / 2-constraint problem -------------------------- #

    def _build_3var_2constraint(self):
        """Return (sp, buf) with 3 vars and 2 constraints added."""
        buf = _make_buf()
        sp = self.lib.solve_problem_init(buf, _BUF_SIZE)
        assert sp is not None
        self.lib.problem_add_var(sp, 0, 8,  0,  0,  255)
        self.lib.problem_add_var(sp, 1, 8,  1, -128, 127)
        self.lib.problem_add_var(sp, 2, 32, 0,  0,  1000)
        # constraint 0: var0 < var1  (BIN_LT)
        v0 = self.lib.expr_var(sp, 0)
        v1 = self.lib.expr_var(sp, 1)
        c0_expr = self.lib.expr_binary(sp, BIN_LT, v0, v1)
        self.lib.problem_add_constraint(sp, c0_expr)
        # constraint 1: var2 >= 10
        v2   = self.lib.expr_var(sp, 2)
        c10  = self.lib.expr_const(sp, 10, 0)
        c1_expr = self.lib.expr_binary(sp, BIN_GTE, v2, c10)
        self.lib.problem_add_constraint(sp, c1_expr)
        return sp, buf

    def test_counts_correct(self):
        sp, buf = self._build_3var_2constraint()
        head = SolveProblemHead.from_address(sp)
        assert head.n_vars == 3
        assert head.n_constraints == 2
        assert head.n_sources == 0

    def test_varspec_linked_list(self):
        sp, buf = self._build_3var_2constraint()
        head = SolveProblemHead.from_address(sp)
        pool_base = self.lib.solve_problem_pool_base(sp)

        var_ids_seen = []
        ref = head.vars_head
        while ref != EXPR_NULL:
            v = VarSpec.from_address(_pool_ptr(pool_base, ref))
            var_ids_seen.append(v.var_id)
            ref = v.next

        assert sorted(var_ids_seen) == [0, 1, 2]

    def test_constraint_linked_list(self):
        sp, buf = self._build_3var_2constraint()
        head = SolveProblemHead.from_address(sp)
        pool_base = self.lib.solve_problem_pool_base(sp)

        count = 0
        ref = head.constraints_head
        while ref != EXPR_NULL:
            c = ConstraintSpec.from_address(_pool_ptr(pool_base, ref))
            assert c.root != EXPR_NULL
            count += 1
            ref = c.next
        assert count == 2

    # -- all expression node types ---------------------------------- #

    def _sp(self):
        buf = _make_buf()
        sp = self.lib.solve_problem_init(buf, _BUF_SIZE)
        assert sp is not None
        return sp, buf

    def test_expr_const(self):
        sp, buf = self._sp()
        ref = self.lib.expr_const(sp, 42, 1)
        assert ref != EXPR_NULL
        base = self.lib.solve_problem_pool_base(sp)
        n = ExprConst.from_address(_pool_ptr(base, ref))
        assert n.kind == EXPR_CONST
        assert n.value == 42
        assert n.is_signed == 1

    def test_expr_const_negative(self):
        sp, buf = self._sp()
        ref = self.lib.expr_const(sp, -100, 1)
        assert ref != EXPR_NULL
        base = self.lib.solve_problem_pool_base(sp)
        n = ExprConst.from_address(_pool_ptr(base, ref))
        assert n.value == -100

    def test_expr_var(self):
        sp, buf = self._sp()
        ref = self.lib.expr_var(sp, 7)
        assert ref != EXPR_NULL
        base = self.lib.solve_problem_pool_base(sp)
        n = ExprVar.from_address(_pool_ptr(base, ref))
        assert n.kind == EXPR_VAR
        assert n.var_id == 7

    def test_expr_binary_all_ops(self):
        sp, buf = self._sp()
        base = self.lib.solve_problem_pool_base(sp)
        lhs = self.lib.expr_var(sp, 0)
        rhs = self.lib.expr_const(sp, 5, 0)
        for op in range(18):  # BIN_ADD .. BIN_OR
            ref = self.lib.expr_binary(sp, op, lhs, rhs)
            assert ref != EXPR_NULL, f"BinOp {op} returned EXPR_NULL"
            n = ExprBinary.from_address(_pool_ptr(base, ref))
            assert n.kind == EXPR_BINARY
            assert n.op == op
            assert n.lhs == lhs
            assert n.rhs == rhs

    def test_expr_unary_all_ops(self):
        sp, buf = self._sp()
        base = self.lib.solve_problem_pool_base(sp)
        operand = self.lib.expr_var(sp, 0)
        for op in (UN_NEG, UN_NOT, UN_INVERT):
            ref = self.lib.expr_unary(sp, op, operand)
            assert ref != EXPR_NULL
            n = ExprUnary.from_address(_pool_ptr(base, ref))
            assert n.kind == EXPR_UNARY
            assert n.op == op

    def test_expr_ite(self):
        sp, buf = self._sp()
        base = self.lib.solve_problem_pool_base(sp)
        cond  = self.lib.expr_var(sp, 0)
        then_e = self.lib.expr_const(sp, 1, 0)
        else_e = self.lib.expr_const(sp, 0, 0)
        ref = self.lib.expr_ite(sp, cond, then_e, else_e)
        assert ref != EXPR_NULL
        n = ExprITE.from_address(_pool_ptr(base, ref))
        assert n.kind == EXPR_ITE
        assert n.cond   == cond
        assert n.then_e == then_e
        assert n.else_e == else_e

    def test_expr_in_range(self):
        sp, buf = self._sp()
        base = self.lib.solve_problem_pool_base(sp)
        val = self.lib.expr_var(sp, 0)
        lo  = self.lib.expr_const(sp, 0,   0)
        hi  = self.lib.expr_const(sp, 100, 0)
        ref = self.lib.expr_in_range(sp, val, lo, hi)
        assert ref != EXPR_NULL
        n = ExprInRange.from_address(_pool_ptr(base, ref))
        assert n.kind  == EXPR_IN_RANGE
        assert n.value == val
        assert n.lo    == lo
        assert n.hi    == hi

    def test_expr_in_set(self):
        sp, buf = self._sp()
        base = self.lib.solve_problem_pool_base(sp)
        val = self.lib.expr_var(sp, 0)
        e2  = self.lib.expr_const(sp, 2, 0)
        e5  = self.lib.expr_const(sp, 5, 0)
        e9  = self.lib.expr_const(sp, 9, 0)
        elems = (ctypes.c_uint32 * 3)(e2, e5, e9)
        ref = self.lib.expr_in_set(sp, val, 3, elems)
        assert ref != EXPR_NULL
        n = ExprInSet.from_address(_pool_ptr(base, ref))
        assert n.kind    == EXPR_IN_SET
        assert n.value   == val
        assert n.n_elems == 3

    def test_expr_in_set_elems_helper(self):
        sp, buf = self._sp()
        val   = self.lib.expr_var(sp, 0)
        e2    = self.lib.expr_const(sp, 2, 0)
        e5    = self.lib.expr_const(sp, 5, 0)
        elems = (ctypes.c_uint32 * 2)(e2, e5)
        ref = self.lib.expr_in_set(sp, val, 2, elems)
        assert ref != EXPR_NULL
        ptr = self.lib.expr_in_set_elems(sp, ref)
        assert ptr is not None
        arr = (ctypes.c_uint32 * 2).from_address(ptr)
        assert arr[0] == e2
        assert arr[1] == e5

    def test_expr_extend(self):
        sp, buf = self._sp()
        base = self.lib.solve_problem_pool_base(sp)
        operand = self.lib.expr_var(sp, 0)
        ref = self.lib.expr_extend(sp, operand, 8, 32, 1)
        assert ref != EXPR_NULL
        n = ExprExtend.from_address(_pool_ptr(base, ref))
        assert n.kind        == EXPR_EXTEND
        assert n.from_bits   == 8
        assert n.to_bits     == 32
        assert n.sign_extend == 1
        assert n.operand     == operand

    def test_expr_extract(self):
        sp, buf = self._sp()
        base = self.lib.solve_problem_pool_base(sp)
        operand = self.lib.expr_var(sp, 0)
        ref = self.lib.expr_extract(sp, operand, 7, 0)
        assert ref != EXPR_NULL
        n = ExprExtract.from_address(_pool_ptr(base, ref))
        assert n.kind    == EXPR_EXTRACT
        assert n.hi_bit  == 7
        assert n.lo_bit  == 0
        assert n.operand == operand

    # -- DAG sharing ------------------------------------------------ #

    def test_dag_sharing(self):
        """Same ExprRef used as child of two different parent nodes."""
        sp, buf = self._sp()
        shared = self.lib.expr_const(sp, 99, 0)
        v0 = self.lib.expr_var(sp, 0)
        v1 = self.lib.expr_var(sp, 1)
        # shared used as rhs of both parents
        parent_a = self.lib.expr_binary(sp, BIN_LT,  v0, shared)
        parent_b = self.lib.expr_binary(sp, BIN_GTE, v1, shared)
        assert parent_a != EXPR_NULL
        assert parent_b != EXPR_NULL
        base = self.lib.solve_problem_pool_base(sp)
        na = ExprBinary.from_address(_pool_ptr(base, parent_a))
        nb = ExprBinary.from_address(_pool_ptr(base, parent_b))
        assert na.rhs == shared
        assert nb.rhs == shared

    # -- overflow --------------------------------------------------- #

    def test_overflow_returns_expr_null(self):
        """Exhausting the pool makes all builders return EXPR_NULL."""
        # Use a small buffer so we hit overflow quickly
        small_size = 256
        small = (ctypes.c_uint8 * small_size)()
        sp = self.lib.solve_problem_init(small, small_size)
        if sp is None:
            pytest.skip("Buffer too small for SolveProblem header")
        # Flood the pool
        hit_null = False
        for i in range(200):
            ref = self.lib.expr_const(sp, i, 0)
            if ref == EXPR_NULL:
                hit_null = True
                break
        assert hit_null, "Expected overflow but never got EXPR_NULL"

    def test_overflow_sticky(self):
        small_size = 256
        small = (ctypes.c_uint8 * small_size)()
        sp = self.lib.solve_problem_init(small, small_size)
        if sp is None:
            pytest.skip("Buffer too small for SolveProblem header")
        for i in range(200):
            if self.lib.expr_const(sp, i, 0) == EXPR_NULL:
                break
        # After overflow, all subsequent calls must also return EXPR_NULL
        for _ in range(5):
            assert self.lib.expr_const(sp, 0, 0) == EXPR_NULL

    # -- reset ------------------------------------------------------ #

    def test_reset_clears_counts(self):
        sp, buf = self._build_3var_2constraint()
        self.lib.solve_problem_reset(sp)
        head = SolveProblemHead.from_address(sp)
        assert head.n_vars        == 0
        assert head.n_constraints == 0
        assert head.n_sources     == 0
        assert head.vars_head        == EXPR_NULL
        assert head.constraints_head == EXPR_NULL

    def test_reset_pool_reusable(self):
        sp, buf = self._sp()
        ref1 = self.lib.expr_const(sp, 1, 0)
        assert ref1 != EXPR_NULL
        self.lib.solve_problem_reset(sp)
        ref2 = self.lib.expr_const(sp, 2, 0)
        assert ref2 != EXPR_NULL
        # After reset, new allocation starts from same base offset as first
        assert ref2 == ref1, "Expected same base offset after reset"

    def test_reset_after_overflow(self):
        small_size = 256
        small = (ctypes.c_uint8 * small_size)()
        sp = self.lib.solve_problem_init(small, small_size)
        if sp is None:
            pytest.skip("Buffer too small for SolveProblem header")
        for i in range(200):
            if self.lib.expr_const(sp, i, 0) == EXPR_NULL:
                break
        self.lib.solve_problem_reset(sp)
        # Should be able to allocate again after reset
        ref = self.lib.expr_const(sp, 42, 0)
        assert ref != EXPR_NULL

    # -- source spec ------------------------------------------------ #

    def test_problem_add_source(self):
        sp, buf = self._sp()
        var_ids = (ctypes.c_uint32 * 3)(0, 1, 2)
        ref = self.lib.problem_add_source(sp, 3, var_ids)
        assert ref != EXPR_NULL
        head = SolveProblemHead.from_address(sp)
        assert head.n_sources == 1
        assert head.sources_head == ref

    def test_source_spec_vars_helper(self):
        sp, buf = self._sp()
        var_ids = (ctypes.c_uint32 * 2)(10, 20)
        ref = self.lib.problem_add_source(sp, 2, var_ids)
        assert ref != EXPR_NULL
        ptr = self.lib.source_spec_vars(sp, ref)
        assert ptr is not None
        arr = (ctypes.c_uint32 * 2).from_address(ptr)
        assert arr[0] == 10
        assert arr[1] == 20
