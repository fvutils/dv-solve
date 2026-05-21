"""Unit tests for placement-specific propagators.

Tests MinOf_N, MaxOf_N, NoOverlap2D, and solver_optimize.
"""
import ctypes
import os
import sys
import pytest
from pathlib import Path

# Ensure the solver module is importable
_ROOT = Path(__file__).parent.parent.parent.parent
if str(_ROOT / "src") not in sys.path:
    sys.path.insert(0, str(_ROOT / "src"))


def _get_lib():
    """Load the native library, skip test if unavailable."""
    from dv_solve.lib import _load_lib
    lib = _load_lib()
    if lib is None:
        pytest.skip("Native solver library not built")
    return lib


def _make_ctx(lib, n_vars, var_specs, buf_size=1 << 20):
    """Create a solver context with the given variables.

    var_specs: list of (var_id, width, is_signed, lo, hi)
    Returns (ctx, ba, ctx_buf, sp_buf) -- caller must clean up.
    """
    sp_buf = (ctypes.c_uint8 * 65536)()
    sp = lib.solve_problem_init(sp_buf, 65536)
    assert sp is not None

    for vid, w, s, lo, hi in var_specs:
        lib.problem_add_var(sp, ctypes.c_uint32(vid),
                            ctypes.c_uint8(w), ctypes.c_uint8(s),
                            ctypes.c_int64(lo), ctypes.c_int64(hi))

    # Source: all vars
    all_ids = [vs[0] for vs in var_specs]
    arr = (ctypes.c_uint32 * len(all_ids))(*all_ids)
    lib.problem_add_source(sp, ctypes.c_uint32(len(all_ids)), arr)

    ba = lib.zsp_block_alloc_create(None, buf_size)
    ctx_buf = (ctypes.c_uint8 * buf_size)()
    ctx = lib.solver_create(ctx_buf, buf_size, ba)
    assert ctx is not None

    rc = lib.solver_compile(ctx, sp)
    assert rc == 0

    return ctx, ba, ctx_buf, sp_buf


def _cleanup(lib, ba):
    lib.zsp_block_alloc_destroy(ba)


class _SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed", ctypes.c_uint64),
        ("max_conflicts", ctypes.c_uint32),
        ("max_restarts", ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad", ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


# ------------------------------------------------------------------ #
# MinOf_N tests                                                       #
# ------------------------------------------------------------------ #

class TestMinOfN:
    def test_min_of_3_basic(self):
        """r == min(a, b, c) with a,b,c in [0,10]; r should be achievable."""
        lib = _get_lib()
        # vars: 0=r, 1=a, 2=b, 3=c
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 4, [
            (0, 32, 0, 0, 10),
            (1, 32, 0, 2, 8),
            (2, 32, 0, 4, 10),
            (3, 32, 0, 1, 5),
        ])

        ops = (ctypes.c_uint32 * 3)(1, 2, 3)
        ref = lib.prop_add_min_of_n_32(ctx, ctypes.c_uint32(0),
                                        ctypes.c_uint32(3), ops,
                                        ctypes.c_uint8(1))
        assert ref != 0xFFFFFFFF

        sopts = _SolveOpts(seed=42, max_conflicts=100, max_restarts=1000)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        assert sr == 0  # SOLVE_OK

        r = lib.solver_get_value(ctx, ctypes.c_uint32(0))
        a = lib.solver_get_value(ctx, ctypes.c_uint32(1))
        b = lib.solver_get_value(ctx, ctypes.c_uint32(2))
        c = lib.solver_get_value(ctx, ctypes.c_uint32(3))

        assert r == min(a, b, c), f"r={r}, min({a},{b},{c})={min(a,b,c)}"
        _cleanup(lib, ba)

    def test_min_of_2_singleton(self):
        """r == min(a, b) where b is fixed at 3."""
        lib = _get_lib()
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 3, [
            (0, 32, 0, 0, 10),
            (1, 32, 0, 0, 10),
            (2, 32, 0, 3, 3),   # b fixed at 3
        ])

        ops = (ctypes.c_uint32 * 2)(1, 2)
        lib.prop_add_min_of_n_32(ctx, ctypes.c_uint32(0),
                                  ctypes.c_uint32(2), ops,
                                  ctypes.c_uint8(1))

        sopts = _SolveOpts(seed=99, max_conflicts=100, max_restarts=1000)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        assert sr == 0

        r = lib.solver_get_value(ctx, ctypes.c_uint32(0))
        a = lib.solver_get_value(ctx, ctypes.c_uint32(1))
        b = lib.solver_get_value(ctx, ctypes.c_uint32(2))
        assert r == min(a, b)
        assert b == 3
        _cleanup(lib, ba)


