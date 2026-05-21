"""Performance benchmarks for the two reference examples.

T1 (Pad Config / AllDifferent):
  - 6 vars, two consecutive triples, all distinct
  - Measures: elab time (build + compile), per-solve time

T2/T3 (Exhaustive Power State / Thermal):
  - State graph construction (~300 states for clock domain)
  - BFS path query time
  - Per-step action solve time
"""
from __future__ import annotations

import ctypes
import time
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

from dv_solve.state_graph import (
    FieldDescriptor, TransitionDescriptor,
    StateGraphBuilder,
)
from dv_solve.structural_solver import (
    solve_state_chain_graph_guided,
)


EXPR_NULL     = 0xFFFF_FFFF
SOLVE_OK      = 0
BIN_EQ        = 10
BIN_ADD       = 0
_CTX_BUF_SIZE = 1 << 20


class _SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",           ctypes.c_uint64),
        ("max_conflicts",  ctypes.c_uint32),
        ("max_restarts",   ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad",           ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


def _setup_lib(lib):
    c = ctypes
    for fn, rt, at in [
        ("zsp_block_alloc_create", c.c_void_p, [c.c_void_p, c.c_size_t]),
        ("zsp_block_alloc_destroy", None, [c.c_void_p]),
        ("solve_problem_init", c.c_void_p, [c.c_void_p, c.c_size_t]),
        ("problem_add_var", c.c_uint32, [c.c_void_p, c.c_uint32, c.c_uint8, c.c_uint8, c.c_int64, c.c_int64]),
        ("problem_add_constraint", c.c_uint32, [c.c_void_p, c.c_uint32]),
        ("problem_add_all_different", c.c_uint32, [c.c_void_p, c.c_uint32, c.c_void_p]),
        ("expr_const", c.c_uint32, [c.c_void_p, c.c_int64, c.c_uint8]),
        ("expr_var", c.c_uint32, [c.c_void_p, c.c_uint32]),
        ("expr_binary", c.c_uint32, [c.c_void_p, c.c_int32, c.c_uint32, c.c_uint32]),
        ("solver_create", c.c_void_p, [c.c_void_p, c.c_size_t, c.c_void_p]),
        ("solver_compile", c.c_int, [c.c_void_p, c.c_void_p]),
        ("solver_solve", c.c_int, [c.c_void_p, c.c_void_p]),
        ("solver_get_value", c.c_int64, [c.c_void_p, c.c_uint32]),
    ]:
        getattr(lib, fn).restype = rt
        getattr(lib, fn).argtypes = at


# ================================================================== #
# T1: Pad Config -- AllDifferent (6 vars, two consecutive triples)    #
# ================================================================== #

class TestT1PadConfig:
    """T1: 6 vars from [0..7], two consecutive triples, all-different.

    To express v[i+1] == v[i] + 1, we use a const-variable `one` with
    domain [1,1] so the compiler sees `var == var + var` (which it handles).
    """

    def _build_and_solve(self, lib, seed):
        """Build, compile, solve. Returns (elab_us, solve_us, values, result)."""
        t0 = time.perf_counter()

        buf = (ctypes.c_uint8 * 65536)()
        sp = lib.solve_problem_init(buf, 65536)

        # 6 primary vars [0..7]
        for i in range(6):
            lib.problem_add_var(sp, i, 8, 0, 0, 7)

        # Const-variable for 1 (domain=[1,1])
        ONE_ID = 6
        lib.problem_add_var(sp, ONE_ID, 8, 0, 1, 1)

        # Temp vars for sums (v[i] + one)
        # Triple 1: v1 == v0+one, v2 == v1+one
        # Triple 2: v4 == v3+one, v5 == v4+one
        tmp_id = 7
        for group_start in [0, 3]:
            for j in range(2):
                a_id = group_start + j
                b_id = group_start + j + 1
                lib.problem_add_var(sp, tmp_id, 8, 0, 0, 8)

                # tmp == a + one
                va = lib.expr_var(sp, a_id)
                vone = lib.expr_var(sp, ONE_ID)
                sum_ref = lib.expr_binary(sp, BIN_ADD, va, vone)
                vtmp = lib.expr_var(sp, tmp_id)
                lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_EQ, vtmp, sum_ref))

                # tmp == b
                vb = lib.expr_var(sp, b_id)
                lib.problem_add_constraint(sp, lib.expr_binary(sp, BIN_EQ, vtmp, vb))

                tmp_id += 1

        # AllDifferent on the 6 primary vars
        vids = (ctypes.c_uint32 * 6)(*range(6))
        lib.problem_add_all_different(sp, 6, vids)

        # Compile
        ba = lib.zsp_block_alloc_create(None, _CTX_BUF_SIZE)
        ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
        ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
        rc = lib.solver_compile(ctx, sp)
        t1 = time.perf_counter()
        elab_us = (t1 - t0) * 1e6

        if rc < 0:
            lib.zsp_block_alloc_destroy(ba)
            return elab_us, 0, None, rc

        # Solve
        t2 = time.perf_counter()
        opts = _SolveOpts(seed=seed)
        result = lib.solver_solve(ctx, ctypes.byref(opts))
        t3 = time.perf_counter()
        solve_us = (t3 - t2) * 1e6

        values = None
        if result == SOLVE_OK:
            values = [lib.solver_get_value(ctx, i) for i in range(6)]

        lib.zsp_block_alloc_destroy(ba)
        return elab_us, solve_us, values, result

    def test_t1_correctness(self, libzsp):
        """Verify: two disjoint consecutive triples from [0..7]."""
        _setup_lib(libzsp)
        elab_us, solve_us, vals, result = self._build_and_solve(libzsp, seed=42)
        assert result == SOLVE_OK, f"T1 should be SAT (rc={result})"
        assert len(set(vals)) == 6, f"Not all distinct: {vals}"
        assert vals[1] == vals[0] + 1 and vals[2] == vals[1] + 1, \
            f"Triple 1 not consecutive: {vals[:3]}"
        assert vals[4] == vals[3] + 1 and vals[5] == vals[4] + 1, \
            f"Triple 2 not consecutive: {vals[3:6]}"

    def test_t1_performance(self, libzsp):
        """Measure elab and per-solve times over 50 seeds."""
        _setup_lib(libzsp)
        elab_times = []
        solve_times = []

        for seed in range(1, 51):
            e, s, vals, result = self._build_and_solve(libzsp, seed)
            assert result == SOLVE_OK, f"seed {seed} failed"
            elab_times.append(e)
            solve_times.append(s)

        avg_elab = sum(elab_times) / len(elab_times)
        avg_solve = sum(solve_times) / len(solve_times)
        p50_solve = sorted(solve_times)[25]
        min_solve = min(solve_times)
        max_solve = max(solve_times)

        print(f"\n{'='*60}")
        print(f"T1: Pad Config (AllDifferent, 6 vars, consecutive triples)")
        print(f"{'='*60}")
        print(f"  Elab (build+compile):  avg {avg_elab:.0f} us")
        print(f"  Solve (per-solution):  avg {avg_solve:.0f} us  "
              f"p50 {p50_solve:.0f} us  min {min_solve:.0f} us  max {max_solve:.0f} us")
        print(f"  Target: < 100 us per solve")
        print(f"{'='*60}")

        assert avg_solve < 1000, f"T1 avg solve {avg_solve:.0f} us exceeds 10x target"


