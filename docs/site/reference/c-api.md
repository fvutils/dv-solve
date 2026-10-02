# C API

The C API builds a constraint problem, compiles it and solves it. It is
declared in one header, `dv_solve.h`, and implemented in `libdv_solve`.
Everything on this page is declared in that header; the other headers
installed beside it are internal and may change in any release.

```c
#include "dv_solve.h"
```

Link with `-ldv_solve`. {doc}`../guides/packaging` explains how to find the
include and library directories, whether dv-solve was installed from a wheel
or built from source. The header can be included from C99 or C++.

The C API follows the same rules as the Python API: see
{doc}`../concepts/problem-model` for what a problem can express and how
expressions are sized.

## Example

This program randomizes a bus packet five times. It is the same problem as
the SystemVerilog examples in {doc}`../getting-started/quickstart-verilator`
and {doc}`../guides/systemverilog-dpi`.

```{literalinclude} ../../examples/c/packet.c
:language: c
```

It prints something like:

```text
addr=000001f4 len=9 kind=13
addr=00000378 len=5 kind=6
addr=00000180 len=14 kind=1
addr=000000a8 len=2 kind=5
addr=00000a04 len=1 kind=3
```

## How the pieces fit

1. A **builder** ({c:type}`dvs_builder_t`) collects variables and
   constraints.
2. {c:func}`dvs_builder_finalize` packs them into a **problem**
   ({c:type}`dvs_problem_t`), a single block of memory.
3. A **solver context** ({c:type}`dvs_ctx_t`) compiles the problem once and
   then solves it as many times as you like, one solution per call.

Ownership is explicit:

| Object | Created by | Freed by | Must outlive |
|---|---|---|---|
| builder | {c:func}`dvs_builder_create` | {c:func}`dvs_builder_destroy` | the problems it finalized, until they are freed |
| problem | {c:func}`dvs_builder_finalize` | {c:func}`dvs_builder_free_problem` | every context compiled from it |
| block allocator | {c:func}`dvs_block_alloc_create` | {c:func}`dvs_block_alloc_destroy` | every context using it |
| context | {c:func}`dvs_solver_create`, in a buffer you supply | {c:func}`dvs_solver_destroy`, then free the buffer | |

A context is not safe to use from two threads at once.

## Types

```{c:type} dvs_builder_t
Builds a problem. Opaque.
```

```{c:type} dvs_problem_t
A finalized problem, ready to compile. Opaque.
```

```{c:type} dvs_ctx_t
A solver context holding one compiled problem. Opaque.
```

```{c:type} dvs_alloc_t
A memory allocator. Every function that takes one accepts `NULL`, meaning
`malloc` and `free`.
```

```{c:type} dvs_block_alloc_t
Supplies a solver context's working memory. Opaque.
```

```{c:type} uint32_t dvs_expr_t
A reference to an expression or a constraint inside a builder. Pass it to
other builder functions.
```

```{c:macro} DVS_EXPR_NULL
The {c:type}`dvs_expr_t` a builder function returns when it cannot allocate
memory. A program that may run out of memory should check for it.
```

## Building a problem

```{c:function} dvs_builder_t *dvs_builder_create(uint32_t block_size, dvs_alloc_t *alloc)
Create a builder.

:param block_size: Size of each memory block the builder grows by, or 0 for
  the default (4096 bytes).
:param alloc: Allocator, or `NULL` for `malloc`.
:returns: The builder, or `NULL` if memory could not be allocated.
```

```{c:function} void dvs_builder_destroy(dvs_builder_t *b)
Free the builder. Problems it finalized are not freed.
```

```{c:function} void dvs_builder_reset(dvs_builder_t *b)
Empty the builder so it can build another problem, keeping its memory.
```

```{c:function} dvs_expr_t dvs_builder_add_var(dvs_builder_t *b, uint32_t var_id, uint8_t width, uint8_t is_signed, int64_t lo, int64_t hi)
Declare a variable.

:param var_id: The variable's id. A problem with *n* variables uses the ids
  0 to *n*−1, each declared once.
:param width: Width in bits, 1 to 64.
:param is_signed: 1 if the variable is signed.
:param lo: Lowest value the variable may take.
:param hi: Highest value the variable may take.
```

```{c:function} dvs_expr_t dvs_builder_add_constraint(dvs_builder_t *b, dvs_expr_t root)
Require the Boolean expression `root` to hold.
```

```{c:function} dvs_expr_t dvs_builder_add_all_different(dvs_builder_t *b, uint32_t n_vars, const uint32_t *var_ids)
Require the variables to take pairwise different values (SystemVerilog
`unique`). Up to 16 variables, each at most 32 bits wide; beyond that,
compiling returns {c:macro}`DVS_COMPILE_UNSUPPORTED_WIDTH`.
```

