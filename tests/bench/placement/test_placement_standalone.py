#!/usr/bin/env python3
"""Standalone tests for placement propagators (no pytest dependency).

Run directly:
    ZSP_SOLVER_PATH=build python3 tests/bench/placement/test_placement_standalone.py
"""
from __future__ import annotations

import ctypes
import os
import sys
import traceback
from pathlib import Path

_ROOT = Path(__file__).parent.parent.parent.parent
if str(_ROOT / "src") not in sys.path:
    sys.path.insert(0, str(_ROOT / "src"))


def _get_lib():
    from dv_solve.lib import _load_lib
    lib = _load_lib()
    if lib is None:
        print("SKIP: native solver library not built")
        sys.exit(0)
    return lib


def _make_ctx(lib, var_specs, buf_size=1 << 20):
    sp_buf = (ctypes.c_uint8 * 65536)()
    sp = lib.solve_problem_init(sp_buf, 65536)
    assert sp is not None

    for vid, w, s, lo, hi in var_specs:
        lib.problem_add_var(sp, ctypes.c_uint32(vid),
                            ctypes.c_uint8(w), ctypes.c_uint8(s),
                            ctypes.c_int64(lo), ctypes.c_int64(hi))

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


class _SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed", ctypes.c_uint64),
        ("max_conflicts", ctypes.c_uint32),
        ("max_restarts", ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad", ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


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


def test_min_of_3():
    lib = _get_lib()
    ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, [
        (0, 32, 0, 0, 10),
        (1, 32, 0, 2, 8),
        (2, 32, 0, 4, 10),
        (3, 32, 0, 1, 5),
    ])
    ops = (ctypes.c_uint32 * 3)(1, 2, 3)
    ref = lib.prop_add_min_of_n_32(ctx, ctypes.c_uint32(0),
                                    ctypes.c_uint32(3), ops,
                                    ctypes.c_uint8(1))
    assert ref != 0xFFFFFFFF, "MinOfN alloc failed"

    sopts = _SolveOpts(seed=42, max_conflicts=100, max_restarts=1000)
    sr = lib.solver_solve(ctx, ctypes.byref(sopts))
    assert sr == 0, f"Expected SOLVE_OK, got {sr}"

    r = lib.solver_get_value(ctx, 0)
    a = lib.solver_get_value(ctx, 1)
    b = lib.solver_get_value(ctx, 2)
    c = lib.solver_get_value(ctx, 3)
    assert r == min(a, b, c), f"r={r}, min({a},{b},{c})={min(a,b,c)}"
    lib.zsp_block_alloc_destroy(ba)


def test_max_of_3():
    lib = _get_lib()
    ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, [
        (0, 32, 0, 0, 20),
        (1, 32, 0, 2, 8),
        (2, 32, 0, 4, 10),
        (3, 32, 0, 1, 15),
    ])
    ops = (ctypes.c_uint32 * 3)(1, 2, 3)
    ref = lib.prop_add_max_of_n_32(ctx, ctypes.c_uint32(0),
                                    ctypes.c_uint32(3), ops,
                                    ctypes.c_uint8(1))
    assert ref != 0xFFFFFFFF

    sopts = _SolveOpts(seed=42, max_conflicts=100, max_restarts=1000)
    sr = lib.solver_solve(ctx, ctypes.byref(sopts))
    assert sr == 0

    r = lib.solver_get_value(ctx, 0)
    a = lib.solver_get_value(ctx, 1)
    b = lib.solver_get_value(ctx, 2)
    c = lib.solver_get_value(ctx, 3)
    assert r == max(a, b, c), f"r={r}, max({a},{b},{c})={max(a,b,c)}"
    lib.zsp_block_alloc_destroy(ba)


