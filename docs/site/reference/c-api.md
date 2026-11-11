# C API

The C API builds a constraint problem, compiles it and solves it. It is
declared in one header, `dv_solve.h`, and implemented in `libdv_solve`.
Everything on this page is declared in that header, or in `dvs_writer.h`,
which it includes and which declares the problem-building half on its own;
the other headers installed beside them are internal and may change in any
release.

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
| problem | {c:func}`dvs_builder_finalize` | {c:func}`dvs_builder_free_problem` | the compile or add that reads it (only that call) |
| context | {c:func}`dvs_solver_new` | {c:func}`dvs_solver_free` | |
| context in your own buffer (embedded) | {c:func}`dvs_solver_create` | {c:func}`dvs_solver_destroy`, then free the buffer | |
| block allocator (embedded only) | {c:func}`dvs_block_alloc_create` | {c:func}`dvs_block_alloc_destroy` | every context using it |

A context from {c:func}`dvs_solver_new` takes its memory from an allocator
and grows as the problem needs; there is no size to choose.

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

```{c:macro} DVS_MAX_WIDTH
The widest variable, constant or expression the engine solves: 2{sup}`24`
bits. Widths are 32-bit in the API; this ceiling keeps the bit-blaster's
arithmetic inside 32 bits. A variable or constant past it is refused when it
is built ({c:macro}`DVS_EXPR_NULL`), and a wider intermediate expression
fails the solve.
```

```{c:function} uint32_t dvs_max_width(void)
{c:macro}`DVS_MAX_WIDTH` of the loaded library, for bindings that cannot read
the header.
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

```{c:function} dvs_expr_t dvs_builder_add_var(dvs_builder_t *b, uint32_t var_id, uint32_t width, uint8_t is_signed, int64_t lo, int64_t hi)
Declare a variable.

:param var_id: The variable's id. A problem with *n* variables uses the ids
  0 to *n*−1, each declared once.
:param width: Width in bits, 1 to {c:macro}`DVS_MAX_WIDTH`. Over 64 bits
  the variable is solved by the bit-blaster, and `lo`/`hi` do not bound it.
  Returns {c:macro}`DVS_EXPR_NULL` for a width of 0 or over the ceiling.
:param is_signed: 1 if the variable is signed.
:param lo: Lowest value the variable may take.
:param hi: Highest value the variable may take.
```

```{c:function} dvs_expr_t dvs_builder_add_constraint(dvs_builder_t *b, dvs_expr_t root)
Require the Boolean expression `root` to hold.
```

```{c:function} dvs_expr_t dvs_builder_add_all_different(dvs_builder_t *b, uint32_t n_vars, const uint32_t *var_ids)
Require the variables to take pairwise different values (SystemVerilog
`unique`). Any number of variables, each at most 32 bits wide; a wider one
makes compiling return {c:macro}`DVS_COMPILE_UNSUPPORTED_WIDTH`.
```

```{c:function} dvs_expr_t dvs_builder_add_soft_constraint(dvs_builder_t *b, dvs_expr_t root, uint32_t priority)
Ask for `root` to hold where possible. Priority 0 is the most important: when
soft constraints conflict, those with the highest priority number are dropped
first. See {doc}`../concepts/soft-constraints`.
```

```{c:function} int dvs_builder_set_var_order(dvs_builder_t *b, uint32_t var_id, uint8_t stage)
Put a variable in a solve-order stage (SystemVerilog `solve ... before`). The
variables of stage 1 are chosen first, then those of stage 2, and so on;
variables in stage 0 (the default) come last. Each stage is chosen among the
values that leave the constraints satisfiable, and in a seeded `fair_pick`
solve the last stage is uniform over the solutions that remain. Declare the
variable first. Returns 0, or -1 if no variable `var_id` has been declared.
```

````{c:enum} dvs_var_kind_t
What a variable is to the solve, in SystemVerilog terms. The value is stored
in the problem, so the numbers are fixed.

```{c:enumerator} DVS_VAR_RAND
`rand`: a search decision. The default.
```

```{c:enumerator} DVS_VAR_AUX
Generated by a compiler: never a search decision. Its value follows from the
constraints that define it.
```

```{c:enumerator} DVS_VAR_RANDC
`randc`: solved as {c:enumerator}`DVS_VAR_RAND`; {c:func}`dvs_randomize`
keeps the variable's history, so it takes each value once per cycle.
```

```{c:enumerator} DVS_VAR_STATE
State: read on each {c:func}`dvs_randomize` call and pinned, never sampled.
Solved as {c:enumerator}`DVS_VAR_RAND` when nothing pins it.
```

```{c:enumerator} DVS_VAR_IMPORT
Another object's variable (see [Parts](#parts)). Set by
{c:func}`dvs_builder_add_import`, not by {c:func}`dvs_builder_set_var_kind`.
```
````

```{c:function} int dvs_builder_set_var_kind(dvs_builder_t *b, uint32_t var_id, dvs_var_kind_t kind)
Set the kind of a variable. Declare the variable first.

:returns: 0, or −1 if no variable `var_id` has been declared or `kind` is not
  a {c:enum}`dvs_var_kind_t`.
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

### Arrays

A problem can declare arrays whose size is given on each call (SystemVerilog
dynamic arrays and queues). Array ids are their own space, 0 to *n*−1, apart
from variable ids. The constraints on the elements are written once, over a
loop index, and instantiated for the sizes of each call. Only a handle solves
such a problem ({c:func}`dvs_randomize_arrays`);
{c:func}`dvs_solver_compile` refuses it ({c:macro}`DVS_COMPILE_ARRAYS`).