```{c:function} dvs_expr_t dvs_builder_add_soft_constraint(dvs_builder_t *b, dvs_expr_t root, uint32_t priority)
Ask for `root` to hold where possible. Priority 0 is the most important: when
soft constraints conflict, those with the highest priority number are dropped
first. See {doc}`../concepts/soft-constraints`.
```

```{c:function} dvs_expr_t dvs_builder_add_dist(dvs_builder_t *b, uint32_t var_id, uint32_t n_entries, const dvs_dist_entry_t *entries)
Give a variable a weighted distribution (SystemVerilog `dist`). See
{doc}`../concepts/randomization-seeds`.
```

````{c:struct} dvs_dist_entry_t
One entry of a weighted distribution.

```{c:member} int64_t lo
Lowest value of the range.
```

```{c:member} int64_t hi
Highest value of the range; equal to `lo` for a single value.
```

```{c:member} uint32_t weight
The entry's weight.
```

```{c:member} uint8_t is_per_value
1: each value in the range gets `weight` (SystemVerilog `:=`). 0: the range
shares `weight` (SystemVerilog `:/`).
```
````

```{c:function} dvs_problem_t *dvs_builder_finalize(dvs_builder_t *b, size_t *size)
Produce the finished problem. The builder is unchanged and can go on to be
reset or destroyed.

:param size: If not `NULL`, receives the problem's size in bytes, which
  {c:func}`dvs_builder_free_problem` needs.
:returns: The problem, or `NULL` if memory could not be allocated.
```

```{c:function} void dvs_builder_free_problem(dvs_builder_t *b, dvs_problem_t *p, size_t size)
Free a problem returned by {c:func}`dvs_builder_finalize`. Destroy every
context compiled from it first.
```

## Expressions

Each function returns a reference to the new expression. Expressions follow
SystemVerilog's sizing and signedness rules; see
{doc}`../concepts/problem-model`.

```{c:function} dvs_expr_t dvs_builder_expr_const(dvs_builder_t *b, int64_t value, uint8_t is_signed)
A constant. Like an unsized SystemVerilog literal it is a 32-bit signed
value, unless it doesn't fit in 32 bits.
```

```{c:function} dvs_expr_t dvs_builder_expr_var(dvs_builder_t *b, uint32_t var_id)
A reference to variable `var_id`, which must be declared before the problem
is compiled.
```

```{c:function} dvs_expr_t dvs_builder_expr_binary(dvs_builder_t *b, dvs_binop_t op, dvs_expr_t lhs, dvs_expr_t rhs)
`lhs op rhs`.
```

```{c:function} dvs_expr_t dvs_builder_expr_unary(dvs_builder_t *b, dvs_unop_t op, dvs_expr_t operand)
`op operand`.
```

```{c:function} dvs_expr_t dvs_builder_expr_ite(dvs_builder_t *b, dvs_expr_t cond, dvs_expr_t then_e, dvs_expr_t else_e)
`cond ? then_e : else_e`. With Boolean arms, an if/else constraint.
```

```{c:function} dvs_expr_t dvs_builder_expr_in_range(dvs_builder_t *b, dvs_expr_t value, dvs_expr_t lo, dvs_expr_t hi)
`value inside {[lo:hi]}`.
```

```{c:function} dvs_expr_t dvs_builder_expr_in_set(dvs_builder_t *b, dvs_expr_t value, uint32_t n_elems, const dvs_expr_t *elems)
`value inside {elems[0], elems[1], ...}`.
```

```{c:function} dvs_expr_t dvs_builder_expr_in_ranges(dvs_builder_t *b, dvs_expr_t value, uint32_t n_ranges, const dvs_expr_t *los, const dvs_expr_t *his)
`value inside {[los[0]:his[0]], [los[1]:his[1]], ...}`.
```

```{c:function} dvs_expr_t dvs_builder_expr_extend(dvs_builder_t *b, dvs_expr_t operand, uint8_t from_bits, uint8_t to_bits, uint8_t sign_extend)
Extend `operand` from `from_bits` to `to_bits` bits: with zeros when
`sign_extend` is 0, with copies of the sign bit when it is 1.
```

```{c:function} dvs_expr_t dvs_builder_expr_extract(dvs_builder_t *b, dvs_expr_t operand, uint8_t hi_bit, uint8_t lo_bit)
`operand[hi_bit:lo_bit]`.
```

```{c:function} dvs_expr_t dvs_builder_expr_concat(dvs_builder_t *b, dvs_expr_t hi, dvs_expr_t lo, uint8_t lo_width)
`{hi, lo}`, where `lo` is `lo_width` bits wide.
```

