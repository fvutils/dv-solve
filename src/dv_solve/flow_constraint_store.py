"""Flow-object constraint projection and storage.

Rewrites consumer constraints into producer variable IDs for
back-propagation across flow-object connections.

Design ref: solver-inference-acceleration-design.md Section 4.3
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import (
    Any, Callable, Dict, List, Optional, Set, Tuple,
)


@dataclass
class ProjectedConstraint:
    """A constraint rewritten for the producer's variable space."""
    field_name: str           # producer output field name
    op: str                   # comparison operator: "eq", "lt", "lte", "gt", "gte", "neq"
    bound: int                # constant bound value
    original_field: str       # consumer input field name


def project_to_producer(
    consumer_constraints: List[Dict[str, Any]],
    field_mapping: Dict[str, str],
    producer_domains: Optional[Dict[str, Tuple[int, int]]] = None,
) -> List[ProjectedConstraint]:
    """Rewrite consumer constraints into producer output field constraints.

    Args:
        consumer_constraints: List of dicts with keys:
            - "field": consumer field name
            - "op": comparison operator string
            - "bound": integer bound value
        field_mapping: Maps consumer input field names to producer
            output field names (e.g. {"input.addr": "output.addr"}).
        producer_domains: Optional domain bounds for producer fields.

    Returns:
        List of ProjectedConstraint for the producer.
    """
    projected: List[ProjectedConstraint] = []

    for cc in consumer_constraints:
        consumer_field = cc["field"]
        if consumer_field not in field_mapping:
            # Constraint references consumer's own field, not a flow field.
            # Use worst-case bounds (Option A from design doc).
            continue

        producer_field = field_mapping[consumer_field]
        projected.append(ProjectedConstraint(
            field_name=producer_field,
            op=cc["op"],
            bound=cc["bound"],
            original_field=consumer_field,
        ))

    return projected


class FlowObjectConstraintStore:
    """Stores and retrieves projected constraints for flow-object slots.

    Usage:
        store = FlowObjectConstraintStore()
        store.register_consumer("slot_a", projected_constraints)
        # Later, before producer solves:
        constraints = store.constraints_for_producer("slot_a")
    """

    def __init__(self):
        self._store: Dict[str, List[ProjectedConstraint]] = {}

    def register_consumer(
        self,
        flow_slot_key: str,
        constraints: List[ProjectedConstraint],
    ) -> None:
        """Register projected constraints from a consumer for a flow slot."""
        if flow_slot_key not in self._store:
            self._store[flow_slot_key] = []
        self._store[flow_slot_key].extend(constraints)

    def constraints_for_producer(
        self,
        flow_slot_key: str,
    ) -> List[ProjectedConstraint]:
        """Retrieve all projected constraints for a producer's flow slot."""
        return self._store.get(flow_slot_key, [])

    def clear(self, flow_slot_key: Optional[str] = None) -> None:
        """Clear constraints for a specific slot, or all slots."""
        if flow_slot_key:
            self._store.pop(flow_slot_key, None)
        else:
            self._store.clear()

    def record_solved_values(
        self,
        flow_slot_key: str,
        field_values: Dict[str, int],
    ) -> None:
        """Record concrete solved values from a producer.

        Converts each solved value into an equality ProjectedConstraint
        for use in N-producer accumulation scenarios.
        """
        for field_name, value in field_values.items():
            self._store.setdefault(flow_slot_key, []).append(
                ProjectedConstraint(field_name, "eq", value, field_name)
            )