```{c:function} int dvs_builder_add_array(dvs_builder_t *b, uint32_t array_id, uint32_t elem_width, uint8_t elem_signed, dvs_var_kind_t kind)
Declare array `array_id` with elements of `elem_width` bits (1 to 64).
`kind` is {c:enumerator}`DVS_VAR_RAND` (the elements are solved) or
{c:enumerator}`DVS_VAR_STATE` (they are read). Returns 0, or −1 for another
width or kind.
```

```{c:function} int dvs_builder_set_array_order(dvs_builder_t *b, uint32_t array_id, uint8_t stage)
Put every element of the array in solve-order stage `stage`, as
{c:func}`dvs_builder_set_var_order`.
```

```{c:function} int dvs_builder_set_array_size_random(dvs_builder_t *b, uint32_t array_id)
Make the size of a RAND array, or of a slot array, random:
{c:func}`dvs_randomize_sizes` solves it, with the constraints that read no
element, before the elements are solved at that size (IEEE 1800-2023
18.5.8.1). A size solved above 65535 fails the call
({c:enumerator}`DVS_RANDOMIZE_TOO_LARGE`).

:returns: 0, or -1 if the array is not declared, or is neither RAND nor a
  slot array.
```

```{c:function} dvs_expr_t dvs_builder_expr_array_size(dvs_builder_t *b, uint32_t array_id)
The size of the array, a 32-bit signed int.
```

```{c:function} dvs_expr_t dvs_builder_expr_loop_index(dvs_builder_t *b, uint32_t level)
The index bound at `level` (below {c:macro}`DVS_MAX_LOOP_LEVELS`) by the
enclosing {c:func}`dvs_builder_foreach_begin` or
{c:func}`dvs_builder_expr_reduce`, a 32-bit signed int.
```

```{c:function} dvs_expr_t dvs_builder_expr_elem(dvs_builder_t *b, uint32_t array_id, dvs_expr_t index)
Element `index` of the array, of its element type; 0 when the index is out of
range. A random index selects among the elements.
```

```{c:function} dvs_expr_t dvs_builder_expr_reduce(dvs_builder_t *b, dvs_binop_t op, uint32_t array_id, uint32_t level, dvs_expr_t body)
A reduction over the array (SystemVerilog `sum()`, `product()`, `and()`,
`or()`, `xor()`, with or without `with`): `body`, built with
{c:func}`dvs_builder_expr_loop_index` of `level` as the element's index,
folded with `op` ({c:enumerator}`DVS_BIN_ADD`, `MUL`, `BAND`, `BOR` or
`BXOR`). Of the type of `body`; over an empty array, the identity of `op`.
```

```{c:function} int dvs_builder_foreach_begin(dvs_builder_t *b, uint32_t array_id, uint32_t level)
Open a foreach over the array (SystemVerilog `foreach (a[i])`). Until the
matching {c:func}`dvs_builder_foreach_end`, {c:func}`dvs_builder_add_constraint`,
{c:func}`dvs_builder_add_soft_constraint` and
{c:func}`dvs_builder_add_dist_elem` add to its body, which holds once per
element with {c:func}`dvs_builder_expr_loop_index` of `level` its index.
Foreachs nest, each with a level of its own. Returns 0, or −1 if `level` is
{c:macro}`DVS_MAX_LOOP_LEVELS` or more, or already bound.
```

```{c:function} int dvs_builder_foreach_end(dvs_builder_t *b)
Close the innermost open foreach. Returns 0, or −1 if none is open.
```

```{c:function} dvs_expr_t dvs_builder_add_dist_elem(dvs_builder_t *b, uint32_t array_id, dvs_expr_t index, uint32_t n_entries, const dvs_dist_entry_t *entries)
Give element `index` of the array a weighted distribution, in the body of an
open foreach ({c:func}`dvs_builder_add_dist` is refused there).
```

```{c:macro} DVS_ARRAY_MEMBER(id)
A member of {c:func}`dvs_builder_add_all_different` that is all the elements
of array `id`.
```

```{c:macro} DVS_MAX_LOOP_LEVELS
The levels one expression may bind: 8.
```

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

```{c:function} void dvs_builder_set_explicit(dvs_builder_t *b)
Mark the problems finalized after this call EXPLICIT: every constant is
sized, and every operator's operands already have the width and signedness
it works at (SMT-LIB bit-vector semantics), so the solver skips
SystemVerilog width elaboration.
```

```{c:macro} DVS_PROBLEM_FORMAT_VERSION
The version of the problem image format. A runtime solves only images of its
own version: {c:func}`dvs_handle_new` refuses any other. The image is
little-endian and the same bytes on every host (a 32-bit writer and a 64-bit
one produce identical images); a big-endian runtime refuses it.
```

```{c:function} int dvs_problem_validate(const dvs_problem_t *p, char *why, size_t whylen)
Check a finalized problem against what the runtime solves, so a program that
writes problems (a compiler embedding them) can refuse one when it writes it
rather than have it fail at run time. It checks that the image is well formed,
then makes the checks {c:func}`dvs_solver_compile` makes:

- variable ids are 0 to n-1, each declared once, 1 to 64 bits wide
  ({c:macro}`DVS_COMPILE_BAD_VAR`, {c:macro}`DVS_COMPILE_UNSUPPORTED_WIDTH`);
- an all-different is over variables of at most 32 bits;
- every variable an expression, all-different, source group or distribution
  names is declared;
- no width over {c:macro}`DVS_MAX_WIDTH`, no reversed slice, and no expression
  nested more than 20000 deep.

It needs only libc, and is in the writer library. A problem it passes can
still fail where the solve's own stack is smaller than that nesting needs.

:param why: If not `NULL`, receives the reason for a refusal, starting with
  the compile code it stands for (`BAD_VAR: ...`, `UNSUPPORTED_WIDTH: ...`).
:param whylen: Size of `why`.
:returns: 0 if the problem passes, -1 if not.
```