The next three functions return constraints, not values. Pass what they
return to {c:func}`dvs_builder_add_constraint`.

```{c:function} dvs_expr_t dvs_builder_expr_sum(dvs_builder_t *b, dvs_expr_t result, uint32_t n_vars, const dvs_expr_t *var_refs)
The constraint `result == var_refs[0] + var_refs[1] + ...`.
```

```{c:function} dvs_expr_t dvs_builder_expr_countones(dvs_builder_t *b, dvs_expr_t result, dvs_expr_t operand)
The constraint `result == $countones(operand)`.
```

```{c:function} dvs_expr_t dvs_builder_expr_clog2(dvs_builder_t *b, dvs_expr_t result, dvs_expr_t operand)
The constraint `result == $clog2(operand)`.
```

### Operators

````{c:enum} dvs_binop_t
Binary operators, for {c:func}`dvs_builder_expr_binary`.

| Enumerator | Operator |
|---|---|
| `DVS_BIN_ADD`, `DVS_BIN_SUB`, `DVS_BIN_MUL` | `+`, `-`, `*` |
| `DVS_BIN_DIV`, `DVS_BIN_MOD` | `/`, `%` |
| `DVS_BIN_BAND`, `DVS_BIN_BOR`, `DVS_BIN_BXOR` | bitwise `&`, `\|`, `^` |
| `DVS_BIN_LSHIFT`, `DVS_BIN_RSHIFT` | `<<`, logical `>>` |
| `DVS_BIN_ASHR` | `>>>`: arithmetic in a signed expression, otherwise `>>` |
| `DVS_BIN_EQ`, `DVS_BIN_NEQ` | `==`, `!=` |
| `DVS_BIN_LT`, `DVS_BIN_LTE`, `DVS_BIN_GT`, `DVS_BIN_GTE` | `<`, `<=`, `>`, `>=` |
| `DVS_BIN_AND`, `DVS_BIN_OR` | logical `&&`, `\|\|` |
````

````{c:enum} dvs_unop_t
Unary operators, for {c:func}`dvs_builder_expr_unary`.

| Enumerator | Operator |
|---|---|
| `DVS_UN_NEG` | arithmetic `-` |
| `DVS_UN_NOT` | logical `!` |
| `DVS_UN_INVERT` | bitwise `~` |
````

## Solving

```{c:function} dvs_block_alloc_t *dvs_block_alloc_create(dvs_alloc_t *alloc, size_t block_size)
Create the allocator that supplies a solver context's working memory.

:param alloc: Allocator, or `NULL` for `malloc`.
:param block_size: Size of each block; 1 MiB is a good default.
```

```{c:function} void dvs_block_alloc_destroy(dvs_block_alloc_t *ba)
Free a block allocator. Destroy every context using it first.
```

```{c:function} dvs_ctx_t *dvs_solver_create(void *static_buf, size_t static_size, dvs_block_alloc_t *block_alloc)
Create a solver context inside a buffer you supply.

:param static_buf: Memory for the context, aligned as `malloc` aligns. It
  must stay valid until {c:func}`dvs_solver_destroy`.
:param static_size: Its size. 1 MiB holds large problems; if it is too small,
  {c:func}`dvs_solver_compile` returns {c:macro}`DVS_COMPILE_NOMEM`.
:param block_alloc: Supplies the rest of the context's memory.
:returns: The context (at `static_buf`), or `NULL` if `static_size` is too
  small.
```

```{c:function} void dvs_solver_destroy(dvs_ctx_t *ctx)
Release the context's memory. `static_buf` itself is yours to free.
```

```{c:function} int dvs_solver_compile(dvs_ctx_t *ctx, dvs_problem_t *p)
Compile problem `p` into the context. `p` must stay valid until the context
is destroyed.

:returns: {c:macro}`DVS_COMPILE_OK`, a negative error code (see
  [Compile results](#compile-results)), or a positive count of constraints
  that could not be compiled. A solve after a positive return would ignore
  those constraints, so treat it as an error.
```

```{c:function} dvs_result_t dvs_solver_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts)
Search for a solution.

:param opts: Options, or `NULL` for the defaults.
```

```{c:function} int64_t dvs_solver_get_value(const dvs_ctx_t *ctx, uint32_t var_id)
The value of variable `var_id` in the solution. Valid after
{c:func}`dvs_solver_solve` returns {c:enumerator}`DVS_SOLVE_OK <dvs_result_t.DVS_SOLVE_OK>`.
```