# ================================================================== #
# T2: Clock Domain State Graph (~300 nodes)                           #
# ================================================================== #

class TestT2ClockDomainGraph:
    """T2: Clock domain FSM -- enumerate valid states, build graph, BFS."""

    def test_t2_graph_construction_and_query(self):
        """Build clock domain graph; report construction + query timing."""
        fields = [
            FieldDescriptor("pll_en", 0, 1),
            FieldDescriptor("rosc_en", 0, 1),
            FieldDescriptor("bypass_en", 0, 1),
            FieldDescriptor("pll_div", 0, 3),   # 0..3 encodes {1,2,4,8}
            FieldDescriptor("rosc_div", 0, 3),
            FieldDescriptor("bypass_div", 0, 3),
        ]

        def invariant(v):
            pll_en, rosc_en, bypass_en = v[0], v[1], v[2]
            bypass_div = v[5]
            if not (pll_en or rosc_en or bypass_en):
                return False
            if bypass_en and bypass_div != 0:
                return False
            return True

        def initial_pred(v):
            return v == (0, 0, 1, 0, 0, 0)

        def toggle_pll(v):
            nv = list(v); nv[0] = 1 - nv[0]; return [tuple(nv)]
        def toggle_rosc(v):
            nv = list(v); nv[1] = 1 - nv[1]; return [tuple(nv)]
        def toggle_bypass(v):
            nv = list(v); nv[2] = 1 - nv[2]; return [tuple(nv)]
        def change_pll_div(v):
            return [tuple(v[:3] + (d,) + v[4:]) for d in range(4) if d != v[3]]
        def change_rosc_div(v):
            return [tuple(v[:4] + (d,) + v[5:]) for d in range(4) if d != v[4]]

        transitions = [
            TransitionDescriptor(0, toggle_pll),
            TransitionDescriptor(1, toggle_rosc),
            TransitionDescriptor(2, toggle_bypass),
            TransitionDescriptor(3, change_pll_div),
            TransitionDescriptor(4, change_rosc_div),
        ]

        t0 = time.perf_counter()
        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=transitions,
            state_invariant=invariant,
            initial_predicate=initial_pred,
        )
        g = builder.build()
        t1 = time.perf_counter()
        build_ms = (t1 - t0) * 1000

        # BFS: initial -> (pll=1, rosc=1, bypass=0, divs=0,0,0)
        target = (1, 1, 0, 0, 0, 0)
        goal = {g._value_to_index[target]}
        t2 = time.perf_counter()
        path = g.find_path({g.initial_index}, goal)
        t3 = time.perf_counter()
        query_us = (t3 - t2) * 1e6

        # Multiple BFS queries for timing
        query_times = []
        targets = [(1, 0, 0, 2, 0, 0), (0, 1, 0, 0, 3, 0), (1, 1, 1, 1, 1, 0)]
        for tgt in targets:
            if tgt in g._value_to_index:
                gset = {g._value_to_index[tgt]}
                ts = time.perf_counter()
                g.find_path({g.initial_index}, gset)
                te = time.perf_counter()
                query_times.append((te - ts) * 1e6)

        avg_query = sum(query_times) / len(query_times) if query_times else 0

        print(f"\n{'='*60}")
        print(f"T2: Clock Domain FSM State Graph")
        print(f"{'='*60}")
        print(f"  Nodes: {len(g.nodes)}  Edges: {len(g.edges)}")
        print(f"  Graph construction:  {build_ms:.1f} ms")
        print(f"  BFS query (first):   {query_us:.0f} us  ({len(path)} steps)")
        print(f"  BFS query (avg):     {avg_query:.0f} us")
        print(f"  Target: construction < 500 ms, query < 1 ms")
        print(f"{'='*60}")

        assert len(g.nodes) > 100
        assert build_ms < 5000