```{c:function} uint32_t dvs_problem_format_version(void)
{c:macro}`DVS_PROBLEM_FORMAT_VERSION` of the linked library.
```

### Parts

A part is the problem of one class in a `randomize()` over several objects
(IEEE 1800-2023 18.5.8): its own constraints, its exports (the variables other
objects' constraints may name) and its slots (its handles to other objects,
whose parts are given on each call). A constraint on another object's variable
reads an import, bound to an export of the object at a slot path.
{c:func}`dvs_randomize_parts` solves the parts together; a part with slots is
solved by it alone.

```{c:function} void dvs_builder_set_n_slots(dvs_builder_t *b, uint32_t n_slots)
Give the part `n_slots` slots, numbered 0 to `n_slots`−1.
```

```{c:function} int dvs_builder_add_export(dvs_builder_t *b, uint32_t var_id)
Export a variable, at the next export index. A derived class's part declares
its base class's exports first, in the same order, so a part imports through
a slot without knowing the class of the object in it. Returns the export
index, or −1 if the variable is not declared or is an import.
```

```{c:function} int dvs_builder_add_import(dvs_builder_t *b, uint32_t var_id, uint32_t n_path, const uint32_t *path, uint32_t export_index)
Make a declared variable an import of export `export_index` of the object
reached through slot `path[0]` of this part, then slot `path[1]` of that
object's part, and so on. A slot that is a slot array is followed by the word
that selects its element: a constant index, or {c:macro}`DVS_PATH_LOOP` (at
most one per path). Declare it with the export's width and signedness.
Returns 0, or −1 if the variable is not declared or already an import, or
`n_path` is 0.
```

```{c:function} int dvs_builder_add_slot_array(dvs_builder_t *b, uint32_t array_id, uint32_t slot)
Declare array `array_id` as a slot array taking slot `slot`: the handles of an
SV dynamic array or queue of class handles. Its size is the number of handles
(random with {c:func}`dvs_builder_set_array_size_random`), a foreach over it
iterates them, and its element `i` is 1 if handle `i` is not null.
{c:func}`dvs_randomize_parts` instantiates a foreach over it once per element.
Returns 0, or −1 if the array could not be added.
```

```{c:macro} DVS_PATH_LOOP(level)
In an import path, after a slot array: the element at the index bound to loop
`level`, so a foreach over the slot array reads each element's export.
```

```{c:macro} DVS_API
Marks every function on this page. A program linking the static libraries
(`libdv_solve.a`, or the writer `libdv_solve_writer.a`) defines `DVS_STATIC`,
as their pkg-config `Cflags` do.
```

## Expressions

Each function returns a reference to the new expression. Expressions follow
SystemVerilog's sizing and signedness rules; see
{doc}`../concepts/problem-model`.

```{c:function} dvs_expr_t dvs_builder_expr_const(dvs_builder_t *b, int64_t value, uint8_t is_signed)
A constant. Like an unsized SystemVerilog literal it is a 32-bit signed
value, unless it doesn't fit in 32 bits.
```

```{c:function} dvs_expr_t dvs_builder_expr_const_sized(dvs_builder_t *b, int64_t value, uint8_t is_signed, uint32_t width)
A sized constant: the low `width` bits (1 to 64) of `value`, signed when
`is_signed`, like the SystemVerilog literal `8'd3`. A `width` of 0 is the
unsized constant of {c:func}`dvs_builder_expr_const`. Spell a wider constant
with {c:func}`dvs_builder_expr_const_wide`.
```

```{c:function} dvs_expr_t dvs_builder_expr_const_wide(dvs_builder_t *b, uint32_t n_limbs, const uint64_t *limbs, uint32_t width, uint8_t is_signed)
A sized constant of `width` bits (1 to {c:macro}`DVS_MAX_WIDTH`), signed when `is_signed`. Its
bit pattern is `n_limbs` = ceil(`width` / 64) 64-bit limbs, least significant
first, so `limbs[0]` holds bits 63:0; bits at and above `width` are ignored.
It is one node at any width, so use it for a constant wider than 64 bits.
Returns `EXPR_NULL` for a bad width or limb count.
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

```{c:function} dvs_expr_t dvs_builder_expr_extend(dvs_builder_t *b, dvs_expr_t operand, uint32_t from_bits, uint32_t to_bits, uint8_t sign_extend)
Extend `operand` from `from_bits` to `to_bits` bits: with zeros when
`sign_extend` is 0, with copies of the sign bit when it is 1.
```

```{c:function} dvs_expr_t dvs_builder_expr_cast(dvs_builder_t *b, dvs_expr_t operand, uint32_t to_bits, uint8_t to_signed)
A cast of `operand` to a `to_bits`-bit integer, signed when `to_signed`:
SystemVerilog's `T'(operand)`. When `to_bits` is wider than the operand, the
operand is evaluated at `to_bits`, so a 64-bit cast of the product of two
32-bit variables does not wrap at 32 bits. The value is then truncated to
`to_bits`, or extended by the operand's own signedness, and read as signed or
unsigned. Not for an explicit problem: see {c:func}`dvs_builder_expr_sv_cast`.
```

```{c:function} dvs_expr_t dvs_builder_expr_sv_cast(dvs_builder_t *b, dvs_expr_t operand, uint32_t from_bits, uint32_t to_bits, uint8_t sign_extend, uint8_t dst_signed)
The conversion of an explicit problem ({c:func}`dvs_builder_set_explicit`), in
which no operator converts its operands: the low `from_bits` bits of
`operand`, zero-extended (`sign_extend` 0) or sign-extended (1) to `to_bits`,
read as signed when `dst_signed`, else unsigned. With `from_bits` equal to
`to_bits` it only changes how the value reads, e.g. an unsigned bit pattern as
signed.
```

```{c:function} dvs_expr_t dvs_builder_expr_extract(dvs_builder_t *b, dvs_expr_t operand, uint32_t hi_bit, uint32_t lo_bit)
`operand[hi_bit:lo_bit]`.
```

```{c:function} dvs_expr_t dvs_builder_expr_concat(dvs_builder_t *b, dvs_expr_t hi, dvs_expr_t lo, uint32_t lo_width)
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