# ------------------------------------------------------------------ #
# MaxOf_N tests                                                       #
# ------------------------------------------------------------------ #

class TestMaxOfN:
    def test_max_of_3_basic(self):
        """r == max(a, b, c) with varying domains."""
        lib = _get_lib()
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 4, [
            (0, 32, 0, 0, 20),
            (1, 32, 0, 2, 8),
            (2, 32, 0, 4, 10),
            (3, 32, 0, 1, 15),
        ])

        ops = (ctypes.c_uint32 * 3)(1, 2, 3)
        lib.prop_add_max_of_n_32(ctx, ctypes.c_uint32(0),
                                  ctypes.c_uint32(3), ops,
                                  ctypes.c_uint8(1))

        sopts = _SolveOpts(seed=42, max_conflicts=100, max_restarts=1000)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        assert sr == 0

        r = lib.solver_get_value(ctx, ctypes.c_uint32(0))
        a = lib.solver_get_value(ctx, ctypes.c_uint32(1))
        b = lib.solver_get_value(ctx, ctypes.c_uint32(2))
        c = lib.solver_get_value(ctx, ctypes.c_uint32(3))

        assert r == max(a, b, c), f"r={r}, max({a},{b},{c})={max(a,b,c)}"
        _cleanup(lib, ba)

    def test_max_of_2_fixed_result(self):
        """r == max(a, b), r fixed at 7. Both a,b must be <= 7, one == 7."""
        lib = _get_lib()
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 3, [
            (0, 32, 0, 7, 7),   # r fixed at 7
            (1, 32, 0, 0, 10),
            (2, 32, 0, 0, 10),
        ])

        ops = (ctypes.c_uint32 * 2)(1, 2)
        lib.prop_add_max_of_n_32(ctx, ctypes.c_uint32(0),
                                  ctypes.c_uint32(2), ops,
                                  ctypes.c_uint8(1))

        sopts = _SolveOpts(seed=42, max_conflicts=100, max_restarts=1000)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        assert sr == 0

        a = lib.solver_get_value(ctx, ctypes.c_uint32(1))
        b = lib.solver_get_value(ctx, ctypes.c_uint32(2))
        assert a <= 7 and b <= 7
        assert max(a, b) == 7
        _cleanup(lib, ba)


# ------------------------------------------------------------------ #
# NoOverlap2D tests                                                   #
# ------------------------------------------------------------------ #

