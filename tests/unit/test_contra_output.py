"""Unit tests for text and JSON output formatters (Sprint 4).

Tests proof_text and proof_json output from contra_analyze_unsat().
"""
from __future__ import annotations

import ctypes
import json
import pytest

EXPR_NULL = 0xFFFF_FFFF
_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 524288

BIN_EQ  = 10
BIN_LTE = 13
BIN_GTE = 15


class ContraRelaxSuggestion(ctypes.Structure):
    _fields_ = [
        ("constraint_id",     ctypes.c_uint32),
        ("original_constant", ctypes.c_int64),
        ("relaxed_constant",  ctypes.c_int64),
        ("delta",             ctypes.c_int64),
        ("is_relaxable",      ctypes.c_uint8),
        ("relax_direction",   ctypes.c_uint8),
        ("_rpad",             ctypes.c_uint8 * 6),
    ]


class ContraConstraintInfo(ctypes.Structure):
    _fields_ = [
        ("constraint_id", ctypes.c_uint32),
        ("name",          ctypes.c_char_p),
        ("source_file",   ctypes.c_char_p),
        ("source_line",   ctypes.c_uint32),
    ]


class ContraOpts(ctypes.Structure):
    _fields_ = [
        ("max_solver_calls",   ctypes.c_uint32),
        ("time_limit_sec",     ctypes.c_double),
        ("skip_minimization",  ctypes.c_uint8),
        ("emit_json",          ctypes.c_uint8),
        ("emit_proof",         ctypes.c_uint8),
        ("compute_relaxations", ctypes.c_uint8),
        ("find_alternatives",  ctypes.c_uint8),
        ("_pad",               ctypes.c_uint8 * 3),
        ("constraint_info",    ctypes.POINTER(ContraConstraintInfo)),
        ("n_constraint_info",  ctypes.c_uint32),
    ]


class ContraResult(ctypes.Structure):
    _fields_ = [
        ("mus_constraint_ids", ctypes.POINTER(ctypes.c_uint32)),
        ("mus_size",           ctypes.c_uint32),
        ("proof_text",         ctypes.c_char_p),
        ("proof_json",         ctypes.c_char_p),
        ("core_size",          ctypes.c_uint32),
        ("n_solver_calls",     ctypes.c_uint32),
        ("elapsed_sec",        ctypes.c_double),
        ("relaxations",        ctypes.POINTER(ContraRelaxSuggestion)),
        ("n_relaxations",      ctypes.c_uint32),
        ("unconfirmed",        ctypes.c_uint8),
        ("_res_pad",           ctypes.c_uint8 * 7),
    ]


def _setup(lib: ctypes.CDLL):
    lib.zsp_block_alloc_create.restype  = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    lib.solve_problem_init.restype  = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.problem_add_var.restype  = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]
    lib.problem_add_constraint.restype  = ctypes.c_uint32
    lib.problem_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.expr_var.restype  = ctypes.c_uint32
    lib.expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.expr_const.restype  = ctypes.c_uint32
    lib.expr_const.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8]
    lib.expr_binary.restype  = ctypes.c_uint32
    lib.expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint32]

    lib.solver_create.restype  = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_compile.restype  = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

    lib.contra_analyze_unsat.restype  = ctypes.c_int
    lib.contra_analyze_unsat.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(ContraOpts), ctypes.POINTER(ContraResult)
    ]
    lib.contra_result_free.restype  = None
    lib.contra_result_free.argtypes = [ctypes.POINTER(ContraResult)]


def _make_ctx(lib):
    ba = lib.zsp_block_alloc_create(None, 4096)
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    assert ctx
    return ctx_buf, ctx, ba


def _build_trivial_unsat(lib):
    """Build a trivial UNSAT problem: x >= 10, x <= 5."""
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 8, 0, 0, 100)
    vx = lib.expr_var(sp, 0)
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_GTE, vx,
                               lib.expr_const(sp, 10, 0)))
    lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_LTE, vx,
                               lib.expr_const(sp, 5, 0)))
    return sp_buf, sp


def test_proof_text_format(libzsp_debug):
    """Text output contains UNSATISFIABLE header and constraint labels."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf, sp = _build_trivial_unsat(lib)
    ctx_buf, ctx, ba = _make_ctx(lib)

    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0

    text = result.proof_text
    assert text is not None, "proof_text should not be None"
    text_str = text.decode("utf-8")

    assert "UNSATISFIABLE" in text_str
    assert "[C1]" in text_str or "[C2]" in text_str
    assert "Relaxation" in text_str

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)


def test_proof_json_valid(libzsp_debug):
    """JSON output parses successfully and contains expected keys."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf, sp = _build_trivial_unsat(lib)
    ctx_buf, ctx, ba = _make_ctx(lib)

    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, None, ctypes.byref(result))
    assert rc == 0

    json_str = result.proof_json
    assert json_str is not None
    data = json.loads(json_str.decode("utf-8"))

    assert "mus" in data
    assert "relaxations" in data
    assert "mus_size" in data
    assert "n_solver_calls" in data
    assert len(data["mus"]) == result.mus_size

    # Each MUS entry should have an id
    for entry in data["mus"]:
        assert "id" in entry

    # Relaxation entries should have constraint_id
    for entry in data["relaxations"]:
        assert "constraint_id" in entry
        assert "is_relaxable" in entry

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)


def test_proof_with_constraint_info(libzsp_debug):
    """When constraint names are provided, they appear in the output."""
    lib = libzsp_debug
    _setup(lib)

    sp_buf, sp = _build_trivial_unsat(lib)
    ctx_buf, ctx, ba = _make_ctx(lib)

    # Provide constraint info
    infos = (ContraConstraintInfo * 2)()
    infos[0].constraint_id = 1
    infos[0].name = b"x >= 10"
    infos[0].source_file = b"test.pss"
    infos[0].source_line = 42
    infos[1].constraint_id = 2
    infos[1].name = b"x <= 5"
    infos[1].source_file = b"test.pss"
    infos[1].source_line = 43

    opts = ContraOpts()
    memset_fn = ctypes.memset
    memset_fn(ctypes.byref(opts), 0, ctypes.sizeof(opts))
    opts.emit_json = 1
    opts.constraint_info = infos
    opts.n_constraint_info = 2

    result = ContraResult()
    rc = lib.contra_analyze_unsat(ctx, sp, ctypes.byref(opts),
                                   ctypes.byref(result))
    assert rc == 0

    # Text should contain the constraint names
    text = result.proof_text.decode("utf-8")
    assert "x >= 10" in text
    assert "x <= 5" in text
    assert "test.pss" in text

    # JSON should contain the names too
    data = json.loads(result.proof_json.decode("utf-8"))
    names = [e.get("name") for e in data["mus"]]
    assert "x >= 10" in names or "x <= 5" in names

    lib.contra_result_free(ctypes.byref(result))
    lib.zsp_block_alloc_destroy(ba)