The operators above follow SystemVerilog: their operands' types decide
signedness and width. The typed operators below say it themselves, as SMT-LIB
does. Each operand is extended to the wider operand's width by the operator's
signedness (sign for the `S` forms, zero for the `U` forms and
`DVS_BIN_LSHR`), and the operator works at that width. Division by zero is
defined, as in SMT-LIB: `x UDIV 0` is all ones, `x UREM 0` is `x`, `x SDIV 0`
is −1 for `x ≥ 0` and 1 otherwise, `x SREM 0` and `x SMOD 0` are `x`.

| Enumerator | Operator |
|---|---|
| `DVS_BIN_ULT`, `DVS_BIN_ULE`, `DVS_BIN_UGT`, `DVS_BIN_UGE` | unsigned `<`, `<=`, `>`, `>=` (`bvult` …) |
| `DVS_BIN_SLT`, `DVS_BIN_SLE`, `DVS_BIN_SGT`, `DVS_BIN_SGE` | signed `<`, `<=`, `>`, `>=` (`bvslt` …) |
| `DVS_BIN_UDIV`, `DVS_BIN_UREM` | unsigned `/`, `%` (`bvudiv`, `bvurem`) |
| `DVS_BIN_SDIV`, `DVS_BIN_SREM` | signed `/` truncating, `%` with the dividend's sign (`bvsdiv`, `bvsrem`) |
| `DVS_BIN_SMOD` | signed modulo with the divisor's sign (`bvsmod`) |
| `DVS_BIN_LSHR` | logical `>>` of the value at its own width; the amount read unsigned (`bvlshr`) |

The CDCL engine does not take typed operators yet: under
{c:enumerator}`DVS_ENGINE_AUTO` a problem using them is solved on bit-blast.
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

```{c:function} dvs_ctx_t *dvs_solver_new(dvs_alloc_t *alloc)
Create a solver context. Its memory comes from `alloc` and grows with the
problem.

:param alloc: Allocator, or `NULL` for `malloc`. It must outlive the context.
:returns: The context, or `NULL` when memory runs out. Release it with
  {c:func}`dvs_solver_free`.
```

```{c:function} void dvs_solver_free(dvs_ctx_t *ctx)
Release a context made by {c:func}`dvs_solver_new`, and all its memory.
```

```{c:function} dvs_block_alloc_t *dvs_block_alloc_create(dvs_alloc_t *alloc, size_t block_size)
Create the allocator that supplies a solver context's working memory.

:param alloc: Allocator, or `NULL` for `malloc`.
:param block_size: Size of each block; 1 MiB is a good default.
```

```{c:function} void dvs_block_alloc_destroy(dvs_block_alloc_t *ba)
Free a block allocator. Destroy every context using it first.
```

```{c:function} dvs_ctx_t *dvs_solver_create(void *static_buf, size_t static_size, dvs_block_alloc_t *block_alloc)
Create a solver context inside a buffer you supply: the embedded option, for
when memory must be bounded or placed. Prefer {c:func}`dvs_solver_new`.

:param static_buf: Memory for the context, aligned as `malloc` aligns. It
  must stay valid until {c:func}`dvs_solver_destroy`.
:param static_size: Its size, at least 64 KiB.
:param block_alloc: With a block allocator, the context grows past
  `static_buf` through the allocator behind it. `NULL` keeps it inside
  `static_buf` (it still mallocs a little for its search trail): a problem
  that does not fit makes {c:func}`dvs_solver_compile` return
  {c:macro}`DVS_COMPILE_NOMEM`.
:returns: The context (at `static_buf`), or `NULL` if `static_size` is too
  small.
```

```{c:function} void dvs_solver_destroy(dvs_ctx_t *ctx)
Release the memory of a context made by {c:func}`dvs_solver_create`.
`static_buf` itself is yours to free.
```

```{c:enumerator} DVS_ENGINE_AUTO
The context chooses (see {c:func}`dvs_solver_set_engine`). The default for
{c:func}`dvs_solver_new`.
```

```{c:enumerator} DVS_ENGINE_CDCL
Solve on the CDCL engine. The default for {c:func}`dvs_solver_create`.
```

```{c:enumerator} DVS_ENGINE_BITBLAST
Solve on the bit-blast engine.
```

```{c:function} int dvs_solver_set_engine(dvs_ctx_t *ctx, int engine)
Fix the engine, before {c:func}`dvs_solver_compile`; for tests and
benchmarks. A caller need not choose: under
{c:enumerator}`DVS_ENGINE_AUTO` the context solves on CDCL, and moves to
bit-blast when CDCL cannot compile the problem whole (wide multiply/divide,
for instance) or when a CDCL search gives up (not one with its own
`time_limit_ms`). A problem with {c:enumerator}`DVS_VAR_STATE` or
{c:enumerator}`DVS_VAR_RANDC` variables, solve order, soft constraints or a
dist stays on CDCL. Pins, value exclusion, checkpoints, incremental adds,
optimization and cost guidance are CDCL's: calling one moves the context back
to CDCL for good, or fails (−1) when CDCL could not compile the problem
whole.

:returns: 0; −1 if the context has compiled already or `engine` is unknown.
```

