"""Stream joint-solve: merge producer + consumer constraint systems.

When two actions are stream-linked, builds a single merged constraint
system with unified variables for shared stream fields. Both sides'
constraints reference the same var_id for shared fields, enabling
cross-boundary propagation in a single solve pass.

Design ref: schedule-buffer-stream-inference-plan.md Section 4 (B2)
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Sequence, Set, Tuple

from .icl import ICLEntry, ICLTable
from .structural_solver import InferenceFeasibilityError


@dataclass
class MergedVariable:
    """A variable in the merged constraint system."""
    unified_id: int
    name: str
    source: str         # "shared" | "producer" | "consumer"
    lo: int
    hi: int
    width: int = 32


@dataclass
class MergedConstraint:
    """A constraint remapped to the unified variable ID space."""
    original_constraint: Any
    remapped_var_ids: Dict[str, int]   # original_name -> unified_id


@dataclass
class ConstraintSystemSpec:
    """Lightweight description of one side's constraint system."""
    variables: Dict[str, Tuple[int, int, int]]   # name -> (lo, hi, width)
    constraints: List[Dict[str, Any]]             # list of constraint dicts


@dataclass
class MergedSystem:
    """Result of merging two constraint systems."""
    variables: List[MergedVariable]
    var_id_map: Dict[str, int]                    # name -> unified_id
    producer_constraints: List[MergedConstraint]
    consumer_constraints: List[MergedConstraint]
    shared_field_names: List[str]


def build_stream_joint_system(
    producer_system: ConstraintSystemSpec,
    consumer_system: ConstraintSystemSpec,
    shared_field_names: List[str],
) -> MergedSystem:
    """Merge two constraint systems with unified stream fields.

    Shared fields get one variable ID with the intersection of domains
    (tightest bounds from either side). Non-shared fields get distinct
    IDs with namespace prefixes.

    Args:
        producer_system: Producer's variables and constraints.
        consumer_system: Consumer's variables and constraints.
        shared_field_names: Names of fields shared via the stream.

    Returns:
        MergedSystem with unified variables and remapped constraints.
    """
    variables: List[MergedVariable] = []
    var_id_map: Dict[str, int] = {}
    next_id = 0

    # Shared fields: one variable per shared field, domain = intersection
    for fname in shared_field_names:
        p_lo, p_hi, p_width = producer_system.variables.get(
            fname, (0, 0xFFFFFFFF, 32)
        )
        c_lo, c_hi, c_width = consumer_system.variables.get(
            fname, (0, 0xFFFFFFFF, 32)
        )
        merged_lo = max(p_lo, c_lo)
        merged_hi = min(p_hi, c_hi)
        merged_width = max(p_width, c_width)

        var = MergedVariable(
            unified_id=next_id,
            name=fname,
            source="shared",
            lo=merged_lo,
            hi=merged_hi,
            width=merged_width,
        )
        variables.append(var)
        var_id_map[fname] = next_id
        # Both sides refer to the same unified_id
        var_id_map[f"producer.{fname}"] = next_id
        var_id_map[f"consumer.{fname}"] = next_id
        next_id += 1

    shared_set = set(shared_field_names)

    # Producer-only variables
    for vname, (lo, hi, width) in producer_system.variables.items():
        if vname in shared_set:
            continue
        prefixed = f"producer.{vname}"
        var = MergedVariable(
            unified_id=next_id,
            name=prefixed,
            source="producer",
            lo=lo, hi=hi, width=width,
        )
        variables.append(var)
        var_id_map[prefixed] = next_id
        var_id_map[vname] = next_id  # also map unprefixed for producer constraints
        next_id += 1

    # Consumer-only variables (prefix to avoid collisions)
    for vname, (lo, hi, width) in consumer_system.variables.items():
        if vname in shared_set:
            continue
        prefixed = f"consumer.{vname}"
        var = MergedVariable(
            unified_id=next_id,
            name=prefixed,
            source="consumer",
            lo=lo, hi=hi, width=width,
        )
        variables.append(var)
        var_id_map[prefixed] = next_id
        next_id += 1

    # Remap producer constraints
    producer_constraints = _remap_constraints(
        producer_system.constraints, var_id_map, "producer"
    )

    # Remap consumer constraints
    consumer_constraints = _remap_constraints(
        consumer_system.constraints, var_id_map, "consumer"
    )

    return MergedSystem(
        variables=variables,
        var_id_map=var_id_map,
        producer_constraints=producer_constraints,
        consumer_constraints=consumer_constraints,
        shared_field_names=list(shared_field_names),
    )


