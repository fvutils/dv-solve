"""Buffer supply inference with ICL lookup and backtracking.

When the orchestrator encounters an unbound buffer input, this module
looks up candidate producers from the ICL table, instantiates each
candidate, projects consumer constraints backward, and solves.
Uses checkpoint/restore for backtracking on UNSAT candidates.

Design ref: schedule-buffer-stream-inference-plan.md Section 3.2 (B1.2, B1.3)
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

from .icl import ICLEntry, ICLTable
from .flow_constraint_store import (
    FlowObjectConstraintStore,
    ProjectedConstraint,
    project_to_producer,
)
from .structural_solver import InferenceFeasibilityError


@dataclass
class InferredBufferAction:
    """Result of buffer inference: an inferred producer action."""
    action_type: str
    solved_field_values: Dict[str, int]
    ordering: str = "sequential_before"


@dataclass
class ActionInstance:
    """Lightweight representation of an action for inference."""
    action_type: str
    fields: Dict[str, Any]     # field_name -> value or None (unbound)
    constraints: List[Dict[str, Any]]  # list of constraint dicts


class BufferInferenceEngine:
    """Infers sequential producer actions for unbound buffer inputs.

    Uses ICL table for candidate lookup and a solve function for
    constraint satisfaction. Supports N-producer accumulation where
    multiple producers each write a subset of fields.
    """

    def __init__(
        self,
        icl_table: ICLTable,
        flow_store: FlowObjectConstraintStore,
        solve_fn: Callable[..., Optional[Dict[str, int]]],
        checkpoint_fn: Optional[Callable[[], int]] = None,
        restore_fn: Optional[Callable[[int], None]] = None,
        max_icl_depth: int = 5,
    ):
        """
        Args:
            icl_table: The ICL lookup table.
            flow_store: Constraint store for flow-object projections.
            solve_fn: Callable(action_type, field_mapping, extra_constraints)
                      -> dict of solved field values, or None if UNSAT.
            checkpoint_fn: Optional callable that saves solver state.
            restore_fn: Optional callable that restores solver state.
            max_icl_depth: Maximum number of candidates to try per field.
        """
        self._icl = icl_table
        self._flow_store = flow_store
        self._solve_fn = solve_fn
        self._checkpoint_fn = checkpoint_fn
        self._restore_fn = restore_fn
        self._max_icl_depth = max_icl_depth
        # Accumulated solved values per flow slot
        self._accumulated: Dict[str, Dict[str, int]] = {}

    def infer_single_hop(
        self,
        consumer_action_type: str,
        input_field: str,
        consumer_constraints: Optional[List[Dict[str, Any]]] = None,
        field_mapping: Optional[Dict[str, str]] = None,
    ) -> InferredBufferAction:
        """Infer a single producer for an unbound buffer input.

        Tries each ICL candidate in order. Uses checkpoint/restore
        to backtrack on UNSAT candidates.

        Args:
            consumer_action_type: The consuming action's type name.
            input_field: The unbound input field name.
            consumer_constraints: Constraints from the consumer on the
                buffer fields (for back-propagation).
            field_mapping: Maps consumer input field names to producer
                output field names. If None, uses identity mapping.

        Returns:
            InferredBufferAction with the selected producer and solved values.

        Raises:
            InferenceFeasibilityError: If no candidate can satisfy the constraints.
        """
        entry = self._icl.lookup(consumer_action_type, input_field)
        if entry is None or not entry.candidates:
            raise InferenceFeasibilityError(
                f"No ICL candidates for {consumer_action_type}.{input_field}"
            )

        # Back-propagate consumer constraints to producer space
        projected: List[ProjectedConstraint] = []
        if consumer_constraints and field_mapping:
            projected = project_to_producer(
                consumer_constraints, field_mapping
            )

        candidates = entry.candidates[:self._max_icl_depth]

        for candidate_type in candidates:
            # Checkpoint before attempting this candidate
            cp = None
            if self._checkpoint_fn is not None:
                cp = self._checkpoint_fn()

            # Build extra constraints from projected + accumulated
            extra_constraints = list(projected)
            flow_slot_key = f"{consumer_action_type}.{input_field}"
            if flow_slot_key in self._accumulated:
                for fname, fval in self._accumulated[flow_slot_key].items():
                    extra_constraints.append(ProjectedConstraint(
                        field_name=fname, op="eq", bound=fval,
                        original_field=fname,
                    ))

            # Attempt solve
            result = self._solve_fn(
                candidate_type, extra_constraints
            )

            if result is not None:
                return InferredBufferAction(
                    action_type=candidate_type,
                    solved_field_values=result,
                    ordering=entry.ordering,
                )

            # UNSAT: restore and try next candidate
            if cp is not None and self._restore_fn is not None:
                self._restore_fn(cp)

        raise InferenceFeasibilityError(
            f"All {len(candidates)} ICL candidates failed for "
            f"{consumer_action_type}.{input_field}"
        )

    def record_solved_values(
        self,
        flow_slot_key: str,
        field_values: Dict[str, int],
    ) -> None:
        """Record concrete solved values from a producer (N-producer accumulation).

        These values become equality constraints for subsequent producers
        or the consumer solving the same flow slot.
        """
        if flow_slot_key not in self._accumulated:
            self._accumulated[flow_slot_key] = {}
        self._accumulated[flow_slot_key].update(field_values)

    def get_accumulated_values(
        self, flow_slot_key: str
    ) -> Dict[str, int]:
        """Retrieve accumulated solved values for a flow slot."""
        return dict(self._accumulated.get(flow_slot_key, {}))

    def clear_accumulated(
        self, flow_slot_key: Optional[str] = None
    ) -> None:
        """Clear accumulated values for a specific slot, or all slots."""
        if flow_slot_key is not None:
            self._accumulated.pop(flow_slot_key, None)
        else:
            self._accumulated.clear()
