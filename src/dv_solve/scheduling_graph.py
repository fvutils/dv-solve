"""Schedule-block DAG analysis and staged execution plan generation.

Computes valid topological ordering over mixed sequential/concurrent/mutex
edge graphs. Detects conflicts (cycles, sequential+concurrent contradictions)
and produces a staged dispatch plan.

Design ref: schedule-buffer-stream-inference-plan.md Section 5 (B3)
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple


class ScheduleConflictError(Exception):
    """Raised when the schedule graph contains a conflict."""


@dataclass(frozen=True)
class ScheduleEdge:
    """An edge in the scheduling graph."""
    src: int              # action index
    dst: int              # action index
    kind: str             # "sequential" | "concurrent" | "mutex"
    source: str           # "buffer_bind" | "state_bind" | "resource_contention" |
                          # "explicit_sequence" | "stream_bind" | "explicit_parallel"


@dataclass
class ExecutionUnit:
    """A group of actions that execute concurrently (stream-linked or parallel)."""
    actions: List[int]    # action indices in this concurrent group
    level: int = -1       # topological level (assigned by analysis)


@dataclass
class StagedPlan:
    """Result of schedule-block analysis: actions grouped into stages."""
    stages: List[List[ExecutionUnit]]   # stages[level] = units at that level
    n_actions: int
    n_stages: int = 0

    def __post_init__(self):
        self.n_stages = len(self.stages)


class _DSU:
    """Disjoint-set union for concurrent group formation."""

    def __init__(self, n: int) -> None:
        self._parent = list(range(n))
        self._rank = [0] * n

    def find(self, x: int) -> int:
        root = x
        while self._parent[root] != root:
            root = self._parent[root]
        while self._parent[x] != root:
            self._parent[x], x = root, self._parent[x]
        return root

    def union(self, a: int, b: int) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return
        if self._rank[ra] < self._rank[rb]:
            ra, rb = rb, ra
        self._parent[rb] = ra
        if self._rank[ra] == self._rank[rb]:
            self._rank[ra] += 1


class SchedulingGraph:
    """Builds and analyses a scheduling DAG for a schedule block.

    Supports sequential, concurrent, and mutex edges. Produces a
    staged execution plan via topological level assignment.
    """

    def __init__(self) -> None:
        self._n_actions: int = 0
        self._seq_edges: List[Tuple[int, int, str]] = []  # (src, dst, source)
        self._con_pairs: List[Tuple[int, int, str]] = []  # (a, b, source)
        self._mutex_pairs: List[Tuple[int, int, str]] = []
        self._action_ids: Set[int] = set()

    def add_action(self, action_id: int) -> None:
        """Register an action in the scheduling graph."""
        self._action_ids.add(action_id)
        if action_id >= self._n_actions:
            self._n_actions = action_id + 1

    def add_edge(self, src: int, dst: int, kind: str, source: str) -> None:
        """Add a scheduling edge between two actions.

        Args:
            src: Source action index.
            dst: Destination action index.
            kind: "sequential", "concurrent", or "mutex".
            source: Origin of the edge (e.g. "buffer_bind").
        """
        if kind == "sequential":
            self._seq_edges.append((src, dst, source))
        elif kind == "concurrent":
            self._con_pairs.append((src, dst, source))
        elif kind == "mutex":
            self._mutex_pairs.append((src, dst, source))
        else:
            raise ValueError(f"Unknown edge kind: {kind}")

        self._action_ids.add(src)
        self._action_ids.add(dst)
        self._n_actions = max(self._n_actions, src + 1, dst + 1)

    def analyse(self) -> StagedPlan:
        """Run the full schedule analysis algorithm.

        Steps:
        1. Build sequential adjacency list
        2. Detect cycles (DFS)
        3. Compute transitive closure (bitset reachability)
        4. Check sequential/concurrent conflicts
        5. Form concurrent groups (union-find)
        6. Check intra-group sequential edges
        7. Topological level assignment (Kahn's algorithm on unit DAG)

        Returns:
            StagedPlan with actions grouped into execution stages.

        Raises:
            ScheduleConflictError: On cycle or seq/concurrent conflict.
        """
        n = self._n_actions
        if n == 0:
            return StagedPlan(stages=[], n_actions=0)

        # Step 1: Build sequential adjacency list
        seq_adj: List[List[int]] = [[] for _ in range(n)]
        for src, dst, _ in self._seq_edges:
            seq_adj[src].append(dst)

        # Step 2: Cycle detection via DFS
        self._detect_cycles(n, seq_adj)

        # Step 3: Transitive closure via bitset reachability
        reachable = self._compute_reachability(n, seq_adj)

        # Step 4: Sequential/concurrent conflict detection
        for a, b, source in self._con_pairs:
            if (reachable[a] >> b) & 1 or (reachable[b] >> a) & 1:
                raise ScheduleConflictError(
                    f"Actions {a} and {b} are both sequentially ordered "
                    f"and concurrent ({source})"
                )

        # Step 5: Concurrent group formation (union-find)
        dsu = _DSU(n)
        for a, b, _ in self._con_pairs:
            dsu.union(a, b)

        # Build execution units from groups
        groups: Dict[int, List[int]] = {}
        for aid in sorted(self._action_ids):
            root = dsu.find(aid)
            groups.setdefault(root, []).append(aid)

        # Step 6: Check intra-group sequential edges
        for src, dst, source in self._seq_edges:
            if dsu.find(src) == dsu.find(dst):
                raise ScheduleConflictError(
                    f"Actions {src} and {dst} are in the same concurrent "
                    f"group but have sequential edge ({source})"
                )

        # Build unit list and map action -> unit_index
        units: List[ExecutionUnit] = []
        action_to_unit: Dict[int, int] = {}
        for root, members in sorted(groups.items()):
            unit_idx = len(units)
            units.append(ExecutionUnit(actions=members))
            for m in members:
                action_to_unit[m] = unit_idx

        # Step 7: Topological level assignment (Kahn's on unit DAG)
        n_units = len(units)
        unit_adj: List[Set[int]] = [set() for _ in range(n_units)]
        in_degree = [0] * n_units

        for src, dst, _ in self._seq_edges:
            u_src = action_to_unit[src]
            u_dst = action_to_unit[dst]
            if u_src != u_dst and u_dst not in unit_adj[u_src]:
                unit_adj[u_src].add(u_dst)
                in_degree[u_dst] += 1

        # Kahn's algorithm with level tracking
        from collections import deque
        queue: deque[Tuple[int, int]] = deque()  # (unit_idx, level)
        for i in range(n_units):
            if in_degree[i] == 0:
                queue.append((i, 0))
                units[i].level = 0

        stages_dict: Dict[int, List[ExecutionUnit]] = {}
        processed = 0

        while queue:
            u_idx, level = queue.popleft()
            processed += 1
            stages_dict.setdefault(level, []).append(units[u_idx])

            for neighbor in sorted(unit_adj[u_idx]):
                in_degree[neighbor] -= 1
                if in_degree[neighbor] == 0:
                    units[neighbor].level = level + 1
                    queue.append((neighbor, level + 1))

        if processed != n_units:
            raise ScheduleConflictError(
                "Unit DAG contains a cycle (should not happen after "
                "action-level cycle check)"
            )

        # Build stages list in level order
        max_level = max(stages_dict.keys()) if stages_dict else -1
        stages: List[List[ExecutionUnit]] = []
        for lvl in range(max_level + 1):
            stages.append(stages_dict.get(lvl, []))

        return StagedPlan(
            stages=stages,
            n_actions=len(self._action_ids),
        )

    def _detect_cycles(self, n: int, adj: List[List[int]]) -> None:
        """DFS cycle detection on the sequential subgraph."""
        WHITE, GRAY, BLACK = 0, 1, 2
        color = [WHITE] * n

        def dfs(u: int) -> None:
            color[u] = GRAY
            for v in adj[u]:
                if color[v] == GRAY:
                    raise ScheduleConflictError(
                        f"Cycle detected: sequential edge {u} -> {v} "
                        f"forms a cycle"
                    )
                if color[v] == WHITE:
                    dfs(v)
            color[u] = BLACK

        for u in range(n):
            if u in self._action_ids and color[u] == WHITE:
                dfs(u)

    def _compute_reachability(
        self, n: int, adj: List[List[int]]
    ) -> List[int]:
        """Compute transitive closure using Python int as bitset.

        reachable[i] has bit j set iff node j is reachable from node i.
        """
        reachable = [0] * n

        # Process in reverse topological order for efficiency
        order = self._topo_order(n, adj)

        for u in reversed(order):
            mask = 0
            for v in adj[u]:
                mask |= (1 << v) | reachable[v]
            reachable[u] = mask

        return reachable

    def _topo_order(self, n: int, adj: List[List[int]]) -> List[int]:
        """Compute topological order via DFS post-order."""
        visited = [False] * n
        order: List[int] = []

        def dfs(u: int) -> None:
            visited[u] = True
            for v in adj[u]:
                if not visited[v]:
                    dfs(v)
            order.append(u)

        for u in range(n):
            if u in self._action_ids and not visited[u]:
                dfs(u)

        order.reverse()
        return order
