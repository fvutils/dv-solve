"""State-graph pre-computation for bounded state types.

Enumerates valid states and transitions for a state type with bounded
integer fields, producing a graph usable for BFS path queries.

Design ref: solver-inference-acceleration-design.md Section 4.1
"""
from __future__ import annotations

import itertools
from collections import deque
from dataclasses import dataclass, field
from typing import (
    Callable, Dict, FrozenSet, List, Optional, Sequence, Set, Tuple,
)


class StateSpaceTooLargeError(Exception):
    """Raised when the Cartesian product of field domains exceeds the limit."""


class NoPathError(Exception):
    """Raised when BFS cannot find a path between start and goal."""


# ------------------------------------------------------------------ #
# Data structures                                                      #
# ------------------------------------------------------------------ #

@dataclass(frozen=True)
class StateNode:
    """One valid state in the state graph."""
    index: int                      # unique integer id
    values: Tuple[int, ...]         # field values in field-order

    def __hash__(self):
        return hash(self.values)


@dataclass(frozen=True)
class TransitionEdge:
    """A directed edge in the state graph."""
    src: int            # source StateNode.index
    dst: int            # destination StateNode.index
    action_type_id: int # which transition action type


@dataclass
class StateGraph:
    """Pre-computed state graph with BFS path queries."""
    nodes: List[StateNode]
    edges: List[TransitionEdge]
    adjacency: Dict[int, List[TransitionEdge]]  # src_index -> edges
    initial_index: Optional[int] = None
    field_names: Tuple[str, ...] = ()

    # ---- Node lookup by values ----
    _value_to_index: Dict[Tuple[int, ...], int] = field(
        default_factory=dict, repr=False,
    )

    def __post_init__(self):
        if not self._value_to_index:
            self._value_to_index = {n.values: n.index for n in self.nodes}

    def find_path(
        self,
        start_set: Set[int],
        goal_set: Set[int],
    ) -> List[TransitionEdge]:
        """BFS shortest path from any node in *start_set* to any in *goal_set*.

        Returns a list of TransitionEdge forming the path, or raises NoPathError.
        """
        if start_set & goal_set:
            return []  # already at goal

        visited: Set[int] = set()
        parent: Dict[int, Tuple[int, TransitionEdge]] = {}
        queue: deque = deque()

        for s in start_set:
            visited.add(s)
            queue.append(s)

        while queue:
            curr = queue.popleft()
            for edge in self.adjacency.get(curr, []):
                if edge.dst in visited:
                    continue
                visited.add(edge.dst)
                parent[edge.dst] = (curr, edge)
                if edge.dst in goal_set:
                    # Reconstruct path
                    path: List[TransitionEdge] = []
                    node = edge.dst
                    while node in parent:
                        prev, e = parent[node]
                        path.append(e)
                        node = prev
                    path.reverse()
                    return path
                queue.append(edge.dst)

        raise NoPathError(
            f"No path from {start_set} to {goal_set}"
        )

    def find_waypoint_path(
        self,
        waypoints: Sequence[Set[int]],
    ) -> List[TransitionEdge]:
        """Chain BFS across consecutive waypoint sets."""
        if len(waypoints) < 2:
            return []
        full_path: List[TransitionEdge] = []
        for i in range(len(waypoints) - 1):
            segment = self.find_path(waypoints[i], waypoints[i + 1])
            full_path.extend(segment)
        return full_path

    def match_constraint(
        self,
        predicate: Callable[[Tuple[int, ...]], bool],
    ) -> Set[int]:
        """Return indices of nodes whose values satisfy *predicate*."""
        return {n.index for n in self.nodes if predicate(n.values)}


# ------------------------------------------------------------------ #
# StateGraphBuilder                                                    #
# ------------------------------------------------------------------ #

@dataclass
class FieldDescriptor:
    """Describes one field of the state type."""
    name: str
    lo: int
    hi: int


@dataclass
class TransitionDescriptor:
    """Describes one transition action type."""
    action_type_id: int
    # Callable: (prev_values) -> list of valid next_values tuples
    # If None, all valid states are potential targets
    transition_fn: Optional[Callable] = None


class StateGraphBuilder:
    """Builds a StateGraph by enumerating states and transitions.

    Requires:
    - field_descriptors: list of FieldDescriptor for the state type
    - state_invariant: optional predicate (values_tuple) -> bool
    - transition_descriptors: list of TransitionDescriptor
    - initial_predicate: optional predicate to identify initial state(s)
    """

    def __init__(
        self,
        field_descriptors: Sequence[FieldDescriptor],
        transition_descriptors: Sequence[TransitionDescriptor],
        state_invariant: Optional[Callable[[Tuple[int, ...]], bool]] = None,
        initial_predicate: Optional[Callable[[Tuple[int, ...]], bool]] = None,
        max_states: int = 10_000,
    ):
        self.fields = list(field_descriptors)
        self.transitions = list(transition_descriptors)
        self.invariant = state_invariant
        self.initial_pred = initial_predicate
        self.max_states = max_states

    def build(self) -> StateGraph:
        """Enumerate valid states and transitions; return StateGraph."""
        field_names = tuple(f.name for f in self.fields)

        # Compute Cartesian product size
        ranges = [range(f.lo, f.hi + 1) for f in self.fields]
        total = 1
        for r in ranges:
            total *= len(r)
        if total > self.max_states:
            raise StateSpaceTooLargeError(
                f"State space has {total} candidates (max {self.max_states})"
            )

        # Enumerate valid states
        nodes: List[StateNode] = []
        value_to_idx: Dict[Tuple[int, ...], int] = {}

        for combo in itertools.product(*ranges):
            if self.invariant and not self.invariant(combo):
                continue
            idx = len(nodes)
            node = StateNode(index=idx, values=combo)
            nodes.append(node)
            value_to_idx[combo] = idx

        # Enumerate transitions
        edges: List[TransitionEdge] = []
        adjacency: Dict[int, List[TransitionEdge]] = {n.index: [] for n in nodes}

        for td in self.transitions:
            for src_node in nodes:
                if td.transition_fn is not None:
                    # Use transition function to get valid next states
                    next_values_list = td.transition_fn(src_node.values)
                    for nv in next_values_list:
                        nv_tuple = tuple(nv)
                        if nv_tuple in value_to_idx:
                            dst_idx = value_to_idx[nv_tuple]
                            edge = TransitionEdge(
                                src=src_node.index,
                                dst=dst_idx,
                                action_type_id=td.action_type_id,
                            )
                            edges.append(edge)
                            adjacency[src_node.index].append(edge)
                else:
                    # All valid states are potential targets
                    for dst_node in nodes:
                        if dst_node.index == src_node.index:
                            continue
                        edge = TransitionEdge(
                            src=src_node.index,
                            dst=dst_node.index,
                            action_type_id=td.action_type_id,
                        )
                        edges.append(edge)
                        adjacency[src_node.index].append(edge)

        # Identify initial state
        initial_index = None
        if self.initial_pred:
            for n in nodes:
                if self.initial_pred(n.values):
                    initial_index = n.index
                    break

        return StateGraph(
            nodes=nodes,
            edges=edges,
            adjacency=adjacency,
            initial_index=initial_index,
            field_names=field_names,
            _value_to_index=value_to_idx,
        )
