"""Problem partitioner: decomposes a ConstraintSystem into independent subproblems.

Uses a union-find (disjoint-set union) structure on variable names.
Variables that share at least one constraint are placed in the same
component (a "subproblem").  Variables touched by no constraint at all
are "unconstrained" and can be randomized directly without a solver call.

Usage::

    p = Partitioner()
    unconstrained, subproblems = p.partition(system)
    # unconstrained: list of variable names with no constraints
    # subproblems:   list of lists of variable names (each is a connected component)
"""
from __future__ import annotations

from typing import Dict, List, Set, Tuple, TYPE_CHECKING

if TYPE_CHECKING:
    from zuspec.dataclasses.solver.core.constraint_system import ConstraintSystem
    from zuspec.dataclasses.solver.core.constraint import Constraint


class _DSU:
    """Path-compressed, union-by-rank disjoint-set union."""

    def __init__(self) -> None:
        self._parent: Dict[str, str] = {}
        self._rank: Dict[str, int] = {}

    def add(self, x: str) -> None:
        if x not in self._parent:
            self._parent[x] = x
            self._rank[x] = 0

    def find(self, x: str) -> str:
        root = x
        while self._parent[root] != root:
            root = self._parent[root]
        # Path compression
        while self._parent[x] != root:
            self._parent[x], x = root, self._parent[x]
        return root

    def union(self, a: str, b: str) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return
        if self._rank[ra] < self._rank[rb]:
            ra, rb = rb, ra
        self._parent[rb] = ra
        if self._rank[ra] == self._rank[rb]:
            self._rank[ra] += 1


def _vars_in_constraint(constraint: "Constraint") -> Set[str]:
    """Recursively collect all variable names referenced by *constraint*."""
    from zuspec.dataclasses.solver.core.variable import Variable
    # The Constraint base class stores variables as a set of Variable objects
    # or variable name strings depending on the version.
    result: Set[str] = set()
    raw = getattr(constraint, "variables", set())
    for item in raw:
        if isinstance(item, Variable):
            result.add(item.name)
        elif isinstance(item, str):
            result.add(item)
    return result


class Partitioner:
    """Partitions a ``ConstraintSystem`` into independent subproblems."""

    def partition(
        self,
        system: "ConstraintSystem",
    ) -> Tuple[List[str], List[List[str]]]:
        """Partition *system* into unconstrained variables and subproblems.

        Returns:
            A ``(unconstrained, subproblems)`` pair where:
            - *unconstrained* is a sorted list of variable names not referenced
              by any constraint.
            - *subproblems* is a list of lists; each inner list is a sorted
              group of variable names that form one connected component.
        """
        dsu = _DSU()
        for name in system.variables:
            dsu.add(name)

        # Variables that appear in at least one constraint
        constrained: Set[str] = set()

        for constraint in system.constraints:
            var_names = _vars_in_constraint(constraint)
            var_names = var_names & system.variables.keys()
            if not var_names:
                continue
            constrained.update(var_names)
            names_list = sorted(var_names)
            # Union all variables in this constraint together
            for i in range(1, len(names_list)):
                dsu.union(names_list[0], names_list[i])

        unconstrained = sorted(
            name for name in system.variables if name not in constrained
        )

        # Build components from constrained variables
        components: Dict[str, List[str]] = {}
        for name in sorted(constrained):
            root = dsu.find(name)
            components.setdefault(root, []).append(name)

        subproblems = [sorted(names) for names in components.values()]
        # Sort subproblems by their first element for determinism
        subproblems.sort(key=lambda g: g[0])

        return unconstrained, subproblems
