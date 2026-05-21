# SolveProblemBuilder API

## Motivation

The original `SolveProblem` API requires a fixed-size buffer allocated up
front.  On overflow, `zsp_pool_alloc` returns `EXPR_NULL` silently.
`SolveProblemBuilder` replaces this with a growable linked-list allocator
that never overflows and produces exact-sized buffers on `finalize()`.

The builder is the write-side API; `SolveProblem` remains the read-only
wire format consumed by `solver_compile`.

```
Builder (write)  -->  finalize()  -->  SolveProblem bytes (read)  -->  solver_compile
```

## C API

### Lifecycle

```c
// Create a builder.  block_size=0 uses the default (4096 bytes).
// alloc=NULL uses malloc.
SolveProblemBuilder *builder_create(uint32_t block_size, zsp_alloc_t *alloc);

// Reset to empty state, reusing allocated blocks.
void builder_reset(SolveProblemBuilder *b);

// Free all resources.
void builder_destroy(SolveProblemBuilder *b);

// Current bytes allocated in the virtual address space.
uint32_t builder_virtual_used(SolveProblemBuilder *b);
```

### Building Expressions

All functions mirror the existing `SolveProblem` API but never return
`EXPR_NULL` due to overflow.

```c
ExprRef builder_add_var(SolveProblemBuilder *b, uint32_t var_id,
                        uint8_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi);

ExprRef builder_expr_const(SolveProblemBuilder *b, int64_t value, uint8_t is_signed);
ExprRef builder_expr_var(SolveProblemBuilder *b, uint32_t var_id);
ExprRef builder_expr_binary(SolveProblemBuilder *b, uint32_t op,
                            ExprRef lhs, ExprRef rhs);
ExprRef builder_expr_unary(SolveProblemBuilder *b, uint32_t op, ExprRef operand);
ExprRef builder_expr_ite(SolveProblemBuilder *b,
                         ExprRef cond, ExprRef then_e, ExprRef else_e);
ExprRef builder_expr_in_range(SolveProblemBuilder *b,
                              ExprRef value, ExprRef lo, ExprRef hi);
ExprRef builder_expr_in_set(SolveProblemBuilder *b, ExprRef value,
                            uint32_t n_elems, ExprRef *elems);
ExprRef builder_expr_extend(SolveProblemBuilder *b, ExprRef operand,
                            uint8_t from, uint8_t to, uint8_t sign);
ExprRef builder_expr_extract(SolveProblemBuilder *b, ExprRef operand,
                             uint8_t hi, uint8_t lo);
ExprRef builder_add_constraint(SolveProblemBuilder *b, ExprRef root);
ExprRef builder_add_source(SolveProblemBuilder *b, uint32_t n_vars,
                           uint32_t *var_ids);
```

### Concat, Soft Constraints, Distribution



### Finalize

```c
// Produce a contiguous SolveProblem buffer.
// Returns a malloc'd buffer; caller frees with builder_free_problem().
// *out_size receives the buffer size in bytes.
SolveProblem *builder_finalize(SolveProblemBuilder *b, size_t *out_size);

// Free a buffer returned by builder_finalize.
void builder_free_problem(SolveProblemBuilder *b, void *buf, size_t size);
```

## Python API

```python
from dv_solve.builder import SolveProblemBuilder

b = SolveProblemBuilder(block_size=4096)
b.add_var(0, width=8, is_signed=False, lo=0, hi=255)
b.add_var(1, width=8, is_signed=False, lo=0, hi=255)

v0 = b.expr_var(0)
v1 = b.expr_var(1)
b.add_constraint(b.expr_binary(BIN_LTE, v0, v1))

# Finalize to ctypes buffer
buf, size = b.finalize()

# Or finalize to Python bytes
data = b.finalize_bytes()

# Reset and reuse
b.reset()
```

### Soft Constraints, Distribution, AllDifferent (Python)



## Migration Guide

### Before (fixed-size SolveProblem)

```python
sp = SolveProblem(buf_size=65536)
sp.add_var(0, width=8, ...)
# ... EXPR_NULL on overflow
```

### After (SolveProblemBuilder)

```python
b = SolveProblemBuilder()
b.add_var(0, width=8, ...)
# ... never overflows
buf, size = b.finalize()
```

The `IRTranslator` and `NativeSolverBackend` have been migrated to use
the builder.  The old `SolveProblem` Python class remains available for
backward compatibility but is considered legacy for problem construction.