class CRectSpec(ctypes.Structure):
    _fields_ = [
        ("x_id", ctypes.c_uint32),
        ("y_id", ctypes.c_uint32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("halo_l", ctypes.c_int32),
        ("halo_r", ctypes.c_int32),
        ("halo_t", ctypes.c_int32),
        ("halo_b", ctypes.c_int32),
    ]


class TestNoOverlap2D:
    def test_two_rects_horizontal(self):
        """Two 5x5 rects on a 10x5 canvas, must be side by side."""
        lib = _get_lib()
        # vars: 0=x0, 1=x1, 2=y0, 3=y1
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 4, [
            (0, 32, 0, 0, 5),   # x0: room for 5-wide rect
            (1, 32, 0, 0, 5),   # x1
            (2, 32, 0, 0, 0),   # y0: forced to 0
            (3, 32, 0, 0, 0),   # y1: forced to 0
        ])

        rects = (CRectSpec * 2)()
        rects[0] = CRectSpec(x_id=0, y_id=2, width=5, height=5,
                              halo_l=0, halo_r=0, halo_t=0, halo_b=0)
        rects[1] = CRectSpec(x_id=1, y_id=3, width=5, height=5,
                              halo_l=0, halo_r=0, halo_t=0, halo_b=0)

        ref = lib.prop_add_no_overlap_2d(ctx, ctypes.c_uint32(2),
                                          rects, ctypes.c_uint8(2))
        assert ref != 0xFFFFFFFF

        sopts = _SolveOpts(seed=42, max_conflicts=100, max_restarts=1000)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        assert sr == 0

        x0 = lib.solver_get_value(ctx, ctypes.c_uint32(0))
        x1 = lib.solver_get_value(ctx, ctypes.c_uint32(1))

        # With y forced to 0, they must be non-overlapping in x
        assert x0 + 5 <= x1 or x1 + 5 <= x0, \
            f"Overlap: x0={x0}, x1={x1}, both 5-wide"
        _cleanup(lib, ba)

    def test_three_rects_with_halo(self):
        """Three rects with halos on a canvas."""
        lib = _get_lib()
        # vars: 0,1,2 = x positions; 3,4,5 = y positions
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 6, [
            (0, 32, 0, 0, 40),
            (1, 32, 0, 0, 40),
            (2, 32, 0, 0, 40),
            (3, 32, 0, 0, 40),
            (4, 32, 0, 0, 40),
            (5, 32, 0, 0, 40),
        ])

        rects = (CRectSpec * 3)()
        rects[0] = CRectSpec(x_id=0, y_id=3, width=10, height=10,
                              halo_l=1, halo_r=1, halo_t=1, halo_b=1)
        rects[1] = CRectSpec(x_id=1, y_id=4, width=8, height=8,
                              halo_l=2, halo_r=2, halo_t=2, halo_b=2)
        rects[2] = CRectSpec(x_id=2, y_id=5, width=6, height=6,
                              halo_l=1, halo_r=1, halo_t=1, halo_b=1)

        ref = lib.prop_add_no_overlap_2d(ctx, ctypes.c_uint32(3),
                                          rects, ctypes.c_uint8(2))
        assert ref != 0xFFFFFFFF

        sopts = _SolveOpts(seed=42, max_conflicts=200, max_restarts=5000)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        assert sr == 0

        # Verify no overlap including halos
        positions = []
        for i in range(3):
            x = lib.solver_get_value(ctx, ctypes.c_uint32(i))
            y = lib.solver_get_value(ctx, ctypes.c_uint32(3 + i))
            positions.append((x, y))

        for i in range(3):
            for j in range(i + 1, 3):
                ri = rects[i]
                rj = rects[j]
                xi, yi = positions[i]
                xj, yj = positions[j]
                # Check non-overlap with halos
                ew_i = ri.width + ri.halo_l + ri.halo_r
                eh_i = ri.height + ri.halo_t + ri.halo_b
                ew_j = rj.width + rj.halo_l + rj.halo_r
                eh_j = rj.height + rj.halo_t + rj.halo_b
                xi_eff = xi - ri.halo_l
                yi_eff = yi - ri.halo_t
                xj_eff = xj - rj.halo_l
                yj_eff = yj - rj.halo_t

                x_sep = (xi_eff + ew_i <= xj_eff) or (xj_eff + ew_j <= xi_eff)
                y_sep = (yi_eff + eh_i <= yj_eff) or (yj_eff + eh_j <= yi_eff)
                assert x_sep or y_sep, \
                    f"Overlap with halo: rect {i} at ({xi},{yi}), rect {j} at ({xj},{yj})"
        _cleanup(lib, ba)

    def test_infeasible_tight(self):
        """Two 6x6 rects on a 10x5 canvas -- should be infeasible."""
        lib = _get_lib()
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 4, [
            (0, 32, 0, 0, 4),   # x0
            (1, 32, 0, 0, 4),   # x1
            (2, 32, 0, 0, 0),   # y0 = 0
            (3, 32, 0, 0, 0),   # y1 = 0
        ])

        rects = (CRectSpec * 2)()
        rects[0] = CRectSpec(x_id=0, y_id=2, width=6, height=6,
                              halo_l=0, halo_r=0, halo_t=0, halo_b=0)
        rects[1] = CRectSpec(x_id=1, y_id=3, width=6, height=6,
                              halo_l=0, halo_r=0, halo_t=0, halo_b=0)

        lib.prop_add_no_overlap_2d(ctx, ctypes.c_uint32(2),
                                    rects, ctypes.c_uint8(2))

        sopts = _SolveOpts(seed=42, max_conflicts=100, max_restarts=500)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        # Should be UNSAT or TIMEOUT (x range 0-4 can't fit two 6-wide with y=0)
        assert sr != 0, "Expected UNSAT for two 6x6 rects on [0,4]x{0} canvas"
        _cleanup(lib, ba)