```{c:function} int dvs_solver_engine(const dvs_ctx_t *ctx)
The engine the context solves on now: {c:enumerator}`DVS_ENGINE_CDCL` or
{c:enumerator}`DVS_ENGINE_BITBLAST` once compiled, else its preference.
```

```{c:function} int dvs_solver_compile(dvs_ctx_t *ctx, dvs_problem_t *p)
Compile problem `p` into the context. The context reads `p` only during the
call: afterwards `p` may be freed, reused or moved.

:returns: {c:macro}`DVS_COMPILE_OK`, a negative error code (see
  [Compile results](#compile-results)), or a positive count of constraints
  that could not be compiled. A solve after a positive return would ignore
  those constraints, so treat it as an error. Under
  {c:enumerator}`DVS_ENGINE_AUTO`, a problem CDCL cannot compile whole goes to
  bit-blast and returns {c:macro}`DVS_COMPILE_OK`, unless it needs CDCL.
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

```{c:member} uint8_t seq_pick
With `fair_pick`: 1 draws the variables one at a time, in search order. 0
(the default) draws them uniformly over solutions where it can. Set it for
a solve order (`solve x before y`), whose distribution is defined by the
order the values are drawn in.
```

```{c:member} uint32_t time_limit_ms
Wall-clock budget for this solve, or 0 for the default.
```

```{c:member} uint8_t canonical
1 makes the values depend only on the constraints, the pinned values and the
seed, never on what earlier solves learnt: a fixed variable order,
per-variable candidate draws, and clauses that only reject.
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
−1: memory ran out (the allocator failed, a fixed context's buffer is full,
or the problem needs more than 65,535 propagators).
```

```{c:macro} DVS_COMPILE_UNSAT
−2: compiling already showed that no solution exists.
```

```{c:macro} DVS_COMPILE_UNSUPPORTED_WIDTH
−3: the problem goes beyond a supported limit of the CDCL engine: a variable
is wider than 64 bits, an expression is wider than {c:macro}`DVS_MAX_WIDTH`
bits or nested more than 20000 deep, or an all-different constraint has a
variable wider than 32 bits. Wider bit-vectors are solved by
the bit-blaster, and through the SMT-LIB2 front end
({doc}`../guides/smt2-solver`).
```

```{c:macro} DVS_COMPILE_BAD_VAR
−4: the variable ids are not 0 to *n*−1 each declared once, or an expression
names a variable that was never declared. For
{c:func}`dvs_solver_add_constraint`: the problem declares an id the context
already uses for a variable of its own, or re-declares one at another width.
```

```{c:macro} DVS_COMPILE_ARRAYS
−5: the problem declares arrays ({c:func}`dvs_builder_add_array`). Only a
handle solves such a problem, with {c:func}`dvs_randomize_arrays`.
```

## Incremental solving

These functions change what the following solves may return, without
recompiling.

```{c:function} int dvs_solver_pin_var(dvs_ctx_t *ctx, uint32_t var_id, int64_t value)
Fix a variable to a value for the following solves, until
{c:func}`dvs_solver_reset` or a {c:func}`dvs_solver_restore` to a checkpoint
taken before the pin. Use it for SystemVerilog `rand_mode(0)` or for state
variables. The pin is hard: soft constraints give way to it.

:returns: 0, or −1 if the value contradicts the constraints. With soft
  constraints a contradiction is left to the solve, which relaxes them and is
  `DVS_SOLVE_UNSAT` if that is not enough.
```

```{c:function} int dvs_solver_exclude_value(dvs_ctx_t *ctx, uint32_t var_id, int64_t value)
Remove a value from those a variable may take. Unlike a pin, this lasts
across {c:func}`dvs_solver_reset`; excluding each solution in turn gives
SystemVerilog `randc` behaviour.

:returns: 0, or −1 if no value would remain.
```

```{c:function} int dvs_solver_add_constraint(dvs_ctx_t *ctx, dvs_problem_t *p)
Add the constraints of problem `p` to an already compiled context. As with
{c:func}`dvs_solver_compile`, `p` is read only during the call.

Variables `p` declares are new, numbered from {c:func}`dvs_solver_n_vars`:
the compile may have made variables of its own after the ones its problem
declared. `p` may also name, or re-declare at the same width, variables a
problem declared before.

:returns: The same codes as {c:func}`dvs_solver_compile`;
  {c:macro}`DVS_COMPILE_BAD_VAR` if `p` declares an id the compile took for
  a variable of its own, or re-declares a variable at another width.
```

```{c:function} uint32_t dvs_solver_n_vars(const dvs_ctx_t *ctx)
The number of variables in the context: those its problems declared and
those the compile made for itself. A problem for
{c:func}`dvs_solver_add_constraint` numbers its new variables from here.
```

```{c:function} int dvs_solver_checkpoint(dvs_ctx_t *ctx)
Save the context's state: its pins and the constraints added so far.

:returns: A checkpoint index for {c:func}`dvs_solver_restore`, or −1 if no
  more can be opened. A context from {c:func}`dvs_solver_new` grows its
  checkpoint stack (to about 65,000) and fails only when out of memory; one
  from {c:func}`dvs_solver_create` holds at most 32.
