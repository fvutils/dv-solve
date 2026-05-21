"""Inference Candidate List (ICL) table construction and lookup.

At elaboration time, builds a static lookup table mapping each
(consumer_action_type, input_field) pair to a list of candidate
producer action types that can supply a compatible flow object.

Design ref: schedule-buffer-stream-inference-plan.md Section 3.2 (B1.1)
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Set, Tuple


@dataclass
class ICLEntry:
    """One entry in the ICL table: a consumer field and its candidate producers."""
    consumer_action_type: str
    input_field: str
    candidates: List[str]       # action type names that can produce this field
    flow_type: str              # "buffer" | "state" | "stream"
    ordering: str               # "sequential_before" | "parallel"


@dataclass
class FlowFieldDescriptor:
    """Describes a flow-object field on an action type."""
    action_type: str
    field_name: str
    flow_object_type: str       # the type name of the flow object
    direction: str              # "input" | "output"
    flow_type: str              # "buffer" | "state" | "stream"
    pool: str = ""              # pool name (empty = default pool)
    is_abstract: bool = False   # True for abstract action types (excluded from ICL as producers)


class ICLTable:
    """Inference Candidate List table.

    Keyed by (consumer_action_type, input_field) -> ICLEntry.
    """

    def __init__(self) -> None:
        self._entries: Dict[Tuple[str, str], ICLEntry] = {}

    def lookup(self, action_type: str, field_name: str) -> Optional[ICLEntry]:
        """Look up candidates for a consumer's unbound input field."""
        return self._entries.get((action_type, field_name))

    def entries(self) -> List[ICLEntry]:
        """Return all ICL entries."""
        return list(self._entries.values())

    def __len__(self) -> int:
        return len(self._entries)


def build_icl_table(
    field_descriptors: Sequence[FlowFieldDescriptor],
) -> ICLTable:
    """Build an ICL table from a flat list of flow-field descriptors.

    For each input field, finds all action types that output a compatible
    flow object (same flow_object_type, same pool, matching flow_type).
    """
    table = ICLTable()

    # Index outputs by (flow_object_type, pool, flow_type)
    # Abstract actions are excluded as producers since they cannot be instantiated.
    outputs: Dict[Tuple[str, str, str], List[str]] = {}
    for fd in field_descriptors:
        if fd.direction == "output" and not fd.is_abstract:
            key = (fd.flow_object_type, fd.pool, fd.flow_type)
            outputs.setdefault(key, []).append(fd.action_type)

    # For each input field, find compatible outputs
    for fd in field_descriptors:
        if fd.direction != "input":
            continue
        key = (fd.flow_object_type, fd.pool, fd.flow_type)
        candidates = outputs.get(key, [])
        if not candidates:
            continue

        ordering = "parallel" if fd.flow_type == "stream" else "sequential_before"

        entry = ICLEntry(
            consumer_action_type=fd.action_type,
            input_field=fd.field_name,
            candidates=list(candidates),
            flow_type=fd.flow_type,
            ordering=ordering,
        )
        table._entries[(fd.action_type, fd.field_name)] = entry

    return table
