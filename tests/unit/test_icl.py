"""T1-3: ICL table unit tests -- abstract action exclusion and stream ordering."""
import pytest
from dv_solve.icl import FlowFieldDescriptor, build_icl_table


def _make_desc(action, field, obj_type, direction, flow_type="buffer", is_abstract=False):
    return FlowFieldDescriptor(
        action_type=action,
        field_name=field,
        flow_object_type=obj_type,
        direction=direction,
        flow_type=flow_type,
        is_abstract=is_abstract,
    )


def test_abstract_producer_excluded_from_icl():
    """Abstract action outputs must not appear as ICL candidates (T1-3)."""
    descriptors = [
        _make_desc("AbstractProducer", "out", "Token", "output", is_abstract=True),
        _make_desc("ConcreteProducer", "out", "Token", "output", is_abstract=False),
        _make_desc("Consumer", "inp", "Token", "input"),
    ]
    table = build_icl_table(descriptors)
    entry = table.lookup("Consumer", "inp")
    assert entry is not None, "Consumer should have an ICL entry"
    assert "AbstractProducer" not in entry.candidates, \
        "Abstract producer must not appear as a candidate"
    assert "ConcreteProducer" in entry.candidates, \
        "Concrete producer must appear as a candidate"


def test_fully_abstract_pool_yields_no_entries():
    """If all producers are abstract, no ICL entry is created for the consumer."""
    descriptors = [
        _make_desc("AbstractProducer", "out", "Token", "output", is_abstract=True),
        _make_desc("Consumer", "inp", "Token", "input"),
    ]
    table = build_icl_table(descriptors)
    entry = table.lookup("Consumer", "inp")
    assert entry is None, "No entry expected when all producers are abstract"


def test_stream_ordering_is_parallel():
    """Stream-type flow fields must produce ordering='parallel' in ICL entries."""
    descriptors = [
        _make_desc("Producer", "out_s", "StreamObj", "output", flow_type="stream"),
        _make_desc("Consumer", "in_s", "StreamObj", "input", flow_type="stream"),
    ]
    table = build_icl_table(descriptors)
    entry = table.lookup("Consumer", "in_s")
    assert entry is not None
    assert entry.ordering == "parallel", \
        f"Stream flow type should have ordering='parallel', got {entry.ordering!r}"


def test_buffer_ordering_is_sequential_before():
    """Buffer-type flow fields must produce ordering='sequential_before'."""
    descriptors = [
        _make_desc("Producer", "out_b", "BufObj", "output", flow_type="buffer"),
        _make_desc("Consumer", "in_b", "BufObj", "input", flow_type="buffer"),
    ]
    table = build_icl_table(descriptors)
    entry = table.lookup("Consumer", "in_b")
    assert entry is not None
    assert entry.ordering == "sequential_before"
