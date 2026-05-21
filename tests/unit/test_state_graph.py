"""Unit tests for state graph construction and BFS (Phase S4)."""
from __future__ import annotations

import pytest
import sys
from pathlib import Path

# Ensure solver package is importable
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

from dv_solve.state_graph import (
    FieldDescriptor, TransitionDescriptor,
    StateGraphBuilder, StateGraph,
    StateSpaceTooLargeError, NoPathError,
)


class TestStateGraph:

    def test_linear_4_states(self):
        """T3: 4 states (0..3), +/-1 transitions. BFS 0->3 returns 3 steps."""
        fields = [FieldDescriptor("level", 0, 3)]

        def step(values):
            v = values[0]
            nexts = []
            if v + 1 <= 3: nexts.append((v + 1,))
            if v - 1 >= 0: nexts.append((v - 1,))
            return nexts

        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[TransitionDescriptor(0, step)],
            initial_predicate=lambda v: v[0] == 0,
        )
        g = builder.build()

        assert len(g.nodes) == 4
        assert g.initial_index == 0

        # BFS from state 0 to state 3
        path = g.find_path({0}, {3})
        assert len(path) == 3
        # Verify path goes 0->1->2->3
        assert path[0].src == 0 and path[0].dst == 1
        assert path[1].src == 1 and path[1].dst == 2
        assert path[2].src == 2 and path[2].dst == 3

    def test_diamond_4_states(self):
        """Diamond: A->B, A->C, B->D, C->D, D->A. BFS A->D = 2 steps."""
        # Encode states as (0=A, 1=B, 2=C, 3=D)
        fields = [FieldDescriptor("state", 0, 3)]

        transitions = {
            0: [1, 2],     # A->B, A->C
            1: [3],        # B->D
            2: [3],        # C->D
            3: [0],        # D->A
        }

        def step(values):
            return [(t,) for t in transitions.get(values[0], [])]

        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[TransitionDescriptor(0, step)],
        )
        g = builder.build()

        assert len(g.nodes) == 4
        path = g.find_path({0}, {3})
        assert len(path) == 2  # A->B->D or A->C->D

    def test_find_path_no_path(self):
        """Disconnected graph; BFS raises NoPathError."""
        fields = [FieldDescriptor("x", 0, 2)]

        # No transitions at all
        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[],
        )
        g = builder.build()

        with pytest.raises(NoPathError):
            g.find_path({0}, {2})

    def test_find_waypoint_path(self):
        """3 waypoints; path visits them in order."""
        fields = [FieldDescriptor("pos", 0, 5)]

        def step(values):
            v = values[0]
            nexts = []
            if v + 1 <= 5: nexts.append((v + 1,))
            if v - 1 >= 0: nexts.append((v - 1,))
            return nexts

        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[TransitionDescriptor(0, step)],
        )
        g = builder.build()

        waypoints = [{0}, {3}, {5}]
        path = g.find_waypoint_path(waypoints)
        # 0->3 = 3 steps, 3->5 = 2 steps => total 5
        assert len(path) == 5

    def test_initial_state_identified(self):
        """Initial constraint identifies unique start node."""
        fields = [FieldDescriptor("x", 0, 4)]

        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[],
            initial_predicate=lambda v: v[0] == 2,
        )
        g = builder.build()

        assert g.initial_index is not None
        assert g.nodes[g.initial_index].values == (2,)

    def test_state_space_too_large(self):
        """State type with large domain; raises StateSpaceTooLargeError."""
        fields = [
            FieldDescriptor("a", 0, 100),
            FieldDescriptor("b", 0, 100),
        ]

        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[],
            max_states=100,
        )

        with pytest.raises(StateSpaceTooLargeError):
            builder.build()

    def test_state_invariant_filters(self):
        """State invariant filters out invalid states."""
        fields = [
            FieldDescriptor("x", 0, 3),
            FieldDescriptor("y", 0, 3),
        ]

        # Only states where x + y <= 3 are valid
        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[],
            state_invariant=lambda v: v[0] + v[1] <= 3,
        )
        g = builder.build()

        # Valid states: (0,0),(0,1),(0,2),(0,3),(1,0),(1,1),(1,2),(2,0),(2,1),(3,0) = 10
        assert len(g.nodes) == 10
        for n in g.nodes:
            assert n.values[0] + n.values[1] <= 3

    def test_match_constraint(self):
        """match_constraint returns correct node indices."""
        fields = [FieldDescriptor("x", 0, 4)]
        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[],
        )
        g = builder.build()

        # Find nodes where x > 2
        matches = g.match_constraint(lambda v: v[0] > 2)
        assert matches == {3, 4}

    def test_multi_field_graph(self):
        """2-field state graph with transitions."""
        fields = [
            FieldDescriptor("a", 0, 1),
            FieldDescriptor("b", 0, 1),
        ]

        def step(values):
            a, b = values
            nexts = []
            if a == 0: nexts.append((1, b))
            if b == 0: nexts.append((a, 1))
            return nexts

        builder = StateGraphBuilder(
            field_descriptors=fields,
            transition_descriptors=[TransitionDescriptor(0, step)],
            initial_predicate=lambda v: v == (0, 0),
        )
        g = builder.build()

        assert len(g.nodes) == 4  # (0,0),(0,1),(1,0),(1,1)

        # BFS from (0,0) to (1,1): needs 2 steps
        start = g._value_to_index[(0, 0)]
        goal = g._value_to_index[(1, 1)]
        path = g.find_path({start}, {goal})
        assert len(path) == 2