def _remap_constraints(
    constraints: List[Dict[str, Any]],
    var_id_map: Dict[str, int],
    side: str,
) -> List[MergedConstraint]:
    """Remap constraint variable references to the unified ID space."""
    result: List[MergedConstraint] = []
    for c in constraints:
        remapped: Dict[str, int] = {}
        for var_name in c.get("variables", []):
            # Try direct lookup first, then prefixed
            if var_name in var_id_map:
                remapped[var_name] = var_id_map[var_name]
            else:
                prefixed = f"{side}.{var_name}"
                if prefixed in var_id_map:
                    remapped[var_name] = var_id_map[prefixed]
        result.append(MergedConstraint(
            original_constraint=c,
            remapped_var_ids=remapped,
        ))
    return result


@dataclass
class StreamJointSolveResult:
    """Result of a stream joint solve."""
    producer_values: Dict[str, int]
    consumer_values: Dict[str, int]
    shared_values: Dict[str, int]


class StreamSolver:
    """Solves stream-linked producer + consumer as a single CSP."""

    def __init__(
        self,
        icl_table: Optional[ICLTable] = None,
        solve_fn: Optional[Callable[..., Optional[Dict[str, int]]]] = None,
    ):
        """
        Args:
            icl_table: ICL table for stream partner inference.
            solve_fn: Callable(merged_system) -> dict of {unified_name: value},
                      or None if UNSAT.
        """
        self._icl = icl_table
        self._solve_fn = solve_fn

    def joint_solve(
        self,
        producer_system: ConstraintSystemSpec,
        consumer_system: ConstraintSystemSpec,
        shared_field_names: List[str],
    ) -> StreamJointSolveResult:
        """Build merged system and solve jointly.

        Raises:
            InferenceFeasibilityError: If the joint problem is UNSAT.
        """
        merged = build_stream_joint_system(
            producer_system, consumer_system, shared_field_names
        )

        # Check for empty domains on shared fields
        for var in merged.variables:
            if var.source == "shared" and var.lo > var.hi:
                raise InferenceFeasibilityError(
                    f"Empty domain for shared field '{var.name}': "
                    f"[{var.lo}, {var.hi}]"
                )

        if self._solve_fn is None:
            raise InferenceFeasibilityError("No solve function configured")

        result = self._solve_fn(merged)
        if result is None:
            raise InferenceFeasibilityError(
                "Joint solve returned UNSAT for stream pair"
            )

        # Split results by source
        shared_values: Dict[str, int] = {}
        producer_values: Dict[str, int] = {}
        consumer_values: Dict[str, int] = {}

        for var in merged.variables:
            val = result.get(var.name)
            if val is None:
                continue
            if var.source == "shared":
                shared_values[var.name] = val
            elif var.source == "producer":
                # Strip prefix for clean output
                clean_name = var.name.removeprefix("producer.")
                producer_values[clean_name] = val
            elif var.source == "consumer":
                clean_name = var.name.removeprefix("consumer.")
                consumer_values[clean_name] = val

        return StreamJointSolveResult(
            producer_values=producer_values,
            consumer_values=consumer_values,
            shared_values=shared_values,
        )

    def infer_and_solve(
        self,
        consumer_action_type: str,
        stream_field: str,
        consumer_system: ConstraintSystemSpec,
        shared_field_names: List[str],
        get_producer_system: Callable[[str], ConstraintSystemSpec],
    ) -> Tuple[str, StreamJointSolveResult]:
        """Infer stream partner from ICL and solve jointly.

        Args:
            consumer_action_type: The consumer action type.
            stream_field: The stream field name.
            consumer_system: Consumer's constraint system.
            shared_field_names: Shared field names.
            get_producer_system: Callable(action_type) -> ConstraintSystemSpec.

        Returns:
            Tuple of (selected_producer_type, solve_result).
        """
        if self._icl is None:
            raise InferenceFeasibilityError("No ICL table for stream inference")

        entry = self._icl.lookup(consumer_action_type, stream_field)
        if entry is None or not entry.candidates:
            raise InferenceFeasibilityError(
                f"No stream candidates for {consumer_action_type}.{stream_field}"
            )

        for candidate_type in entry.candidates:
            producer_system = get_producer_system(candidate_type)
            try:
                result = self.joint_solve(
                    producer_system, consumer_system, shared_field_names
                )
                return candidate_type, result
            except InferenceFeasibilityError:
                continue

        raise InferenceFeasibilityError(
            f"All stream candidates failed for "
            f"{consumer_action_type}.{stream_field}"
        )