# ================================================================== #
# T3: Thermal Throttle 6-Step Chain                                    #
# ================================================================== #

class TestT3ThermalChain:
    """T3: Build thermal graph, infer 0->3->0, solve each step."""

    def test_t3_end_to_end(self, libzsp):
        """Full pipeline: graph + BFS + 6 action solves. Timing report."""
        _setup_lib(libzsp)
        lib = libzsp

        fields = [FieldDescriptor("level", 0, 3)]
        def step(values):
            v = values[0]
            nexts = []
            if v + 1 <= 3: nexts.append((v + 1,))
            if v - 1 >= 0: nexts.append((v - 1,))
            return nexts

        t0 = time.perf_counter()
        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[TransitionDescriptor(0, step)],
            initial_predicate=lambda v: v[0] == 0,
        )
        g = builder.build()
        t_graph = time.perf_counter()

        start = {g._value_to_index[(0,)]}
        wp1 = {g._value_to_index[(3,)]}
        wp2 = {g._value_to_index[(0,)]}
        t_bfs0 = time.perf_counter()
        actions = solve_state_chain_graph_guided(g, [start, wp1, wp2])
        t_bfs1 = time.perf_counter()

        assert len(actions) == 6

        solve_times = []
        for act in actions:
            buf = (ctypes.c_uint8 * 65536)()
            sp = lib.solve_problem_init(buf, 65536)
            prev, nxt = act.prev_state_values[0], act.next_state_values[0]
            lib.problem_add_var(sp, 0, 8, 0, prev, prev)
            lib.problem_add_var(sp, 1, 8, 0, nxt, nxt)
            lib.problem_add_var(sp, 2, 8, 1, -1, 1)

            ba = lib.zsp_block_alloc_create(None, _CTX_BUF_SIZE)
            ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
            ctx = lib.solver_create(ctx_buf, _CTX_BUF_SIZE, ba)
            lib.solver_compile(ctx, sp)

            ts = time.perf_counter()
            opts = _SolveOpts(seed=42)
            result = lib.solver_solve(ctx, ctypes.byref(opts))
            te = time.perf_counter()
            assert result == SOLVE_OK
            solve_times.append((te - ts) * 1e6)
            lib.zsp_block_alloc_destroy(ba)

        t_end = time.perf_counter()
        graph_ms = (t_graph - t0) * 1000
        bfs_us = (t_bfs1 - t_bfs0) * 1e6
        avg_solve = sum(solve_times) / len(solve_times)
        total_ms = (t_end - t0) * 1000

        print(f"\n{'='*60}")
        print(f"T3: Thermal Throttle 6-Step Chain (graph-guided)")
        print(f"{'='*60}")
        print(f"  Graph construction:    {graph_ms:.3f} ms")
        print(f"  BFS (2 queries):       {bfs_us:.0f} us")
        print(f"  Per-step solve (avg):  {avg_solve:.0f} us")
        print(f"  Total end-to-end:      {total_ms:.2f} ms")
        print(f"  Target: < 10 ms total")
        print(f"{'='*60}")

        assert total_ms < 100