```

```{c:function} void dvs_solver_restore(dvs_ctx_t *ctx, uint32_t cp)
Return to the state saved by {c:func}`dvs_solver_checkpoint`, dropping pins
and constraints added since. Checkpoint `cp` and every later one are
discarded; take a new checkpoint to restore to the same state again.
```

The pattern for a solve under temporary pins is: checkpoint, pin, solve,
restore.

## Problem handles

A handle is one `randomize()` call site: a problem image compiled once and
solved on every {c:func}`dvs_randomize`. Variables are read and written in
place, through a {c:struct}`dvs_ref_t` per variable, so a simulator can hand
over its own storage. See `docs/verilator_problem_buffer.md` in the
repository for the design.

```{c:type} dvs_handle_t
A problem image and its compiled context.
```

````{c:struct} dvs_ref_t
Where the value of one variable lives. `refs[i]` is variable `i`.

```{c:member} void *addr
The variable's storage, or `NULL`: the variable is not read or written.
```

```{c:member} uint32_t width
Width in bits, 1 to 64.
```

```{c:member} uint8_t is_signed
A {c:enumerator}`DVS_VAR_STATE` value is sign-extended when set.
```

```{c:member} uint8_t layout
How the value is stored: {c:enumerator}`DVS_REF_SCALAR`.
```
````

```{c:enumerator} DVS_REF_SCALAR
The only {c:member}`dvs_ref_t.layout` so far: one little-endian word of 1, 2,
4 or 8 bytes, the smallest that holds `width` (Verilator's C, S, I and Q
types).
```

```{c:function} dvs_handle_t *dvs_handle_new(const void *buf, size_t size)
Make a handle for the problem image `buf` of `size` bytes, as
{c:func}`dvs_builder_finalize` returns it. The image is copied, so `buf` may
be freed or be read-only, and need not be aligned. It is compiled on the
first {c:func}`dvs_randomize`.

:returns: The handle, or `NULL` if the image is not a well-formed problem of
  {c:macro}`DVS_PROBLEM_FORMAT_VERSION`, or memory runs out. Before anything
  in the image is used, its header is checked against `size`, and every
  offset reachable from it is checked to be in range and aligned.
```

```{c:function} void dvs_handle_free(dvs_handle_t *h)
Release a handle and its compiled context. `NULL` is ignored.
```

```{c:function} uint32_t dvs_handle_n_vars(const dvs_handle_t *h)
The number of entries {c:func}`dvs_randomize` reads from `refs`: one past the
highest variable id the problem declares.
```

```{c:function} int dvs_handle_set_engine(dvs_handle_t *h, int engine)
Choose the engine, before the first {c:func}`dvs_randomize`: the handle's
context gets it ({c:func}`dvs_solver_set_engine`, which describes
{c:enumerator}`DVS_ENGINE_AUTO`, the default). A problem with
{c:enumerator}`DVS_VAR_STATE` or {c:enumerator}`DVS_VAR_RANDC` variables, soft
constraints, a `dist` or a solve-order stays on CDCL: the bit-blast engine has
no pins or value exclusion yet, and its resample ignores the others.

:returns: 0; −1 if `h` is `NULL` or has already solved, `engine` is unknown,
  or {c:enumerator}`DVS_ENGINE_BITBLAST` is asked of a problem that stays on
  CDCL.
```

```{c:function} int dvs_handle_engine(const dvs_handle_t *h)
The engine the handle solves on: {c:enumerator}`DVS_ENGINE_CDCL` or
{c:enumerator}`DVS_ENGINE_BITBLAST` once it has solved, otherwise what
{c:func}`dvs_handle_set_engine` set.
```

```{c:enumerator} DVS_RANDOMIZE_OK
The values of the RAND and RANDC variables were written.
```

```{c:enumerator} DVS_RANDOMIZE_UNSAT
No solution exists with these STATE values.
```

```{c:enumerator} DVS_RANDOMIZE_ERROR
An unsupported ref or `modes`, a compile error, or the search gave up.
```

```{c:enumerator} DVS_RANDOMIZE_TOO_LARGE
{c:func}`dvs_randomize_arrays`: the arrays the call must solve together are
too large (over 65536 elements).
{c:func}`dvs_randomize_parts`: also over 4096 objects or 65536 slot-array
elements.
```

```{c:enumerator} DVS_RANDOMIZE_NULL
{c:func}`dvs_randomize_parts`: a constraint reads a variable through a null
handle, and no guard of STATE values rules the read out.
```

```{c:function} int dvs_randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes, uint64_t seed)
Solve the handle's problem once (SystemVerilog `randomize()`). Each
{c:enumerator}`DVS_VAR_STATE` variable with a non-`NULL` ref is pinned to the
value read through it. Then the value of each RAND and RANDC variable with a
non-`NULL` ref is written through it, masked to the ref's width. On any
return but {c:enumerator}`DVS_RANDOMIZE_OK`, no ref is written.

A RANDC variable takes each value it can once per cycle. A cycle ends when no
value is left, and an UNSAT call ends every cycle: it is solved once more
before it returns {c:enumerator}`DVS_RANDOMIZE_UNSAT`. Calls on one handle
must not overlap.

:param refs: {c:func}`dvs_handle_n_vars` entries.
:param modes: `constraint_mode` / `rand_mode` bits. The buffer format has
  none yet: pass `NULL` (anything else is {c:enumerator}`DVS_RANDOMIZE_ERROR`).
:param seed: The sampling seed. The same seed and STATE values give the same
  result.
:returns: {c:enumerator}`DVS_RANDOMIZE_OK`, {c:enumerator}`DVS_RANDOMIZE_UNSAT`
  or {c:enumerator}`DVS_RANDOMIZE_ERROR`.
