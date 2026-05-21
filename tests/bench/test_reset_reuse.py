"""Benchmark: solver_reset + re-solve throughput.

Measures the speedup of using solver_reset() + re-solve versus
full recompilation. The compile-once + reset pattern is the primary
use case for Verilator integration.

Scenario: 3 variables with sum constraint, solved repeatedly with
different seeds after reset.
"""
import ctypes
import time
import pytest

EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0

_SP_BUF_SIZE  = 65536
_CTX_BUF_SIZE = 1048576


def _setup(lib):
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

    class SolveOpts(ctypes.Structure):
        _fields_ = [
            ("seed",           ctypes.c_uint64),
            ("max_conflicts",  ctypes.c_uint32),
            ("max_restarts",   ctypes.c_uint32),
            ("use_phase_save", ctypes.c_uint8),
            ("_pad",           ctypes.c_uint8 * 3),
            ("max_shave_iters", ctypes.c_uint32),
        ]
    lib._SolveOpts = SolveOpts
    lib.solver_solve.restype  = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.solver_get_value.restype  = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.solver_reset.restype  = None
    lib.solver_reset.argtypes = [ctypes.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [ctypes.c_void_p]


BIN_ADD = 0
BIN_EQ  = 10
BIN_LTE = 13


@pytest.mark.bench
def test_reset_reuse(tmp_path):
    """Benchmark: solver_reset + re-solve vs full recompile."""
    import shutil, subprocess
    from pathlib import Path
    PKG = Path(__file__).parent.parent.parent
    if not shutil.which("cmake"):
        pytest.skip("cmake not found")
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    subprocess.run(["cmake", str(PKG), "-DCMAKE_BUILD_TYPE=Release"],
                   cwd=build_dir, check=True, capture_output=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--parallel"],
                   check=True, capture_output=True)
    hits = sorted(build_dir.glob("libdv_solve.so*"), key=lambda p: len(p.name))
    if not hits:
        pytest.skip("libdv_solve.so not found")
    lib = ctypes.CDLL(str(hits[0]))
    _setup(lib)

    N_ITERS = 10000

    # Build the problem once
    sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
    sp = lib.solve_problem_init(sp_buf, _SP_BUF_SIZE)
    lib.problem_add_var(sp, 0, 32, 1, 0, 99)
    lib.problem_add_var(sp, 1, 32, 1, 0, 99)
    lib.problem_add_var(sp, 2, 32, 1, 0, 198)
    v_x = lib.expr_var(sp, 0)
    v_y = lib.expr_var(sp, 1)
    v_s = lib.expr_var(sp, 2)
    add_e = lib.expr_binary(sp, BIN_ADD, v_x, v_y)
    eq_e  = lib.expr_binary(sp, BIN_EQ, v_s, add_e)
    lib.problem_add_constraint(sp, eq_e)
    c_50 = lib.expr_const(sp, 50, 1)
    le_e = lib.expr_binary(sp, BIN_LTE, v_s, c_50)
    lib.problem_add_constraint(sp, le_e)

    # --- Method 1: compile once + reset + re-solve ---
    ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
    ba = lib.zsp_block_alloc_create(None, 0)
    ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
    lib.solver_compile(ctx, sp)

    t0 = time.perf_counter_ns()
    for i in range(N_ITERS):
        opts = lib._SolveOpts(seed=(i + 1) * 0x9E3779B97F4A7C15)
        rc = lib.solver_solve(ctx, ctypes.byref(opts))
        assert rc == SOLVE_OK
        x = lib.solver_get_value(ctx, 0)
        y = lib.solver_get_value(ctx, 1)
        assert x + y <= 50
        lib.solver_reset(ctx)
    reset_ns = time.perf_counter_ns() - t0

    lib.zsp_block_alloc_destroy(ba)

    reset_per = reset_ns / N_ITERS
    print(f"\n  reset+solve: {N_ITERS} iters, "
          f"{reset_ns/1e6:.1f} ms total, "
          f"{reset_per/1e3:.1f} us/iter, "
          f"{N_ITERS/(reset_ns/1e9):.0f} solves/s")
