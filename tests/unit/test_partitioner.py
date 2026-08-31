"""Unit tests for the Partitioner (Phase 9).

Builds ConstraintSystem objects manually and verifies that
Partitioner correctly identifies unconstrained variables and
connected-component subproblems.
"""
from __future__ import annotations

import pytest
from zuspec.be.py.solver.core.variable import Variable, VarKind
from zuspec.be.py.solver.core.domain import IntDomain
from zuspec.be.py.solver.core.constraint_system import ConstraintSystem
from zuspec.be.py.solver.core.constraints import (
    CompareConstraint,
    VariableRefConstraint,
)
from zuspec.ir.core.expr import CmpOp

from dv_solve.partitioner import Partitioner


# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

def _var(name: str) -> Variable:
    domain = IntDomain([(0, 10)], 32, False)
    return Variable(name=name, domain=domain, kind=VarKind.RAND)


def _cmp_constraint(a: Variable, b: Variable):
    """a < b comparison constraint touching variables a and b."""
    return CompareConstraint(
        left=VariableRefConstraint(a),
        op=CmpOp.Lt,
        right=VariableRefConstraint(b),
    )


def _make_system(*vars: Variable) -> ConstraintSystem:
    sys = ConstraintSystem()
    for v in vars:
        sys.add_variable(v)
    return sys


# ------------------------------------------------------------------ #
# Tests                                                                #
# ------------------------------------------------------------------ #

class TestPartitioner:
    def test_all_unconstrained(self):
        """4 rand vars, 0 constraints → all unconstrained, 0 subproblems."""
        a, b, c, d = _var("a"), _var("b"), _var("c"), _var("d")
        sys = _make_system(a, b, c, d)

        p = Partitioner()
        unconstrained, subproblems = p.partition(sys)

        assert sorted(unconstrained) == ["a", "b", "c", "d"]
        assert subproblems == []

    def test_one_constraint_two_vars(self):
        """4 rand vars, 1 constraint on 2 of them → 2 unconstrained + 1 subproblem of size 2."""
        a, b, c, d = _var("a"), _var("b"), _var("c"), _var("d")
        sys = _make_system(a, b, c, d)
        sys.add_constraint(_cmp_constraint(a, b))  # a < b touches a and b only

        p = Partitioner()
        unconstrained, subproblems = p.partition(sys)

        assert sorted(unconstrained) == ["c", "d"]
        assert len(subproblems) == 1
        assert sorted(subproblems[0]) == ["a", "b"]

    def test_chain_all_connected(self):
        """4 rand vars in a chain a<b, b<c, c<d → 0 unconstrained + 1 subproblem of 4."""
        a, b, c, d = _var("a"), _var("b"), _var("c"), _var("d")
        sys = _make_system(a, b, c, d)
        sys.add_constraint(_cmp_constraint(a, b))
        sys.add_constraint(_cmp_constraint(b, c))
        sys.add_constraint(_cmp_constraint(c, d))

        p = Partitioner()
        unconstrained, subproblems = p.partition(sys)

        assert unconstrained == []
        assert len(subproblems) == 1
        assert sorted(subproblems[0]) == ["a", "b", "c", "d"]

    def test_two_disjoint_clusters(self):
        """Two disjoint constraint clusters → 0 unconstrained + 2 independent subproblems."""
        a, b, c, d = _var("a"), _var("b"), _var("c"), _var("d")
        sys = _make_system(a, b, c, d)
        sys.add_constraint(_cmp_constraint(a, b))  # cluster 1: {a, b}
        sys.add_constraint(_cmp_constraint(c, d))  # cluster 2: {c, d}

        p = Partitioner()
        unconstrained, subproblems = p.partition(sys)

        assert unconstrained == []
        assert len(subproblems) == 2
        # Sort each subproblem and compare
        groups = sorted([sorted(sp) for sp in subproblems])
        assert groups == [["a", "b"], ["c", "d"]]

    def test_single_var_single_constraint(self):
        """Single variable with a self-constraint (like x == const) is constrained."""
        x = _var("x")
        sys = _make_system(x)
        from zuspec.be.py.solver.core.constraints import ConstantConstraint
        # CompareConstraint(x, Eq, const) touches x
        sys.add_constraint(CompareConstraint(
            left=VariableRefConstraint(x),
            op=CmpOp.Eq,
            right=ConstantConstraint(5),
        ))

        p = Partitioner()
        unconstrained, subproblems = p.partition(sys)

        assert unconstrained == []
        assert len(subproblems) == 1
        assert subproblems[0] == ["x"]

    def test_empty_system(self):
        """Empty system → nothing."""
        sys = ConstraintSystem()
        p = Partitioner()
        unconstrained, subproblems = p.partition(sys)
        assert unconstrained == []
        assert subproblems == []
