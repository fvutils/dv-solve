"""Unit tests for the InferenceOrchestrator integration."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

from dv_solve.icl import FlowFieldDescriptor, build_icl_table
from dv_solve.structural_solver import (
    InferenceOrchestrator,
    InferenceFeasibilityError,
)
from dv_solve.stream_solver import ConstraintSystemSpec


class TestInferenceOrchestrator:

    def _make_orchestrator(self, descriptors=None, solve_results=None):
        """Create an orchestrator with optional ICL and mock solver."""
        icl = None
        if descriptors:
            icl = build_icl_table(descriptors)

        results = solve_results or {}

        def mock_solve(action_type, extra_constraints=None):
            return results.get(action_type)

        return InferenceOrchestrator(
            icl_table=icl,
            solve_fn=mock_solve,
        )

    def test_buffer_inference_via_orchestrator(self):
        """Orchestrator delegates buffer inference correctly."""
        descriptors = [
            FlowFieldDescriptor("writer", "output.data", "buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "buf_t",
                                "input", "buffer"),
        ]
        orch = self._make_orchestrator(
            descriptors, {"writer": {"addr": 100}}
        )
        result = orch.infer_buffer("reader", "input.data")
        assert result.action_type == "writer"
        assert result.solved_field_values["addr"] == 100

    def test_stream_solve_via_orchestrator(self):
        """Orchestrator delegates stream joint solve correctly."""
        descriptors = [
            FlowFieldDescriptor("tx", "output.s", "s_t",
                                "output", "stream"),
            FlowFieldDescriptor("rx", "input.s", "s_t",
                                "input", "stream"),
        ]

        def mock_solve(merged_system):
            return {v.name: 42 for v in merged_system.variables}

        orch = InferenceOrchestrator(
            icl_table=build_icl_table(descriptors),
            solve_fn=mock_solve,
        )

        producer = ConstraintSystemSpec(
            variables={"data": (0, 255, 8)}, constraints=[]
        )
        consumer = ConstraintSystemSpec(
            variables={"data": (0, 255, 8)}, constraints=[]
        )
        result = orch.solve_stream_pair(producer, consumer, ["data"])
        assert "data" in result.shared_values

    def test_schedule_via_orchestrator(self):
        """Orchestrator builds schedule DAG correctly."""
        orch = self._make_orchestrator()
        plan = orch.build_schedule(
            action_ids=[0, 1, 2],
            edges=[
                (0, 1, "sequential", "buffer_bind"),
                (1, 2, "sequential", "buffer_bind"),
            ],
        )
        assert plan.n_stages == 3
        assert plan.n_actions == 3

    def test_record_buffer_values(self):
        """Orchestrator records buffer values for accumulation."""
        descriptors = [
            FlowFieldDescriptor("writer", "output.data", "buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "buf_t",
                                "input", "buffer"),
        ]
        orch = self._make_orchestrator(
            descriptors, {"writer": {"addr": 200}}
        )
        orch.record_buffer_values("slot_a", {"addr": 0x100})
        # Verify it was stored in the flow store
        constraints = orch._flow_store.constraints_for_producer("slot_a")
        assert len(constraints) == 1
        assert constraints[0].bound == 0x100

    def test_no_icl_raises_for_buffer(self):
        """Buffer inference without ICL raises error."""
        orch = InferenceOrchestrator()
        with pytest.raises(InferenceFeasibilityError, match="not configured"):
            orch.infer_buffer("reader", "input.data")

    def test_no_icl_raises_for_stream(self):
        """Stream solve without ICL raises error."""
        orch = InferenceOrchestrator()
        p = ConstraintSystemSpec(variables={"x": (0, 10, 4)}, constraints=[])
        c = ConstraintSystemSpec(variables={"x": (0, 10, 4)}, constraints=[])
        with pytest.raises(InferenceFeasibilityError, match="not configured"):
            orch.solve_stream_pair(p, c, ["x"])