# ------------------------------------------------------------------ #
# Optimization tests                                                  #
# ------------------------------------------------------------------ #

class TestOptimize:
    def test_minimize_sum(self):
        """Minimize r = a + b where a in [1,10], b in [1,10]."""
        lib = _get_lib()
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 3, [
            (0, 32, 0, 2, 20),   # r = a + b
            (1, 32, 0, 1, 10),   # a
            (2, 32, 0, 1, 10),   # b
        ])

        # Add r == a + b constraint via SumEq
        summands = (ctypes.c_uint32 * 2)(1, 2)
        lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(0),
                                ctypes.c_uint32(2), summands,
                                ctypes.c_uint8(1))

        # Optimize: minimize var 0
        class COptimizeOpts(ctypes.Structure):
            _fields_ = [
                ("seed", ctypes.c_uint64),
                ("max_conflicts", ctypes.c_uint32),
                ("max_restarts", ctypes.c_uint32),
                ("max_rounds", ctypes.c_uint32),
                ("time_limit_sec", ctypes.c_double),
                ("use_phase_save", ctypes.c_uint8),
                ("_pad", ctypes.c_uint8 * 3),
                ("max_shave_iters", ctypes.c_uint32),
            ]

        class COptimizeResult(ctypes.Structure):
            _fields_ = [
                ("found", ctypes.c_int),
                ("best_objective", ctypes.c_int64),
                ("n_rounds", ctypes.c_uint32),
                ("elapsed_sec", ctypes.c_double),
            ]

        opts = COptimizeOpts(seed=42, max_conflicts=100, max_restarts=1000,
                              max_rounds=50, time_limit_sec=5.0,
                              use_phase_save=1, max_shave_iters=500)
        result = COptimizeResult()

        rc = lib.solver_optimize(ctx, ctypes.c_uint32(0),
                                  ctypes.byref(opts), ctypes.byref(result))
        assert rc == 0
        assert result.found == 1
        assert result.best_objective == 2, \
            f"Expected min(a+b)=2, got {result.best_objective}"
        _cleanup(lib, ba)


# ------------------------------------------------------------------ #
# Combined HPWL test: MinOf + MaxOf + SumEq for net HPWL             #
# ------------------------------------------------------------------ #

