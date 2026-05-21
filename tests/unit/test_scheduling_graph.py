"""Unit tests for schedule-block DAG analysis (Phase B3).

Tests:
- Linear schedule (A -> B -> C)
- Parallel schedule (A, B unrelated)
- Diamond schedule (A -> B, A -> C, B -> D, C -> D)
- Stream concurrent group
- Cycle detection
- Sequential/concurrent conflict
- Mutex pairs at same level
- State write exclusion
- Inferred action integration
- 8-action mixed schedule
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

from dv_solve.scheduling_graph import (
    SchedulingGraph,
    ScheduleConflictError,
    ScheduleEdge,
    ExecutionUnit,
    StagedPlan,
)


class TestSchedulingGraph:

    def test_linear_schedule(self):
        """A -> B -> C (buffer binds); produces 3 stages."""
        g = SchedulingGraph()
        for i in range(3):
            g.add_action(i)
        g.add_edge(0, 1, "sequential", "buffer_bind")
        g.add_edge(1, 2, "sequential", "buffer_bind")

        plan = g.analyse()

        assert plan.n_actions == 3
        assert plan.n_stages == 3
        # Each stage has one unit with one action
        for stage in plan.stages:
            assert len(stage) == 1
            assert len(stage[0].actions) == 1

        # Verify ordering
        stage_actions = [plan.stages[i][0].actions[0] for i in range(3)]
        assert stage_actions == [0, 1, 2]

    def test_parallel_schedule(self):
        """A and B unrelated; same stage."""
        g = SchedulingGraph()
        g.add_action(0)
        g.add_action(1)

        plan = g.analyse()

        assert plan.n_actions == 2
        assert plan.n_stages == 1
        # Both actions at level 0
        all_actions = []
        for unit in plan.stages[0]:
            all_actions.extend(unit.actions)
        assert set(all_actions) == {0, 1}

    def test_diamond_schedule(self):
        """A -> B, A -> C, B -> D, C -> D; 3 stages."""
        g = SchedulingGraph()
        for i in range(4):
            g.add_action(i)
        g.add_edge(0, 1, "sequential", "buffer_bind")
        g.add_edge(0, 2, "sequential", "buffer_bind")
        g.add_edge(1, 3, "sequential", "buffer_bind")
        g.add_edge(2, 3, "sequential", "buffer_bind")

        plan = g.analyse()

        assert plan.n_stages == 3
        # Stage 0: A
        assert _stage_action_set(plan, 0) == {0}
        # Stage 1: B and C (parallel)
        assert _stage_action_set(plan, 1) == {1, 2}
        # Stage 2: D
        assert _stage_action_set(plan, 2) == {3}

    def test_stream_concurrent_group(self):
        """A <-> B (stream bind); same execution unit."""
        g = SchedulingGraph()
        g.add_action(0)
        g.add_action(1)
        g.add_edge(0, 1, "concurrent", "stream_bind")

        plan = g.analyse()

        assert plan.n_stages == 1
        # Both in the same execution unit
        assert len(plan.stages[0]) == 1
        unit = plan.stages[0][0]
        assert set(unit.actions) == {0, 1}

    def test_cycle_detection(self):
        """A -> B -> A; raises ScheduleConflictError."""
        g = SchedulingGraph()
        g.add_action(0)
        g.add_action(1)
        g.add_edge(0, 1, "sequential", "buffer_bind")
        g.add_edge(1, 0, "sequential", "buffer_bind")

        with pytest.raises(ScheduleConflictError, match="Cycle"):
            g.analyse()

    def test_seq_concurrent_conflict(self):
        """A -> B and A <-> B; raises error."""
        g = SchedulingGraph()
        g.add_action(0)
        g.add_action(1)
        g.add_edge(0, 1, "sequential", "explicit_sequence")
        g.add_edge(0, 1, "concurrent", "stream_bind")

        with pytest.raises(ScheduleConflictError, match="both sequentially"):
            g.analyse()

    def test_mutex_pair_same_level(self):
        """A, B lock same resource; mutex doesn't force ordering."""
        g = SchedulingGraph()
        g.add_action(0)
        g.add_action(1)
        g.add_edge(0, 1, "mutex", "resource_contention")

        plan = g.analyse()

        # Mutex pairs don't impose ordering; both at level 0
        assert plan.n_stages == 1
        assert _stage_action_set(plan, 0) == {0, 1}

    def test_state_write_exclusion(self):
        """Two state writers with sequential edge; different stages."""
        g = SchedulingGraph()
        g.add_action(0)
        g.add_action(1)
        g.add_edge(0, 1, "sequential", "state_bind")

        plan = g.analyse()

        assert plan.n_stages == 2
        assert _stage_action_set(plan, 0) == {0}
        assert _stage_action_set(plan, 1) == {1}

    def test_inferred_action_integration(self):
        """Add an inferred producer; re-run analysis; correct stages."""
        g = SchedulingGraph()
        # Initial: just action 1 (consumer)
        g.add_action(1)

        # Infer producer (action 0)
        g.add_action(0)
        g.add_edge(0, 1, "sequential", "buffer_bind")

        plan = g.analyse()

        assert plan.n_stages == 2
        assert _stage_action_set(plan, 0) == {0}
        assert _stage_action_set(plan, 1) == {1}

    def test_schedule_8_actions(self):
        """8 actions, mixed edges; correct level assignment.

        Graph:
          0 -> 1 -> 3 -> 5
          0 -> 2 -> 4 -> 5
          6 <-> 7 (concurrent)
          5 -> 6
        """
        g = SchedulingGraph()
        for i in range(8):
            g.add_action(i)

        g.add_edge(0, 1, "sequential", "buffer_bind")
        g.add_edge(0, 2, "sequential", "buffer_bind")
        g.add_edge(1, 3, "sequential", "buffer_bind")
        g.add_edge(2, 4, "sequential", "buffer_bind")
        g.add_edge(3, 5, "sequential", "buffer_bind")
        g.add_edge(4, 5, "sequential", "buffer_bind")
        g.add_edge(5, 6, "sequential", "buffer_bind")
        g.add_edge(6, 7, "concurrent", "stream_bind")

        plan = g.analyse()

        assert plan.n_actions == 8
        # Stage 0: action 0
        assert _stage_action_set(plan, 0) == {0}
        # Stage 1: actions 1, 2
        assert _stage_action_set(plan, 1) == {1, 2}
        # Stage 2: actions 3, 4
        assert _stage_action_set(plan, 2) == {3, 4}
        # Stage 3: action 5
        assert _stage_action_set(plan, 3) == {5}
        # Stage 4: actions 6, 7 (concurrent unit)
        assert _stage_action_set(plan, 4) == {6, 7}
        # 6 and 7 should be in the same unit
        units_at_4 = plan.stages[4]
        assert len(units_at_4) == 1
        assert set(units_at_4[0].actions) == {6, 7}

    def test_empty_graph(self):
        """Empty graph produces empty plan."""
        g = SchedulingGraph()
        plan = g.analyse()
        assert plan.n_actions == 0
        assert plan.n_stages == 0

    def test_single_action(self):
        """Single action, no edges."""
        g = SchedulingGraph()
        g.add_action(0)
        plan = g.analyse()
        assert plan.n_actions == 1
        assert plan.n_stages == 1
        assert _stage_action_set(plan, 0) == {0}


def _stage_action_set(plan: StagedPlan, stage_idx: int) -> set:
    """Helper: collect all action IDs at a given stage."""
    actions = set()
    for unit in plan.stages[stage_idx]:
        actions.update(unit.actions)
    return actions
