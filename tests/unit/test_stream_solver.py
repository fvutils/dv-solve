"""T1-7: Stream solver unit tests -- joint solve and ICL ordering."""
import pytest
from dv_solve.icl import FlowFieldDescriptor, build_icl_table
from dv_solve.stream_solver import (
    ConstraintSystemSpec,
    StreamSolver,
    build_stream_joint_system,
)


def test_stream_joint_system_shared_fields():
    """Shared fields in merged system have intersected domains."""
    producer = ConstraintSystemSpec(
        variables={"val": (0, 100, 32)},
        constraints=[],
    )
    consumer = ConstraintSystemSpec(
        variables={"val": (50, 200, 32)},
        constraints=[],
    )
    merged = build_stream_joint_system(producer, consumer, shared_field_names=["val"])
    shared = [v for v in merged.variables if v.source == "shared"]
    assert len(shared) == 1
    assert shared[0].name == "val"
    assert shared[0].lo == 50   # max(0, 50)
    assert shared[0].hi == 100  # min(100, 200)


def test_stream_joint_system_empty_domain_detected():
    """Empty domain (lo > hi) on a shared field is detected by build."""
    producer = ConstraintSystemSpec(
        variables={"val": (0, 10, 32)},
        constraints=[],
    )
    consumer = ConstraintSystemSpec(
        variables={"val": (50, 100, 32)},
        constraints=[],
    )
    merged = build_stream_joint_system(producer, consumer, shared_field_names=["val"])
    shared = [v for v in merged.variables if v.source == "shared"]
    assert shared[0].lo > shared[0].hi  # empty domain; solver should detect this


def test_icl_stream_ordering():
    """Stream fields in the ICL have ordering='parallel' (T1-7)."""
    descriptors = [
        FlowFieldDescriptor("Prod", "out_s", "StreamT", "output", "stream"),
        FlowFieldDescriptor("Cons", "in_s", "StreamT", "input", "stream"),
    ]
    table = build_icl_table(descriptors)
    entry = table.lookup("Cons", "in_s")
    assert entry is not None
    assert entry.ordering == "parallel"
    assert "Prod" in entry.candidates
