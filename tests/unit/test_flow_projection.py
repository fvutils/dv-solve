"""Unit tests for flow-object constraint projection (Phase S6)."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

from dv_solve.flow_constraint_store import (
    ProjectedConstraint,
    project_to_producer,
    FlowObjectConstraintStore,
)


class TestFlowProjection:

    def test_simple_field_projection(self):
        """Consumer constrains input.x > 5; projected to producer output.x > 5."""
        constraints = [{"field": "input.x", "op": "gt", "bound": 5}]
        mapping = {"input.x": "output.x"}

        projected = project_to_producer(constraints, mapping)

        assert len(projected) == 1
        assert projected[0].field_name == "output.x"
        assert projected[0].op == "gt"
        assert projected[0].bound == 5
        assert projected[0].original_field == "input.x"

    def test_multi_field_projection(self):
        """Multiple consumer constraints projected correctly."""
        constraints = [
            {"field": "input.a", "op": "gte", "bound": 10},
            {"field": "input.b", "op": "lt", "bound": 100},
        ]
        mapping = {"input.a": "output.a", "input.b": "output.b"}

        projected = project_to_producer(constraints, mapping)

        assert len(projected) == 2
        assert projected[0].field_name == "output.a"
        assert projected[1].field_name == "output.b"

    def test_consumer_own_field_skipped(self):
        """Consumer's own field (not a flow field) is skipped."""
        constraints = [
            {"field": "input.x", "op": "gt", "bound": 5},
            {"field": "my_field", "op": "lt", "bound": 100},
        ]
        mapping = {"input.x": "output.x"}

        projected = project_to_producer(constraints, mapping)

        assert len(projected) == 1
        assert projected[0].field_name == "output.x"

    def test_no_constraints_no_change(self):
        """No constraints means empty projection."""
        projected = project_to_producer([], {"input.x": "output.x"})
        assert len(projected) == 0

    def test_store_register_and_retrieve(self):
        """Store: register consumer constraints, retrieve for producer."""
        store = FlowObjectConstraintStore()
        pc = ProjectedConstraint("output.addr", "eq", 0x100, "input.addr")
        store.register_consumer("slot_a", [pc])

        retrieved = store.constraints_for_producer("slot_a")
        assert len(retrieved) == 1
        assert retrieved[0].field_name == "output.addr"
        assert retrieved[0].bound == 0x100

    def test_store_empty_slot(self):
        """Unregistered slot returns empty list."""
        store = FlowObjectConstraintStore()
        assert store.constraints_for_producer("nonexistent") == []

    def test_store_clear(self):
        """Clear removes constraints."""
        store = FlowObjectConstraintStore()
        pc = ProjectedConstraint("output.x", "gt", 5, "input.x")
        store.register_consumer("slot_a", [pc])
        store.clear("slot_a")
        assert store.constraints_for_producer("slot_a") == []

    def test_store_multiple_consumers(self):
        """Multiple consumers can register for the same slot."""
        store = FlowObjectConstraintStore()
        pc1 = ProjectedConstraint("output.x", "gt", 5, "input.x")
        pc2 = ProjectedConstraint("output.x", "lt", 100, "input.x")
        store.register_consumer("slot_a", [pc1])
        store.register_consumer("slot_a", [pc2])

        retrieved = store.constraints_for_producer("slot_a")
        assert len(retrieved) == 2
