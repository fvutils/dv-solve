"""Placement benchmark harness.

Converts a JSON benchmark to a zuspec-solver problem, solves it, and
collects metrics (time to first solution, HPWL, backtracks, etc.).
"""
from __future__ import annotations

import ctypes
import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional


@dataclass
class PlacementResult:
    """Collected metrics from a single benchmark run."""
    name: str = ""
    n_macros: int = 0
    n_nets: int = 0
    feasible: bool = False
    hpwl: int = 0
    t_first_sec: float = 0.0
    t_total_sec: float = 0.0
    solver_result: int = -1
    positions: List[Dict[str, int]] = field(default_factory=list)


def _compute_hpwl(bench: Dict, positions: List[Dict[str, int]]) -> int:
    """Compute total HPWL from placed macro positions."""
    total = 0
    for net in bench["nets"]:
        xs, ys = [], []
        for pin in net["pins"]:
            mid = pin["macro_id"]
            pos = positions[mid]
            xs.append(pos["x"] + pin["pin_x"])
            ys.append(pos["y"] + pin["pin_y"])
        if xs:
            total += (max(xs) - min(xs)) + (max(ys) - min(ys))
    return total


def _validate_no_overlap(bench: Dict, positions: List[Dict[str, int]]) -> List[str]:
    """Check that no rectangles overlap (including halos)."""
    errors = []
    macros = bench["macros"]
    halos = bench.get("halos", [])

    for i in range(len(macros)):
        mi = macros[i]
        pi = positions[i]
        hi = halos[i] if i < len(halos) else {"left": 0, "right": 0, "top": 0, "bottom": 0}
        xi_lo = pi["x"] - hi["left"]
        xi_hi = pi["x"] + mi["width"] + hi["right"]
        yi_lo = pi["y"] - hi["top"]
        yi_hi = pi["y"] + mi["height"] + hi["bottom"]

        for j in range(i + 1, len(macros)):
            mj = macros[j]
            pj = positions[j]
            hj = halos[j] if j < len(halos) else {"left": 0, "right": 0, "top": 0, "bottom": 0}
            xj_lo = pj["x"] - hj["left"]
            xj_hi = pj["x"] + mj["width"] + hj["right"]
            yj_lo = pj["y"] - hj["top"]
            yj_hi = pj["y"] + mj["height"] + hj["bottom"]

            if xi_lo < xj_hi and xj_lo < xi_hi and yi_lo < yj_hi and yj_lo < yi_hi:
                errors.append(f"Overlap: macro {i} and {j}")

    return errors