def test_no_overlap_2_rects():
    lib = _get_lib()
    ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, [
        (0, 32, 0, 0, 5),
        (1, 32, 0, 0, 5),
        (2, 32, 0, 0, 0),
        (3, 32, 0, 0, 0),
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

    x0 = lib.solver_get_value(ctx, 0)
    x1 = lib.solver_get_value(ctx, 1)
    assert x0 + 5 <= x1 or x1 + 5 <= x0, f"Overlap: x0={x0}, x1={x1}"
    lib.zsp_block_alloc_destroy(ba)


def test_no_overlap_3_rects_with_halo():
    lib = _get_lib()
    ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, [
        (0, 32, 0, 0, 40), (1, 32, 0, 0, 40), (2, 32, 0, 0, 40),
        (3, 32, 0, 0, 40), (4, 32, 0, 0, 40), (5, 32, 0, 0, 40),
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

    positions = []
    for i in range(3):
        x = lib.solver_get_value(ctx, i)
        y = lib.solver_get_value(ctx, 3 + i)
        positions.append((x, y))

    for i in range(3):
        for j in range(i + 1, 3):
            ri, rj = rects[i], rects[j]
            xi, yi = positions[i]
            xj, yj = positions[j]
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
            assert x_sep or y_sep, f"Overlap: rect {i} at {positions[i]}, rect {j} at {positions[j]}"
    lib.zsp_block_alloc_destroy(ba)


def test_optimize_sum():
    lib = _get_lib()
    ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, [
        (0, 32, 0, 2, 20),
        (1, 32, 0, 1, 10),
        (2, 32, 0, 1, 10),
    ])
    summands = (ctypes.c_uint32 * 2)(1, 2)
    lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(0),
                            ctypes.c_uint32(2), summands, ctypes.c_uint8(1))

    opts = COptimizeOpts(seed=42, max_conflicts=100, max_restarts=1000,
                          max_rounds=50, time_limit_sec=5.0,
                          use_phase_save=1, max_shave_iters=500)
    result = COptimizeResult()

    rc = lib.solver_optimize(ctx, ctypes.c_uint32(0),
                              ctypes.byref(opts), ctypes.byref(result))
    assert rc == 0
    assert result.found == 1
    assert result.best_objective == 2, f"Expected 2, got {result.best_objective}"
    lib.zsp_block_alloc_destroy(ba)


def test_hpwl():
    lib = _get_lib()
    # Use width=31 to keep variables in tier-0 (32-bit unsigned gets promoted to tier-1)
    ctx, ba, ctx_buf, sp_buf = _make_ctx(lib, [
        (0, 31, 0, 0, 50), (1, 31, 0, 0, 50),
        (2, 31, 0, 0, 50), (3, 31, 0, 0, 50),
        (4, 31, 0, 0, 50), (5, 31, 0, 0, 50),
        (6, 31, 0, 0, 50), (7, 31, 0, 0, 50),
        (8, 31, 0, 0, 100), (9, 31, 0, 0, 100),
        (10, 31, 0, 0, 200),
    ])

    x_ops = (ctypes.c_uint32 * 2)(0, 2)
    lib.prop_add_min_of_n_32(ctx, ctypes.c_uint32(4), ctypes.c_uint32(2), x_ops, ctypes.c_uint8(1))
    lib.prop_add_max_of_n_32(ctx, ctypes.c_uint32(5), ctypes.c_uint32(2), x_ops, ctypes.c_uint8(1))

    y_ops = (ctypes.c_uint32 * 2)(1, 3)
    lib.prop_add_min_of_n_32(ctx, ctypes.c_uint32(6), ctypes.c_uint32(2), y_ops, ctypes.c_uint8(1))
    lib.prop_add_max_of_n_32(ctx, ctypes.c_uint32(7), ctypes.c_uint32(2), y_ops, ctypes.c_uint8(1))

    # xmax == hpwl_x + xmin
    hx_sum = (ctypes.c_uint32 * 2)(8, 4)
    lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(5), ctypes.c_uint32(2), hx_sum, ctypes.c_uint8(1))

    # ymax == hpwl_y + ymin
    hy_sum = (ctypes.c_uint32 * 2)(9, 6)
    lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(7), ctypes.c_uint32(2), hy_sum, ctypes.c_uint8(1))

    # hpwl == hpwl_x + hpwl_y
    h_sum = (ctypes.c_uint32 * 2)(8, 9)
    lib.prop_add_sum_eq_32(ctx, ctypes.c_uint32(10), ctypes.c_uint32(2), h_sum, ctypes.c_uint8(1))

    sopts = _SolveOpts(seed=42, max_conflicts=200, max_restarts=5000)
    sr = lib.solver_solve(ctx, ctypes.byref(sopts))
    assert sr == 0

    x0 = lib.solver_get_value(ctx, 0)
    y0 = lib.solver_get_value(ctx, 1)
    x1 = lib.solver_get_value(ctx, 2)
    y1 = lib.solver_get_value(ctx, 3)
    hpwl = lib.solver_get_value(ctx, 10)
    expected = abs(x0 - x1) + abs(y0 - y1)
    assert hpwl == expected, f"HPWL={hpwl}, expected={expected} ({x0},{y0}),({x1},{y1})"
    lib.zsp_block_alloc_destroy(ba)


def main():
    tests = [
        ("test_min_of_3", test_min_of_3),
        ("test_max_of_3", test_max_of_3),
        ("test_no_overlap_2_rects", test_no_overlap_2_rects),
        ("test_no_overlap_3_rects_with_halo", test_no_overlap_3_rects_with_halo),
        ("test_optimize_sum", test_optimize_sum),
        ("test_hpwl", test_hpwl),
    ]

    passed = 0
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  PASS: {name}")
            passed += 1
        except Exception as e:
            print(f"  FAIL: {name}: {e}")
            traceback.print_exc()
            failed += 1

    print(f"\n{passed} passed, {failed} failed out of {len(tests)} tests")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
