"""Unit tests for the inference chain orchestrator (Phase S5)."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

from dv_solve.state_graph import (
    FieldDescriptor, TransitionDescriptor,
    StateGraphBuilder, NoPathError,
)
from dv_solve.structural_solver import (
    solve_state_chain_graph_guided,
    solve_state_chain_constraint_guided,
    InferredAction, InferenceFeasibilityError, InferenceLimitError,
    TransitionCandidate,
)


def _build_linear_graph(n_states=4):
    """Helper: build a linear graph 0->1->2->...->n-1 with +/-1 transitions."""
    fields = [FieldDescriptor("level", 0, n_states - 1)]

    def step(values):
        v = values[0]
        nexts = []
        if v + 1 <= n_states - 1:
            nexts.append((v + 1,))
        if v - 1 >= 0:
            nexts.append((v - 1,))
        return nexts

    builder = StateGraphBuilder(
        field_descriptors=fields,
        transition_descriptors=[TransitionDescriptor(0, step)],
        initial_predicate=lambda v: v[0] == 0,
    )
    return builder.build()


class TestInferenceChain:

    def test_thermal_chain_graph_guided(self):
        """T3: infer 6 steps (0->3->0). Shortest path via graph."""
        g = _build_linear_graph(4)

        # Waypoints: start at 0, go to 3, then back to 0
        wp0 = {g._value_to_index[(0,)]}
        wp1 = {g._value_to_index[(3,)]}
        wp2 = {g._value_to_index[(0,)]}

        actions = solve_state_chain_graph_guided(g, [wp0, wp1, wp2])
        assert len(actions) == 6  # 0->1->2->3->2->1->0

        # Verify state values at each step
        assert actions[0].prev_state_values == (0,)
        assert actions[0].next_state_values == (1,)
        assert actions[2].next_state_values == (3,)
        assert actions[5].next_state_values == (0,)

    def test_diamond_backtracking(self):
        """Diamond: A(0)->D(3) in 2 steps."""
        fields = [FieldDescriptor("state", 0, 3)]
        transitions = {
            0: [1, 2],
            1: [3],
            2: [3],
            3: [0],
        }

        def step(values):
            return [(t,) for t in transitions.get(values[0], [])]

        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[TransitionDescriptor(0, step)],
        )
        g = builder.build()

        start = {g._value_to_index[(0,)]}
        goal = {g._value_to_index[(3,)]}
        actions = solve_state_chain_graph_guided(g, [start, goal])
        assert len(actions) == 2

    def test_no_feasible_path(self):
        """Disconnected graph; raises InferenceFeasibilityError."""
        fields = [FieldDescriptor("x", 0, 2)]
        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[],
        )
        g = builder.build()

        with pytest.raises(InferenceFeasibilityError):
            solve_state_chain_graph_guided(g, [{0}, {2}])

    def test_multiple_waypoints(self):
        """3 waypoints in linear model; path visits all in order."""
        g = _build_linear_graph(6)
        wp0 = {g._value_to_index[(0,)]}
        wp1 = {g._value_to_index[(3,)]}
        wp2 = {g._value_to_index[(5,)]}

        actions = solve_state_chain_graph_guided(g, [wp0, wp1, wp2])
        # 0->3 = 3 steps, 3->5 = 2 steps
        assert len(actions) == 5

    def test_constraint_guided_basic(self):
        """Basic constraint-guided: 1-step transition."""
        valid_states = [(0,), (1,), (2,), (3,)]
        tc = TransitionCandidate(action_type_id=0, build_constraints=None)

        actions = solve_state_chain_constraint_guided(
            initial_values=(0,),
            waypoint_predicates=[lambda v: v[0] == 3],
            transition_candidates=[tc],
            valid_states=valid_states,
            max_depth=5,
        )
        assert len(actions) >= 1
        assert actions[-1].next_state_values == (3,)

    def test_constraint_guided_no_path(self):
        """Constraint-guided with no valid transitions; raises error."""
        valid_states = [(0,), (1,)]
        # No transition candidates at all
        with pytest.raises(InferenceFeasibilityError):
            solve_state_chain_constraint_guided(
                initial_values=(0,),
                waypoint_predicates=[lambda v: v[0] == 1],
                transition_candidates=[],
                valid_states=valid_states,
                max_depth=5,
            )
