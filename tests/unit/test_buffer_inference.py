"""Unit tests for buffer inference (Phase B1).

Tests:
- Single-hop buffer inference with one candidate
- Back-propagation of consumer constraints to producer
- Multi-candidate ICL with first candidate UNSAT
- N-producer constraint accumulation
- Empty ICL raises error
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "src"))

from dv_solve.icl import (
    ICLEntry,
    ICLTable,
    FlowFieldDescriptor,
    build_icl_table,
)
from dv_solve.buffer_inference import (
    BufferInferenceEngine,
    InferredBufferAction,
)
from dv_solve.flow_constraint_store import (
    FlowObjectConstraintStore,
    ProjectedConstraint,
)
from dv_solve.structural_solver import InferenceFeasibilityError


# ------------------------------------------------------------------ #
# ICL table tests                                                      #
# ------------------------------------------------------------------ #

class TestICLTable:

    def test_build_basic_icl(self):
        """One producer, one consumer, same buffer type."""
        descriptors = [
            FlowFieldDescriptor("writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        assert len(table) == 1

        entry = table.lookup("reader", "input.data")
        assert entry is not None
        assert entry.candidates == ["writer"]
        assert entry.flow_type == "buffer"
        assert entry.ordering == "sequential_before"

    def test_no_producer_no_entry(self):
        """Consumer with no matching producer -> no ICL entry."""
        descriptors = [
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        assert len(table) == 0
        assert table.lookup("reader", "input.data") is None

    def test_multi_producer_candidates(self):
        """Two producers for the same buffer type."""
        descriptors = [
            FlowFieldDescriptor("fast_writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("slow_writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        entry = table.lookup("reader", "input.data")
        assert entry is not None
        assert set(entry.candidates) == {"fast_writer", "slow_writer"}

    def test_stream_ordering_is_parallel(self):
        """Stream flow type gets parallel ordering."""
        descriptors = [
            FlowFieldDescriptor("producer", "output.s", "stream_t",
                                "output", "stream"),
            FlowFieldDescriptor("consumer", "input.s", "stream_t",
                                "input", "stream"),
        ]
        table = build_icl_table(descriptors)
        entry = table.lookup("consumer", "input.s")
        assert entry is not None
        assert entry.ordering == "parallel"

    def test_pool_isolation(self):
        """Different pools are isolated."""
        descriptors = [
            FlowFieldDescriptor("writer_a", "output.data", "data_buf_t",
                                "output", "buffer", pool="pool_a"),
            FlowFieldDescriptor("reader_b", "input.data", "data_buf_t",
                                "input", "buffer", pool="pool_b"),
        ]
        table = build_icl_table(descriptors)
        assert len(table) == 0


# ------------------------------------------------------------------ #
# Buffer inference tests                                               #
# ------------------------------------------------------------------ #

class TestBufferInference:

    def _make_engine(self, icl_table, solve_results, checkpoint=True):
        """Create a BufferInferenceEngine with a mock solve function."""
        call_log = []

        def mock_solve(action_type, extra_constraints):
            call_log.append((action_type, extra_constraints))
            return solve_results.get(action_type)

        cp_counter = [0]
        def mock_checkpoint():
            cp_counter[0] += 1
            return cp_counter[0]

        restore_log = []
        def mock_restore(cp):
            restore_log.append(cp)

        store = FlowObjectConstraintStore()
        engine = BufferInferenceEngine(
            icl_table=icl_table,
            flow_store=store,
            solve_fn=mock_solve,
            checkpoint_fn=mock_checkpoint if checkpoint else None,
            restore_fn=mock_restore if checkpoint else None,
        )
        return engine, call_log, restore_log

    def test_single_hop_buffer_inference(self):
        """Consumer needs buffer; one candidate producer; infer + solve."""
        descriptors = [
            FlowFieldDescriptor("writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        solve_results = {"writer": {"addr": 0x100, "data": 42}}

        engine, call_log, _ = self._make_engine(table, solve_results)
        result = engine.infer_single_hop("reader", "input.data")

        assert result.action_type == "writer"
        assert result.solved_field_values == {"addr": 0x100, "data": 42}
        assert result.ordering == "sequential_before"
        assert len(call_log) == 1

    def test_buffer_backprop_constraint(self):
        """Consumer constrains input.addr > 100; constraint propagated."""
        descriptors = [
            FlowFieldDescriptor("writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        solve_results = {"writer": {"addr": 200}}

        engine, call_log, _ = self._make_engine(table, solve_results)

        consumer_constraints = [{"field": "input.addr", "op": "gt", "bound": 100}]
        field_mapping = {"input.addr": "output.addr"}

        result = engine.infer_single_hop(
            "reader", "input.data",
            consumer_constraints=consumer_constraints,
            field_mapping=field_mapping,
        )

        assert result.action_type == "writer"
        # Verify the projected constraint was passed to the solve function
        _, extra = call_log[0]
        assert len(extra) == 1
        assert extra[0].field_name == "output.addr"
        assert extra[0].op == "gt"
        assert extra[0].bound == 100

    def test_buffer_multi_candidate_icl(self):
        """Two candidates; first is UNSAT; second succeeds."""
        descriptors = [
            FlowFieldDescriptor("bad_writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("good_writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        # bad_writer returns None (UNSAT), good_writer succeeds
        solve_results = {"good_writer": {"data": 99}}

        engine, call_log, restore_log = self._make_engine(table, solve_results)
        result = engine.infer_single_hop("reader", "input.data")

        assert result.action_type == "good_writer"
        assert len(call_log) == 2
        # Should have restored after bad_writer failed
        assert len(restore_log) == 1

    def test_buffer_n_producer_accumulation(self):
        """Two sequential producers; consumer sees both values."""
        descriptors = [
            FlowFieldDescriptor("writer", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        solve_results = {"writer": {"addr": 0x200, "len": 64}}

        engine, _, _ = self._make_engine(table, solve_results)

        # First producer solves and records values
        engine.record_solved_values("reader.input.data", {"addr": 0x100})

        # Verify accumulated values
        acc = engine.get_accumulated_values("reader.input.data")
        assert acc == {"addr": 0x100}

        # Second producer adds more values
        engine.record_solved_values("reader.input.data", {"len": 32})
        acc = engine.get_accumulated_values("reader.input.data")
        assert acc == {"addr": 0x100, "len": 32}

    def test_buffer_inference_no_candidate(self):
        """Empty ICL; raises InferenceFeasibilityError."""
        table = ICLTable()  # empty
        engine, _, _ = self._make_engine(table, {})

        with pytest.raises(InferenceFeasibilityError, match="No ICL candidates"):
            engine.infer_single_hop("reader", "input.data")

    def test_all_candidates_fail(self):
        """All candidates UNSAT; raises InferenceFeasibilityError."""
        descriptors = [
            FlowFieldDescriptor("writer_a", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("writer_b", "output.data", "data_buf_t",
                                "output", "buffer"),
            FlowFieldDescriptor("reader", "input.data", "data_buf_t",
                                "input", "buffer"),
        ]
        table = build_icl_table(descriptors)
        solve_results = {}  # all return None

        engine, _, restore_log = self._make_engine(table, solve_results)

        with pytest.raises(InferenceFeasibilityError, match="All .* candidates failed"):
            engine.infer_single_hop("reader", "input.data")
        assert len(restore_log) == 2


# ------------------------------------------------------------------ #
# FlowObjectConstraintStore enhancement tests                         #
# ------------------------------------------------------------------ #

class TestFlowStoreRecordSolvedValues:

    def test_record_creates_eq_constraints(self):
        store = FlowObjectConstraintStore()
        store.record_solved_values("slot_a", {"addr": 0x100, "data": 42})

        constraints = store.constraints_for_producer("slot_a")
        assert len(constraints) == 2
        ops = {c.field_name: c for c in constraints}
        assert ops["addr"].op == "eq"
        assert ops["addr"].bound == 0x100
        assert ops["data"].op == "eq"
        assert ops["data"].bound == 42

    def test_record_accumulates(self):
        store = FlowObjectConstraintStore()
        store.record_solved_values("slot_a", {"addr": 0x100})
        store.record_solved_values("slot_a", {"data": 42})

        constraints = store.constraints_for_producer("slot_a")
        assert len(constraints) == 2
