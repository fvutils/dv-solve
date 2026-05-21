"""Inference chain orchestrator for multi-step state-chain inference.

Provides graph-guided and constraint-guided solving modes for deriving
action sequences that transition between state waypoints.

Design ref: solver-inference-acceleration-design.md Section 4.2
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import (
    Any, Callable, Dict, List, Optional, Sequence, Set, Tuple, Type,
)

from .state_graph import (
    StateGraph, StateGraphBuilder, TransitionEdge, NoPathError,
)


class InferenceLimitError(Exception):
    """Raised when inference depth exceeds the configured maximum."""


class InferenceFeasibilityError(Exception):
    """Raised when no feasible action sequence can be found."""


@dataclass
class InferredAction:
    """One step in an inferred action sequence."""
    action_type_id: int
    prev_state_values: Tuple[int, ...]
    next_state_values: Tuple[int, ...]
    ordering: str = "sequential_before"


# ------------------------------------------------------------------ #
# Graph-guided mode                                                    #
# ------------------------------------------------------------------ #

def solve_state_chain_graph_guided(
    state_graph: StateGraph,
    waypoints: Sequence[Set[int]],
) -> List[InferredAction]:
    """Use pre-computed state graph to find shortest path through waypoints.

    Each waypoint is a set of valid node indices.  Returns an InferredAction
    for each edge in the resulting path.
    """
    if len(waypoints) < 2:
        return []

    try:
        path = state_graph.find_waypoint_path(list(waypoints))
    except NoPathError as e:
        raise InferenceFeasibilityError(str(e)) from e

    actions: List[InferredAction] = []
    for edge in path:
        src_vals = state_graph.nodes[edge.src].values
        dst_vals = state_graph.nodes[edge.dst].values
        actions.append(InferredAction(
            action_type_id=edge.action_type_id,
            prev_state_values=src_vals,
            next_state_values=dst_vals,
        ))
    return actions


# ------------------------------------------------------------------ #
# Constraint-guided mode                                               #
# ------------------------------------------------------------------ #

@dataclass
class TransitionCandidate:
    """A candidate transition type with its constraint builder."""
    action_type_id: int
    build_constraints: Callable  # (prev_values) -> SolveProblem or None


def solve_state_chain_constraint_guided(
    initial_values: Tuple[int, ...],
    waypoint_predicates: Sequence[Callable[[Tuple[int, ...]], bool]],
    transition_candidates: Sequence[TransitionCandidate],
    valid_states: Sequence[Tuple[int, ...]],
    max_depth: int = 20,
) -> List[InferredAction]:
    """Use backtracking search over transition candidates.

    For each waypoint, tries each transition candidate in order.
    Backtracks if a candidate doesn't lead to the next waypoint.
    """
    if not waypoint_predicates:
        return []

    actions: List[InferredAction] = []
    current_values = initial_values

    for wp_idx, wp_pred in enumerate(waypoint_predicates):
        found = False
        # BFS-like search: try to reach a state satisfying wp_pred
        visited: Set[Tuple[int, ...]] = set()
        # Queue: (current_values, path_so_far)
        from collections import deque
        queue: deque = deque()
        queue.append((current_values, []))

        while queue:
            cv, path = queue.popleft()
            if len(path) > max_depth:
                continue

            if wp_pred(cv) and len(path) > 0:
                actions.extend(path)
                current_values = cv
                found = True
                break

            if cv in visited:
                continue
            visited.add(cv)

            for tc in transition_candidates:
                for nv in valid_states:
                    nv_tuple = tuple(nv)
                    if nv_tuple in visited:
                        continue
                    action = InferredAction(
                        action_type_id=tc.action_type_id,
                        prev_state_values=cv,
                        next_state_values=nv_tuple,
                    )
                    queue.append((nv_tuple, path + [action]))

        if not found:
            if len(actions) > max_depth:
                raise InferenceLimitError(
                    f"Depth limit {max_depth} exceeded at waypoint {wp_idx}"
                )
            raise InferenceFeasibilityError(
                f"No feasible path to waypoint {wp_idx}"
            )

    return actions


# ------------------------------------------------------------------ #
# Entry point                                                          #
# ------------------------------------------------------------------ #

# Module-level cache for state graphs
_graph_cache: Dict[int, StateGraph] = {}


def solve_state_chain(
    state_graph: Optional[StateGraph],
    waypoints: Sequence[Set[int]],
    **kwargs,
) -> List[InferredAction]:
    """Select graph-guided or constraint-guided mode and solve.

    If a StateGraph is provided, uses graph-guided mode (fast BFS).
    Otherwise, falls back to constraint-guided mode.
    """
    if state_graph is not None:
        return solve_state_chain_graph_guided(state_graph, waypoints)
    raise InferenceFeasibilityError(
        "Constraint-guided mode requires explicit transition candidates"
    )


# ------------------------------------------------------------------ #
# Integrated inference orchestrator                                    #
# ------------------------------------------------------------------ #

# Imports are deferred to method bodies to avoid circular dependencies
# between buffer_inference -> structural_solver -> buffer_inference.


@dataclass
class InferenceResult:
    """Combined result from all inference passes."""
    state_chain_actions: List[InferredAction] = field(default_factory=list)
    buffer_actions: List[Any] = field(default_factory=list)       # InferredBufferAction
    stream_results: List[Any] = field(default_factory=list)       # StreamJointSolveResult
    schedule_plan: Optional[Any] = None                           # StagedPlan


class InferenceOrchestrator:
    """Top-level orchestrator that coordinates all inference patterns.

    Integrates state-chain inference (existing), buffer inference (B1),
    stream joint-solve (B2), and schedule-block ordering (B3).
    """

    def __init__(
        self,
        icl_table: Optional[Any] = None,
        state_graph: Optional[StateGraph] = None,
        solve_fn: Optional[Callable] = None,
        checkpoint_fn: Optional[Callable[[], int]] = None,
        restore_fn: Optional[Callable[[int], None]] = None,
    ):
        from .flow_constraint_store import FlowObjectConstraintStore

        self._icl = icl_table
        self._state_graph = state_graph
        self._flow_store = FlowObjectConstraintStore()
        self._solve_fn = solve_fn

        # Buffer inference engine (lazy import to avoid circular dep)
        self._buffer_engine = None
        if icl_table is not None and solve_fn is not None:
            from .buffer_inference import BufferInferenceEngine
            self._buffer_engine = BufferInferenceEngine(
                icl_table=icl_table,
                flow_store=self._flow_store,
                solve_fn=solve_fn,
                checkpoint_fn=checkpoint_fn,
                restore_fn=restore_fn,
            )

        # Stream solver
        self._stream_solver = None
        if icl_table is not None:
            from .stream_solver import StreamSolver
            self._stream_solver = StreamSolver(
                icl_table=icl_table,
                solve_fn=solve_fn,
            )

        # Scheduling graph (built per schedule block)
        self._sched_graph = None

    def infer_buffer(
        self,
        consumer_action_type: str,
        input_field: str,
        consumer_constraints: Optional[List[Dict[str, Any]]] = None,
        field_mapping: Optional[Dict[str, str]] = None,
    ) -> InferredBufferAction:
        """Infer a buffer producer. Delegates to BufferInferenceEngine."""
        if self._buffer_engine is None:
            raise InferenceFeasibilityError(
                "Buffer inference not configured (missing ICL or solve_fn)"
            )
        return self._buffer_engine.infer_single_hop(
            consumer_action_type, input_field,
            consumer_constraints, field_mapping,
        )

    def solve_stream_pair(
        self,
        producer_system: ConstraintSystemSpec,
        consumer_system: ConstraintSystemSpec,
        shared_fields: List[str],
    ) -> StreamJointSolveResult:
        """Solve a stream-linked pair jointly. Delegates to StreamSolver."""
        if self._stream_solver is None:
            raise InferenceFeasibilityError(
                "Stream solver not configured (missing ICL)"
            )
        return self._stream_solver.joint_solve(
            producer_system, consumer_system, shared_fields,
        )

    def build_schedule(
        self,
        action_ids: List[int],
        edges: List[Tuple[int, int, str, str]],
    ) -> StagedPlan:
        """Build and analyse a schedule-block DAG.

        Args:
            action_ids: List of action indices.
            edges: List of (src, dst, kind, source) tuples.

        Returns:
            StagedPlan with actions grouped into stages.
        """
        from .scheduling_graph import SchedulingGraph
        graph = SchedulingGraph()
        for aid in action_ids:
            graph.add_action(aid)
        for src, dst, kind, source in edges:
            graph.add_edge(src, dst, kind, source)
        plan = graph.analyse()
        self._sched_graph = graph
        return plan

    def record_buffer_values(
        self, flow_slot_key: str, field_values: Dict[str, int]
    ) -> None:
        """Record solved buffer values for N-producer accumulation."""
        if self._buffer_engine is not None:
            self._buffer_engine.record_solved_values(flow_slot_key, field_values)
        self._flow_store.record_solved_values(flow_slot_key, field_values)