```{c:function} void dvs_solver_get_values(const dvs_ctx_t *ctx, uint32_t n, const uint32_t *var_ids, int64_t *out)
The values of `n` variables: `out[i]` receives the value of `var_ids[i]`.
```

```{c:function} void dvs_solver_reset(dvs_ctx_t *ctx)
Clear the previous solution so {c:func}`dvs_solver_solve` can run again.
Call it before every solve after the first. It also removes pins.
```

```{c:function} int dvs_solver_validate_model(dvs_ctx_t *ctx, dvs_problem_t *p, FILE *err)
Check the current solution against every constraint of `p`, the problem the
context was compiled from. Constraints it cannot evaluate (sums,
`$countones`, `$clog2` and `inside`) are skipped.

:param err: Where to describe each violation, or `NULL`.
:returns: 0 if every checked constraint holds, otherwise the number violated.
```

### Options

````{c:struct} dvs_solve_opts_t
Options for {c:func}`dvs_solver_solve`. Zero-initialize it, then set the
fields you need; a zero field takes its default.

```{c:member} uint64_t seed
Selects which solution is returned: the same seed gives the same solution.
0 continues from the context's current random state.
```

```{c:member} uint8_t fair_pick
1 breaks decision ties at random, spreading solutions evenly over the
solution space; use it for constrained-random stimulus. 0 is fastest.
```

```{c:member} uint32_t time_limit_ms
Wall-clock budget for this solve, or 0 for the default.
```

```{c:member} uint32_t max_conflicts
Conflicts allowed per restart, or 0 for no limit.
```

```{c:member} uint32_t max_restarts
Restarts allowed in total, or 0 for no limit.
```

The remaining fields, `use_phase_save`, `use_lcg` and `max_shave_iters`,
tune the search; leave them 0.
````

### Results

````{c:enum} dvs_result_t
The result of {c:func}`dvs_solver_solve`.

```{c:enumerator} DVS_SOLVE_OK
A solution was found.
```

```{c:enumerator} DVS_SOLVE_UNSAT
No solution exists.
```

```{c:enumerator} DVS_SOLVE_TIMEOUT
The search gave up before deciding; a solution may or may not exist.
```
````

### Compile results

```{c:macro} DVS_COMPILE_OK
0: the problem compiled.
```

```{c:macro} DVS_COMPILE_NOMEM
−1: the context buffer, or other memory, ran out.
```

```{c:macro} DVS_COMPILE_UNSAT
−2: compiling already showed that no solution exists.
```

```{c:macro} DVS_COMPILE_UNSUPPORTED_WIDTH
−3: the problem goes beyond a supported limit: a variable is wider than
64 bits, an expression is wider than 255 bits or nested more than 20000 deep,
or an all-different constraint has more than 16 variables or one wider than
32 bits. Wider bit-vectors are supported through the SMT-LIB2 front end
({doc}`../guides/smt2-solver`).
```

```{c:macro} DVS_COMPILE_BAD_VAR
−4: the variable ids are not 0 to *n*−1 each declared once, or an expression
names a variable that was never declared.
```

## Incremental solving

These functions change what the following solves may return, without
recompiling.

```{c:function} int dvs_solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value)
Fix a variable to a value for the following solves, until
{c:func}`dvs_solver_reset` or a {c:func}`dvs_solver_restore` to a checkpoint
taken before the pin. Use it for SystemVerilog `rand_mode(0)` or for state
variables.

:returns: 0, or −1 if the value contradicts the constraints.
```

```{c:function} int dvs_solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value)
Remove a value from those a variable may take. Unlike a pin, this lasts
across {c:func}`dvs_solver_reset`; excluding each solution in turn gives
SystemVerilog `randc` behaviour.

:returns: 0, or −1 if no value would remain.
```

```{c:function} int dvs_solver_add_constraint(dvs_ctx_t *ctx, dvs_problem_t *p)
Add the constraints of problem `p` to an already compiled context. `p` may
name the context's variables without declaring them, and must stay valid
until the context is destroyed.

:returns: The same codes as {c:func}`dvs_solver_compile`.
```

```{c:function} int dvs_solver_checkpoint(dvs_ctx_t *ctx)
Save the context's state: its pins and the constraints added so far.

:returns: A checkpoint index for {c:func}`dvs_solver_restore`, or −1 if too
  many checkpoints are open (the limit is 32).
```

```{c:function} void dvs_solver_restore(dvs_ctx_t *ctx, uint32_t cp)
Return to the state saved by {c:func}`dvs_solver_checkpoint`, dropping pins
and constraints added since. Checkpoint `cp` and every later one are
discarded; take a new checkpoint to restore to the same state again.
```

The pattern for a solve under temporary pins is: checkpoint, pin, solve,
restore.