def run_benchmark(bench_path: Path, time_budget_sec: float = 60.0,
                   optimize: bool = False,
                   use_cost_guided: bool = True) -> PlacementResult:
    """Run a single placement benchmark using the native solver.

    Builds the CSP from the JSON benchmark and runs the solver.
    When use_cost_guided=True, uses HPWL CostGuided value selection.
    """
    from dv_solve.lib import _load_lib

    lib = _load_lib()
    if lib is None:
        raise RuntimeError("Native solver library not available")

    with open(bench_path) as f:
        bench = json.load(f)

    result = PlacementResult()
    result.name = bench.get("name", bench_path.stem)
    macros = bench["macros"]
    nets = bench["nets"]
    canvas = bench["canvas"]
    halos = bench.get("halos", [])
    result.n_macros = len(macros)
    result.n_nets = len(nets)

    # Build solver problem using the C builder API
    # Use larger builder pool for problems with many constraints
    builder_size = 65536 if len(bench.get('spacing_rules', [])) > 0 else 0
    builder = lib.builder_create(builder_size, None)
    if not builder:
        raise RuntimeError("builder_create failed")

    n = len(macros)
    cw, ch = canvas["width"], canvas["height"]

    # Variable layout:
    #   var 0..n-1 = x positions
    #   var n..2n-1 = y positions
    #   var 2n..3n-1 = (if optimize) per-net HPWL_x  -- will be added later
    for i, m in enumerate(macros):
        w, h = m["width"], m["height"]
        if m["preplaced"]:
            x_lo = x_hi = m["x"]
            y_lo = y_hi = m["y"]
        else:
            x_lo, x_hi = 0, max(0, cw - w)
            y_lo, y_hi = 0, max(0, ch - h)
        lib.builder_add_var(builder, ctypes.c_uint32(i),
                            ctypes.c_uint8(32), ctypes.c_uint8(0),
                            ctypes.c_int64(x_lo), ctypes.c_int64(x_hi))
        lib.builder_add_var(builder, ctypes.c_uint32(n + i),
                            ctypes.c_uint8(32), ctypes.c_uint8(0),
                            ctypes.c_int64(y_lo), ctypes.c_int64(y_hi))

    # Add non-overlap constraints using DisjClause decomposition
    # For each pair (i,j), at least one of:
    #   xi + wi + halo_r_i + halo_l_j <= xj
    #   xj + wj + halo_r_j + halo_l_i <= xi
    #   yi + hi + halo_b_i + halo_t_j <= yj
    #   yj + hj + halo_b_j + halo_t_i <= yi
    #
    # We use the C API directly via builder for standard constraints,
    # then add NoOverlap2D propagator post-compile.

    # ── Forbidden regions: model as fixed-position virtual rectangles ──
    # Each forbidden region becomes a variable pair (x, y) fixed at the
    # region's position.  These are added to NoOverlap2D propagators so
    # no macro can overlap them.
    forbidden = bench.get("forbidden_regions", [])
    n_forbidden = len(forbidden)
    # Variable IDs for forbidden regions: after macro vars (2*n ... 2*n + 2*n_forbidden - 1)
    fr_base_x = 2 * n  # x vars for forbidden regions
    fr_base_y = 2 * n + n_forbidden  # y vars for forbidden regions
    for fi, fr in enumerate(forbidden):
        fx, fy, fw, fh = fr["x"], fr["y"], fr["width"], fr["height"]
        # Fixed-position variables (lo == hi)
        lib.builder_add_var(builder, ctypes.c_uint32(fr_base_x + fi),
                            ctypes.c_uint8(32), ctypes.c_uint8(0),
                            ctypes.c_int64(fx), ctypes.c_int64(fx))
        lib.builder_add_var(builder, ctypes.c_uint32(fr_base_y + fi),
                            ctypes.c_uint8(32), ctypes.c_uint8(0),
                            ctypes.c_int64(fy), ctypes.c_int64(fy))

    # ── Symmetry breaking for identical macros ──
    # Only apply when no spacing rules reference the group (to avoid
    # over-constraining).
    groups = {}
    for i, m in enumerate(macros):
        g = m.get("group_id", -1)
        if g >= 0:
            groups.setdefault(g, []).append(i)

    spacing_rules = bench.get("spacing_rules", [])
    groups_with_spacing = set()
    for sr in spacing_rules:
        groups_with_spacing.add(sr["from_group"])
        groups_with_spacing.add(sr["to_group"])

    for g, members in groups.items():
        if len(members) < 2 or g in groups_with_spacing:
            continue
        members.sort()
        for k in range(len(members) - 1):
            mi, mj = members[k], members[k + 1]
            e_xi = lib.builder_expr_var(builder, ctypes.c_uint32(mi))
            e_xj = lib.builder_expr_var(builder, ctypes.c_uint32(mj))
            e_le = lib.builder_expr_binary(builder, ctypes.c_uint32(13),
                                            e_xi, e_xj)
            lib.builder_add_constraint(builder, e_le)

    # Spacing rules are enforced post-compile as per-rule NoOverlap2D
    # propagators (see below).

    # Source group: all position variables
    all_var_ids = list(range(2 * n + 2 * n_forbidden))
    arr = (ctypes.c_uint32 * len(all_var_ids))(*all_var_ids)
    lib.builder_add_source(builder, ctypes.c_uint32(len(all_var_ids)), arr)

    # Finalize the builder
    sp_size = ctypes.c_size_t(0)
    sp = lib.builder_finalize(builder, ctypes.byref(sp_size))
    if not sp:
        lib.builder_destroy(builder)
        raise RuntimeError("builder_finalize failed")

    # Create solver context
    ctx_buf_size = 1 << 24 if n > 64 else 1 << 22  # 16 MiB for large N
    ba = lib.zsp_block_alloc_create(None, ctx_buf_size)
    ctx_buf = (ctypes.c_uint8 * ctx_buf_size)()
    ctx = lib.solver_create(ctx_buf, ctx_buf_size, ba)

    rc = lib.solver_compile(ctx, sp)
    if rc < 0:  # negative = error, positive = uncompiled constraints (OK)
        lib.builder_free_problem(builder, sp, sp_size.value)
        lib.builder_destroy(builder)
        lib.zsp_block_alloc_destroy(ba)
        result.solver_result = rc
        return result

    # Add NoOverlap2D propagator
    # Build RectSpec array
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

    # Build per-macro RectSpec entries (including forbidden region rects)
    all_rects = list(range(n))
    rect_specs = {}
    for mi in all_rects:
        m = macros[mi]
        h = halos[mi] if mi < len(halos) else {"left": 0, "right": 0, "top": 0, "bottom": 0}
        rect_specs[mi] = (mi, n + mi, m["width"], m["height"],
                          h["left"], h["right"], h["top"], h["bottom"])
    # Add forbidden regions as virtual rects
    for fi, fr in enumerate(forbidden):
        virt_id = n + fi  # virtual macro index
        rect_specs[virt_id] = (fr_base_x + fi, fr_base_y + fi,
                               fr["width"], fr["height"], 0, 0, 0, 0)
        all_rects.append(virt_id)

    # For N <= 64, one propagator covers all pairs.
    # For N > 64, split into half-blocks of 32 and create propagators
    # for all block-pair combinations so every (i,j) pair is covered.
    MAX_PROP = 64
    HALF = MAX_PROP // 2

    if n <= MAX_PROP:
        prop_groups = [all_rects]
    else:
        raw_blocks = [all_rects[i:i + HALF] for i in range(0, n, HALF)]
        prop_groups = []
        nb = len(raw_blocks)
        for bi in range(nb):
            for bj in range(bi, nb):
                combined = raw_blocks[bi] + (raw_blocks[bj] if bj != bi else [])
                if len(combined) >= 2:
                    prop_groups.append(combined)

    for group in prop_groups:
        if len(group) < 2:
            continue
        rects_arr = (CRectSpec * len(group))()
        for ci, mi in enumerate(group):
            spec = rect_specs[mi]
            rects_arr[ci].x_id = spec[0]
            rects_arr[ci].y_id = spec[1]
            rects_arr[ci].width = spec[2]
            rects_arr[ci].height = spec[3]
            rects_arr[ci].halo_l = spec[4]
            rects_arr[ci].halo_r = spec[5]
            rects_arr[ci].halo_t = spec[6]
            rects_arr[ci].halo_b = spec[7]

        lib.prop_add_no_overlap_2d(ctx, ctypes.c_uint32(len(group)),
                                    rects_arr, ctypes.c_uint8(2))

    # Add per-rule NoOverlap2D propagators.
    # Each spacing rule gets its own propagator containing the macros
    # from both groups, with that rule's specific extra halo.
    # This avoids the over-approximation of applying max halo globally.
    for sr in spacing_rules:
        from_macros = groups.get(sr["from_group"], [])
        to_macros = groups.get(sr["to_group"], [])
        if not from_macros or not to_macros:
            continue
        min_dist = sr["min_distance"]
        direction = sr["direction"]
        extra_x = min_dist if direction in ("x", "both") else 0
        extra_y = min_dist if direction in ("y", "both") else 0

        # Collect unique macros from both groups
        rule_macros = sorted(set(from_macros) | set(to_macros))
        if len(rule_macros) < 2:
            continue

        # Chunk if needed (max 64 per propagator)
        for chunk_start in range(0, len(rule_macros), MAX_PROP):
            chunk = rule_macros[chunk_start:chunk_start + MAX_PROP]
            if len(chunk) < 2:
                continue
            sr_rects = (CRectSpec * len(chunk))()
            for ci, mi in enumerate(chunk):
                m = macros[mi]
                h = halos[mi] if mi < len(halos) else {"left": 0, "right": 0, "top": 0, "bottom": 0}
                sr_rects[ci].x_id = mi
                sr_rects[ci].y_id = n + mi
                sr_rects[ci].width = m["width"]
                sr_rects[ci].height = m["height"]
                sr_rects[ci].halo_l = h["left"] + extra_x // 2
                sr_rects[ci].halo_r = h["right"] + (extra_x + 1) // 2
                sr_rects[ci].halo_t = h["top"] + extra_y // 2
                sr_rects[ci].halo_b = h["bottom"] + (extra_y + 1) // 2
            lib.prop_add_no_overlap_2d(ctx, ctypes.c_uint32(len(chunk)),
                                        sr_rects, ctypes.c_uint8(2))

    # Build HPWL context for CostGuided / LNS when nets are present
    hpwl_cleanup = None
    hpwl_ctx_ref = None  # keep a reference for LNS
    if use_cost_guided and nets:
        class CCostGuidedPin(ctypes.Structure):
            _fields_ = [
                ("macro_id", ctypes.c_uint32),
                ("offset_x", ctypes.c_int32),
                ("offset_y", ctypes.c_int32),
            ]
        class CCostGuidedNet(ctypes.Structure):
            _fields_ = [
                ("n_pins", ctypes.c_uint32),
                ("pins", ctypes.POINTER(CCostGuidedPin)),
            ]
        class CCostGuidedMacro(ctypes.Structure):
            _fields_ = [
                ("x_var_id", ctypes.c_uint32),
                ("y_var_id", ctypes.c_uint32),
            ]
        class CHPWLCostCtx(ctypes.Structure):
            _fields_ = [
                ("n_macros", ctypes.c_uint32),
                ("n_nets", ctypes.c_uint32),
                ("macros", ctypes.POINTER(CCostGuidedMacro)),
                ("nets", ctypes.POINTER(CCostGuidedNet)),
                ("macro_net_count", ctypes.POINTER(ctypes.c_uint32)),
                ("macro_net_ids", ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32))),
                ("macro_widths", ctypes.POINTER(ctypes.c_int32)),
                ("macro_heights", ctypes.POINTER(ctypes.c_int32)),
            ]
        class CLNSOpts(ctypes.Structure):
            _fields_ = [
                ("max_iterations", ctypes.c_uint32),
                ("time_limit_sec", ctypes.c_double),
                ("neighborhood_size", ctypes.c_uint32),
                ("subproblem_conflicts", ctypes.c_uint32),
                ("seed", ctypes.c_uint64),
            ]
        class CLNSResult(ctypes.Structure):
            _fields_ = [
                ("improved", ctypes.c_int),
                ("initial_hpwl", ctypes.c_int64),
                ("best_hpwl", ctypes.c_int64),
                ("iterations", ctypes.c_uint32),
                ("improvements", ctypes.c_uint32),
                ("elapsed_sec", ctypes.c_double),
            ]

        c_macros = (CCostGuidedMacro * n)()
        for i in range(n):
            c_macros[i].x_var_id = i
            c_macros[i].y_var_id = n + i

        all_pin_arrays = []
        c_nets = (CCostGuidedNet * len(nets))()
        for ni, net in enumerate(nets):
            pins_data = net["pins"]
            c_pins = (CCostGuidedPin * len(pins_data))()
            for pi, pin in enumerate(pins_data):
                c_pins[pi].macro_id = pin["macro_id"]
                c_pins[pi].offset_x = pin["pin_x"]
                c_pins[pi].offset_y = pin["pin_y"]
            all_pin_arrays.append(c_pins)
            c_nets[ni].n_pins = len(pins_data)
            c_nets[ni].pins = c_pins

        c_widths = (ctypes.c_int32 * n)()
        c_heights = (ctypes.c_int32 * n)()
        for i in range(n):
            c_widths[i] = macros[i]["width"]
            c_heights[i] = macros[i]["height"]

        hpwl_ctx = CHPWLCostCtx()
        hpwl_ctx.n_macros = n
        hpwl_ctx.n_nets = len(nets)
        hpwl_ctx.macros = c_macros
        hpwl_ctx.nets = c_nets
        hpwl_ctx.macro_net_count = None
        hpwl_ctx.macro_net_ids = None
        hpwl_ctx.macro_widths = c_widths
        hpwl_ctx.macro_heights = c_heights

        hpwl_cleanup = (hpwl_ctx, c_macros, c_nets, all_pin_arrays,
                         c_widths, c_heights)

        # Run LNS: greedy initial placement + net-based neighborhood search
        out_pos = (ctypes.c_int32 * (2 * n))()
        lr = CLNSResult()
        # Scale LNS iterations with time budget
        lns_iters = max(100, int(time_budget_sec * 50))
        lo = CLNSOpts(max_iterations=lns_iters,
                      time_limit_sec=time_budget_sec * 0.9,
                      neighborhood_size=4,
                      subproblem_conflicts=1000, seed=42)

        t0 = time.monotonic()
        lrc = lib.solver_lns_optimize(ctx, ctypes.byref(hpwl_ctx),
                                       ctypes.byref(lo), out_pos,
                                       ctypes.byref(lr))
        t1 = time.monotonic()

        if lrc == 0:
            result.feasible = True
            result.solver_result = 0
            result.positions = [{"x": out_pos[2*i], "y": out_pos[2*i+1]}
                                for i in range(n)]
            result.hpwl = _compute_hpwl(bench, result.positions)
            result.t_first_sec = t1 - t0
            result.t_total_sec = t1 - t0

            errors = _validate_no_overlap(bench, result.positions)
            if errors:
                print(f"  WARNING: {len(errors)} overlap violations!")
                for e in errors[:5]:
                    print(f"    {e}")

    # If LNS didn't run or failed, fall back to systematic solver
    if not result.feasible:
        class CSolveOpts(ctypes.Structure):
            _fields_ = [
                ("seed", ctypes.c_uint64),
                ("max_conflicts", ctypes.c_uint32),
                ("max_restarts", ctypes.c_uint32),
                ("use_phase_save", ctypes.c_uint8),
                ("_pad", ctypes.c_uint8 * 3),
                ("max_shave_iters", ctypes.c_uint32),
            ]

        t0 = time.monotonic()
        sopts = CSolveOpts(seed=42, max_conflicts=5000, max_restarts=500,
                            use_phase_save=0, max_shave_iters=0)
        sr = lib.solver_solve(ctx, ctypes.byref(sopts))
        t1 = time.monotonic()
        result.solver_result = sr
        result.t_first_sec = t1 - t0
        result.t_total_sec = t1 - t0

        if sr == 0:
            result.feasible = True
            positions = []
            for i in range(n):
                x = lib.solver_get_value(ctx, ctypes.c_uint32(i))
                y = lib.solver_get_value(ctx, ctypes.c_uint32(n + i))
                positions.append({"x": x, "y": y})
            result.positions = positions
            result.hpwl = _compute_hpwl(bench, positions)

            errors = _validate_no_overlap(bench, positions)
            if errors:
                print(f"  WARNING: {len(errors)} overlap violations!")
                for e in errors[:5]:
                    print(f"    {e}")

    # Cleanup
    if hpwl_cleanup:
        lib.hpwl_cost_ctx_destroy(ctypes.byref(hpwl_cleanup[0]))
    lib.builder_free_problem(builder, sp, sp_size.value)
    lib.builder_destroy(builder)
    lib.zsp_block_alloc_destroy(ba)

    return result