```

```{c:type} dvs_array_ref_t
One array of a problem with arrays: `n_elems` elements at `data`, packed,
each in the smallest of 1, 2, 4 or 8 bytes that holds the element width (as a
{c:enumerator}`DVS_REF_SCALAR` ref), little-endian. `data` may be `NULL` when
`n_elems` is 0.
```

```{c:function} uint32_t dvs_handle_n_arrays(const dvs_handle_t *h)
The number of entries {c:func}`dvs_randomize_arrays` reads from `arrays`: the
arrays the problem declares.
```

```{c:function} int dvs_randomize_sizes(dvs_handle_t *h, const dvs_ref_t *refs, uint32_t *sizes, uint64_t seed)
Solve the random sizes of a problem with arrays
({c:func}`dvs_builder_set_array_size_random`): the first part of a
randomize() call whose constraints read sizes, with the constraints that read
no element, before any element is solved (IEEE 1800-2023 18.5.8.1). Nothing
is written through `refs`. The caller then gives each array its size and
calls {c:func}`dvs_randomize_arrays`.

:param sizes: {c:func}`dvs_handle_n_arrays` entries: the current size of each
  array; on {c:enumerator}`DVS_RANDOMIZE_OK`, each random size is replaced by
  the size solved.
:returns: as {c:func}`dvs_randomize`, or
  {c:enumerator}`DVS_RANDOMIZE_TOO_LARGE` if a size solved is over 65535
  (sizes unchanged). With no random size, {c:enumerator}`DVS_RANDOMIZE_OK`.
```

```{c:function} int dvs_randomize_arrays(dvs_handle_t *h, const dvs_ref_t *refs, const dvs_array_ref_t *arrays, const uint32_t *modes, uint64_t seed)
Solve a problem with arrays once. Each array's size is its `n_elems`, fixed
for the call: as in SystemVerilog (IEEE 1800-2023 18.5.8.1), sizes are solved
before the constraints on the elements, so a caller with constrained sizes
solves them first, as a problem of their own. The elements of a STATE array
are read; those of a RAND array are written, on
{c:enumerator}`DVS_RANDOMIZE_OK` only. `refs`, `modes` and `seed` are as for
{c:func}`dvs_randomize`.

An array whose elements are constrained one at a time (only in foreachs over
it whose bodies read nothing random but the element) is solved element by
element, for any size. The rest are instantiated for the sizes of the call
and compiled; the handle keeps the last few instantiations, so a size seen
again costs no compile.

:param arrays: {c:func}`dvs_handle_n_arrays` entries.
:returns: as {c:func}`dvs_randomize`, or
  {c:enumerator}`DVS_RANDOMIZE_TOO_LARGE`.
```

### Several objects

```{c:function} uint32_t dvs_handle_n_slots(const dvs_handle_t *h)
The number of slots of a part ({c:func}`dvs_builder_set_n_slots`).
```

````{c:struct} dvs_part_t
One object of a {c:func}`dvs_randomize_parts` call. Its address is the
object's identity: an object reached through several slots is given by the
same `dvs_part_t`, and solved once.

```{c:member} dvs_handle_t *handle
The problem of the object's class.
```

```{c:member} const dvs_ref_t *refs
{c:func}`dvs_handle_n_vars` entries; those of imports are not read.
```

```{c:member} dvs_array_ref_t *arrays
{c:func}`dvs_handle_n_arrays` entries.
```

```{c:member} const dvs_slot_t *slots
{c:func}`dvs_handle_n_slots` entries.
```

The array ref of a slot array gives its size; its `data` is not read.
````

```{c:type} dvs_slot_t
One slot of an object: `state`, one of {c:enumerator}`DVS_SLOT_NULL` (the
handle is null), {c:enumerator}`DVS_SLOT_INACTIVE` (`rand_mode(0)`: the
object is not randomized, and the exports imported through it are read) and
{c:enumerator}`DVS_SLOT_ACTIVE` (the object is randomized with this one); and
`child`, the object's part. The slot of a slot array instead gives the handles
the array holds: `n_elems` slots at `elems`. Past them, up to the array's size
in the call, its elements are null (a dynamic array resized larger).
```

```{c:function} uint32_t dvs_handle_array_slot(const dvs_handle_t *h, uint32_t array_id)
The slot of array `array_id` if it is a slot array, else `DVS_NO_SLOT`.
```

```{c:function} int dvs_randomize_parts(dvs_part_t *root, uint64_t seed)
Solve `root` and the objects reached from it through active slots together:
the constraints of each, its imports bound to the exports they name. An import
through an inactive slot is the export's current value; one through a null
slot is 0, and a constraint that reads it must be guarded by STATE values that
rule the read out (`if (h != null) ... h.x ...`, the guard a STATE bit with
`!`, `&&`, `||` and conditions), or the call returns
{c:enumerator}`DVS_RANDOMIZE_NULL`. Soft constraints (IEEE 1800-2023 18.5.13)
of an object rank above those of the objects it reaches, those through a later
slot (or a later element of a slot array) above those through an earlier one,
and an object reached twice takes the higher rank. A foreach over a slot array
is instantiated per element, each with that element's imports. The problem
assembled for each shape (the classes, the slot states and the slot array
sizes) is kept on `root`'s handle; a root without slots is solved as by
{c:func}`dvs_randomize_arrays`.

:returns: as {c:func}`dvs_randomize_arrays`,
  {c:enumerator}`DVS_RANDOMIZE_NULL`, or
  {c:enumerator}`DVS_RANDOMIZE_TOO_LARGE` past 4096 objects or 65536
  slot-array elements.
