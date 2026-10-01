# Soft constraints

A soft constraint is one the solver keeps when it can and drops when it
conflicts with other constraints. Hard constraints (`add_constraint`) always
hold. A soft constraint is added with `add_soft_constraint(expr, priority)`.

**Priority 0 is the most important.** When soft constraints conflict, the
solver drops those with the highest priority number first, until the rest can
all be met. Note that this is the opposite of SystemVerilog, where a later
`soft` constraint takes precedence over an earlier one; give later constraints
lower numbers to get the same effect.

If the hard constraints alone cannot be met, dropping soft constraints does not
help: `solve()` returns `SOLVE_UNSAT`.

```{literalinclude} ../../examples/features.py
:language: python
:start-after: "# Soft constraints:"
:end-before: "# Weighted distribution:"
```