def run_suite(bench_dir: Path, time_budget_sec: float = 60.0) -> List[PlacementResult]:
    """Run all JSON benchmarks in a directory."""
    results = []
    for path in sorted(bench_dir.glob("*.json")):
        print(f"Running {path.name}...")
        try:
            r = run_benchmark(path, time_budget_sec)
            status = "FEASIBLE" if r.feasible else f"result={r.solver_result}"
            print(f"  {r.name}: {status}, HPWL={r.hpwl}, time={r.t_first_sec:.3f}s")
            results.append(r)
        except Exception as e:
            print(f"  ERROR: {e}")
            results.append(PlacementResult(name=path.stem))
    return results


def print_results_table(results: List[PlacementResult]) -> None:
    """Print results in a readable table."""
    print(f"\n{'Name':<8} {'N':>5} {'Nets':>5} {'Feasible':>8} "
          f"{'HPWL':>10} {'T_first(s)':>10} {'T_total(s)':>10}")
    print("-" * 65)
    for r in results:
        print(f"{r.name:<8} {r.n_macros:>5} {r.n_nets:>5} "
              f"{'YES' if r.feasible else 'NO':>8} "
              f"{r.hpwl:>10} {r.t_first_sec:>10.3f} {r.t_total_sec:>10.3f}")