```

```{c:function} int dvs_randomize_parts_sizes(dvs_part_t *root, uint64_t seed)
The random sizes of a {c:func}`dvs_randomize_parts` call, solved first, as
{c:func}`dvs_randomize_sizes` solves them: each such size replaces its array's
`n_elems`. The caller then gives each array its size and calls
{c:func}`dvs_randomize_parts`. A foreach over a slot array is not part of it.
```

## Oracle check

A debug and test mode: every answer of a context is checked by a reference
SMT solver (z3 by default), run as a child process. A solution
({c:enumerator}`DVS_SOLVE_OK`) is checked by asking the reference solver
whether the constraints hold with every variable fixed to dv-solve's value; an
UNSAT, from a solve or from {c:macro}`DVS_COMPILE_UNSAT`, by letting it solve
the problem. The problem is translated to SMT-LIB2 independently of
dv-solve's own elaboration ({c:func}`dvs_problem_write_smt2`). Every answer is
recorded in a run directory, the same format as `dv-solve-smt2`'s `DV_ORACLE`
check; `python -m dv_solve.oracle report <dir>` summarises one or many. The
oracle never changes what dv-solve answers. POSIX only; one thread at a time.

````{c:struct} dvs_oracle_opts_t
Options for {c:func}`dvs_oracle_create`. Zero-initialize it, then set the
fields you need.

```{c:member} const char *solver
`"z3"` (also NULL, the default) or `"bitwuzla"`.
```

```{c:member} const char *bin
The solver's executable, or NULL to find it on `PATH`.
```

```{c:member} const char *command
Instead of `solver` and `bin`, a whole command line of a solver that reads
SMT-LIB2 on its standard input.
```

```{c:member} const char *out_dir
The run directory: `%t` expands to the UTC time, `%p` to the process id.
NULL gives `dvs-oracle/%t-%p`.
```

```{c:member} const char *tag
Free text recorded in the run's `run.json`.
```

```{c:member} double timeout_s
Limit per query, or 0 for 10 seconds. A query the reference solver cannot
decide in time is recorded as unchecked.
```

```{c:member} int keep_all
1 also keeps `transcript.smt2`, every script sent to the reference solver.
```
````

```{c:type} dvs_oracle_t
A reference solver process and the run directory it records into.
```

```{c:function} dvs_oracle_t *dvs_oracle_create(const dvs_oracle_opts_t *opts, FILE *err)
Start a reference solver and claim a run directory. `opts` may be NULL for
the defaults; `err` (NULL: `stderr`) receives setup problems and failed
checks.

:returns: The oracle, or NULL if the solver could not be started or the run
  directory created.
```

```{c:function} void dvs_oracle_destroy(dvs_oracle_t *o)
Finalise `run.json`, stop the reference solver and free the oracle. Detach or
destroy every context using it first.
```

```{c:function} int dvs_solver_set_oracle(dvs_ctx_t *ctx, dvs_oracle_t *o, const char *label)
Check every answer of `ctx` with `o` from now on; NULL detaches. Attach before
{c:func}`dvs_solver_compile`, which hands the oracle the problem. One oracle
may serve many contexts. `label` (may be NULL) names the context in every
record.

:returns: 0, or −1 if `ctx` is NULL or memory ran out.
```

```{c:function} int dvs_oracle_last_result(const dvs_oracle_t *o)
The result of the most recent query: one of the `DVS_ORACLE_RES_*` values.
```

```{c:macro} DVS_ORACLE_RES_NONE
Nothing has been checked yet.
```

```{c:macro} DVS_ORACLE_RES_OK
The reference solver agrees.
```

```{c:macro} DVS_ORACLE_RES_BAD_MODEL
dv-solve's values violate the constraints.
```

```{c:macro} DVS_ORACLE_RES_BAD_UNSAT
dv-solve called a satisfiable problem UNSAT.
```

```{c:macro} DVS_ORACLE_RES_UNCHECKED
The reference solver could not decide in time.
```

```{c:macro} DVS_ORACLE_RES_ERROR
The reference solver failed, or the problem could not be translated.
```

```{c:macro} DVS_ORACLE_RES_SKIPPED
Not a sat or unsat answer ({c:enumerator}`DVS_SOLVE_TIMEOUT`).
```

```{c:struct} dvs_oracle_stats_t
Totals over every query so far: `queries`, then `ok`, `bad_model`,
`bad_unsat`, `unchecked`, `error` and `skipped`, and the milliseconds spent by
dv-solve (`dvs_ms`) and by the reference solver (`oracle_ms`).
```

```{c:function} void dvs_oracle_get_stats(const dvs_oracle_t *o, dvs_oracle_stats_t *st)
Fill `st` with the totals so far.
```

```{c:function} const char *dvs_oracle_run_dir(const dvs_oracle_t *o)
The run directory, valid until {c:func}`dvs_oracle_destroy`.
```

```{c:function} int dvs_problem_write_smt2(const dvs_problem_t *p, FILE *out)
Write `p` as a stand-alone SMT-LIB2 (QF_BV) script: the variables, as
`v<id>`, with their domains, the hard constraints, and `(check-sat)`. This is
the translation the oracle check uses, made under the builder's
SystemVerilog sizing rules by its own reading of them.

:returns: 0; −1 if part of the problem could not be translated (the script
  says what) or the write failed.
```

## Build information

```{c:function} const char *dvs_build_info(void)
How this copy of the library was built, as one line of `key=value` pairs:
`commit`, `dirty` (tracked files were modified at build time), `build_type`,
`optimized` (compiled with optimisation), `asan`, `ndebug` and `cc`.
Benchmark records carry it, so a result can be traced to the build that
produced it.
```

```{c:function} const char *dvs_build_library_path(void)
The file this library's code was loaded from: the shared library actually
resolved at run time (`LD_LIBRARY_PATH` takes precedence over a binary's
`RUNPATH`), or the executable when it is linked statically. `"unknown"` where
the platform cannot tell.
```