class TestHPWL:
    def test_hpwl_2_macros_1_net(self):
        """Two macros, one net. HPWL = |x0 - x1| + |y0 - y1|.

        Model with min/max:
          xmin = min(x0, x1), xmax = max(x0, x1)
          ymin = min(y0, y1), ymax = max(y0, y1)
          hpwl_x = xmax - xmin (via xmin + hpwl_x = xmax)
          hpwl_y = ymax - ymin
          hpwl = hpwl_x + hpwl_y
        """
        lib = _get_lib()
        # vars: 0=x0, 1=y0, 2=x1, 3=y1,
        #        4=xmin, 5=xmax, 6=ymin, 7=ymax,
        #        8=hpwl_x, 9=hpwl_y, 10=hpwl
        ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, 11, [
            (0, 32, 0, 0, 50),   # x0
            (1, 32, 0, 0, 50),   # y0
            (2, 32, 0, 0, 50),   # x1
            (3, 32, 0, 0, 50),   # y1
            (4, 32, 0, 0, 50),   # xmin
            (5, 32, 0, 0, 50),   # xmax
            (6, 32, 0, 0, 50),   # ymin
            (7, 32, 0, 0, 50),   # ymax
            (8, 32, 0, 0, 100),  # hpwl_x
            (9, 32, 0, 0, 100),  # hpwl_y
            (10, 32, 0, 0, 200), # hpwl
        ])

        # xmin = min(x0, x1)
        x_ops = (ctypes.c_uint32 * 2)(0, 2)
        lib.prop_add_min_of_n_32(ctx, ctypes.c_uint32(4),
                                  ctypes.c_uint32(2), x_ops, ctypes.c_uint8(1))
        # xmax = max(x0, x1)
        lib.prop_add_max_of_n_32(ctx, ctypes.c_uint32(5),
                                  ctypes.c_uint32(2), x_ops, ctypes.c_uint8(1))
        # ymin = min(y0, y1)
        y_ops = (ctypes.c_uint32 * 2)(1, 3)
        lib.prop_add_min_of_n_32(ctx, ctypes.c_uint32(6),
                                  ctypes.c_uint32(2), y_ops, ctypes.c_uint8(1))
        # ymax = max(y0, y1)
        lib.prop_add_max_of_n_32(ctx, ctypes.c_uint32(7),
                                  ctypes.c_uint32(2), y_ops, ctypes.c_uint8(1))

        # hpwl_x = xmax - xmin  -->  xmin + hpwl_x = xmax
        # Rewrite: hpwl_x + xmin = xmax  (SumEq: xmax == hpwl_x + xmin)
        hpwl_x_summands = (ctypes.c_uint32 * 2)(8, 4)
        lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(5),
                                ctypes.c_uint32(2), hpwl_x_summands,
                                ctypes.c_uint8(1))

        # hpwl_y + ymin = ymax
        hpwl_y_summands = (ctypes.c_uint32 * 2)(9, 6)
        lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(7),
                                ctypes.c_uint32(2), hpwl_y_summands,
                                ctypes.c_uint8(1))

        # hpwl = hpwl_x + hpwl_y
        hpwl_summands = (ctypes.c_uint32 * 2)(8, 9)
        lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(10),
                                ctypes.c_uint32(2), hpwl_summands,
                                ctypes.c_uint8(1))

        sopts = _SolveOpts(seed=42, max_conflicts=200, max_restarts=5000)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        assert sr == 0

        x0 = lib.solver_get_value(ctx, ctypes.c_uint32(0))
        y0 = lib.solver_get_value(ctx, ctypes.c_uint32(1))
        x1 = lib.solver_get_value(ctx, ctypes.c_uint32(2))
        y1 = lib.solver_get_value(ctx, ctypes.c_uint32(3))
        hpwl = lib.solver_get_value(ctx, ctypes.c_uint32(10))

        expected_hpwl = abs(x0 - x1) + abs(y0 - y1)
        assert hpwl == expected_hpwl, \
            f"HPWL mismatch: got {hpwl}, expected {expected_hpwl} " \
            f"(pos: ({x0},{y0}), ({x1},{y1}))"
        _cleanup(lib, ba)
