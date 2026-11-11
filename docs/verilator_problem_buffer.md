# Verilator problem buffers: design (V1)

Status: V1 final, 2026-10-29 (draft 2026-10-28). Plan item V1 of the simbench speed
plan. Decisions on the former open questions are at the end; V2 is next.

## Goal

Replace the SMT-LIB pipe between Verilator and dv-solve with a direct call:

```c
int dvs_randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes,
                  uint64_t seed);
```

- `h` is a handle to a **problem buffer** that Verilator generated at SV compile time:
  one per class `randomize()` and one per `randomize() with {}` call site.
- `refs` is a table of variable references. The solver reads the current state values
  through it and writes the results back.
- `modes` are the constraint_mode and rand_mode bits for this call.

Anything SV compilation can work out (which values are per-call state, what is
static, the structure) is passed in, not guessed. The pipe front end guesses today: it
hashes each problem to find repeats, probes a "shape" to choose an engine and makes
the choice sticky, and re-derives structure from text. None of that is ported. See
"What moves from the SMT front end" below.

## What Verilator does today (sampler-control, HEAD 880ec8843)

Everything is under `verilator-sampler-control`.

- **Constraint text is built at run time** on every call:
  - Each constraint becomes nested `AstSFormatF` format strings (`V3Randomize.cpp`
    `ConstraintExprVisitor`), with operator templates from `emitSMT()`.
  - Each `randomize()` runs `clearConstraints` → `__Vsetup_constraints` → one
    `<c>_setup_constraint` per constraint → `constraint.hard(str)`.
  - Finally `VlRandomizer::next()` sends declare, assert, check-sat and get-value
    over the pipe (`verilated_random.cpp` `nextFlat`).
- **State variables are baked into the text** as `#x…` literals of their current
  values (`editFormat`). Two calls with different state are different strings, which
  is why the pipe's identity cache misses on benches like `dist_if`.
- **constraint_mode** is a C++ `if` around each setup task. **rand_mode** turns a
  rand variable into `Cond(__Vrandmode.at(i), name, #x<value>)`, so a disabled
  variable becomes a constant.
- **dist** is lowered by Verilator. The C++ RNG picks a bucket and emits a soft
  `ConstraintIf` chain (`lowerDistConstraints`). The solver never sees a
  distribution.
- **foreach** is unrolled at run time (one `hard()` per element). **unique** becomes
  `(distinct …)` over the current elements. **randc** is enforced by excluding
  values already used. **solve-before** is a layered solve (`nextPhased`).
- **Arrays with a constrained size** take two solves:
  1. solve the sizes (`__Vsize_ok`);
  2. resize, rebuild every constraint, pin the sizes, and solve again
     (`__Vfinal_ok`).
- **Handles:** each `__Vrandwith###` function, each `<c>_setup_constraint` and each
  class's `randomize()` is a unique compile-time site, and any of them can hold a
  `static` handle. At run time there are only object pointers.

What differs between two calls on the same site:
- state values;
- mode bits;
- dist picks (today; native dist removes this);
- array sizes, foreach trip counts and element addresses;
- variable addresses (another object);
- randc history;
- captured `with` locals (these are state).

## The buffer

A buffer is a read-only, relocatable blob. It can be emitted into the generated C++ as
a `static const` array and is never modified. It has two sections.

### 1. Problem section (required)

This is a `dvs_problem_t` image. The pool format already fits:
- every expression is a pool-relative offset;
- `dvs_builder_finalize` is deterministic, so a buffer is byte-identical across
  builds;
- `DVS_PROBLEM_F_EXPLICIT` already marks problems whose widths and signedness are
  explicit (SMT-LIB semantics).
- the image is the same bytes on every host (format version 5): the header starts
  with the magic `DVSP` and `DVS_PROBLEM_FORMAT_VERSION`, every field is fixed-width,
  every node is 8-aligned with its 8-byte fields at 8-aligned offsets, and the format
  is little-endian (a big-endian runtime refuses it). `src/c/dvs_image.c` pins the
  layout with `_Static_assert`s that both libraries compile;
- `dvs_handle_new` checks an image before it uses it (header, then every reachable
  offset in range and aligned), and the writer's `dvs_problem_validate` refuses at
  write time what `dvs_solver_compile` would refuse.

Verilator sets the flag (`dvs_builder_set_explicit`): V3Width has already fixed every
width, extension and signedness, and its SMT back-end emits the same explicit form, so
the core does no SV elaboration (`dvs_sv.h`) on load. (Decided 2026-11-06, replacing
SV semantics here; see Verilator's `docs/design/dv_solve_integration.md`, Q3.)

Changes to the problem format:

| Need | Today | Change |
|---|---|---|
| Per-call state | none; values are constants | `VarSpec.kind = STATE`: an ordinary var, never a decision, pinned per call from its ref. |
| rand vs randc vs state | `is_aux` only | `kind` ∈ {RAND, RANDC, STATE, AUX} |
| constraint_mode | n/a | `ConstraintSpec.block`: index of the constraint block. A block's mode bit guards its constraints (and softs). |
| rand_mode | n/a | `VarSpec.mode_bit`: when the bit is clear, the var is treated as STATE for that call (pinned to its current value). |
| dist | `DistSpec` per var, unconditional | add an optional guard expression (dist under `if`/implication), and weight expressions that may read state (`:= w` where `w` is state). |
| unique | `AllDiffSpec` | unchanged for fixed arrays; see arrays below |
| soft + priority | `SoftSpec` | unchanged; priority = Verilator's deepest-first setup order |
| solve-before | `VarSpec.order` stage | unchanged |

State-dependent structure therefore never reaches the buffer:
- a state value is a pinned variable, not a constant;
- a mode is a guard bit, not an `if` around emission.

One buffer serves every call on a site. The compiled context is reused as long as the
buffer's handle lives (see Lifecycle).

#### Construct identity

SV constructs arrive as constructs, not as the expressions Verilator lowers them to
today. The samplers then recognise them exactly and for free, instead of
pattern-matching a lowered form (the pipe path re-derives `distinct`, dist bucket
chains and sorted chains from text).

| SV construct | Buffer form | Today on the pipe |
|---|---|---|
| `unique {..}`, `unique` over an array | `AllDiffSpec` | `distinct`, re-recognised |
| `arr.sum()` | `EXPR_SUM`, result width = element width (SV); `with (int'(item))` widens the elements first | bvadd chain |
| `$countones(x)` | `EXPR_COUNTONES` | adder tree |
| `$clog2(x)` | `EXPR_CLOG2` | ite chain |
| `x inside {a, [b:c]}` | `EXPR_IN_SET` / `EXPR_IN_RANGES` | or of compares |
| `dist` | `DistSpec` (with guard and weight expressions, above); the solver draws the bucket | C++ bucket pick + soft chain |
| `foreach (a[i]) if (i > 0) a[i] OP a[i-1]` (sorted/ordered chain) | new `ChainSpec {vars, op, signed}` | n separate compares |
| `solve a before b` | `VarSpec.order` | per-stage solve |

Every construct keeps the SV sizing and signedness V3Width gave it: unsized
constants are 32-bit, signed `%` and `/` follow the dividend's sign, and the sampler for
a construct uses the same semantics as its SMT encoding. Only `ChainSpec` is new; it
is a recognition hint, and the core also accepts the same chain as plain compares.

### 2. Analysis section (optional, versioned)

Facts SV compilation can establish, so the solver does not re-derive them on first
load. The section carries a format version. If the version does not match the library,
the solver ignores the section and computes the same facts itself. **Correctness never
depends on it**: every entry is something the core could derive, and a debug build
re-derives and compares.

Candidates, roughly in order of expected value:

1. **Variable groups:** variables and constraints that share no constraint form
   independent groups. Each is solved, cached and sampled on its own. A group that
   reads no STATE var and no mode bit has the same solution space on every call; its
   compiled form, and for enumerable domains its solution table, are built once.
2. **Bounds:** per-var `[lo, hi]` from enum ranges, `inside` and literal compares
   (`VarSpec.lo/hi` already exist; this just fills them).
3. **Rewrites already done at elaboration:** mask/field decomposition
   (`dvs_sv_rewrite_masks`), `inside` lowered to ranges, `ite` self-rewrites.
4. **Engine per group:** CDCL, bitblast or enumeration, chosen from structure (64-bit
   mul/div, wide vars and countones go to bitblast; small finite domains go to
   enumeration). Today the SMT front end decides this by probing and making the
   choice sticky (`_shape_wants_bitblast`, `verilator_cdcl`).
5. **Sampling plan:** dist variables are sampled first, then solve-before stages, then
   the rest.

## Variable references

```c
typedef struct {
    void     *addr;       /* the SV variable's storage (VlRandomVar datap) */
    uint32_t  width;      /* bits */
    uint8_t   is_signed;
    uint8_t   layout;     /* scalar (C/S/I/Q/W words), packed array, dyn array, queue */
    uint16_t  _pad;
} dvs_ref_t;
```

`refs[i]` corresponds to `var_id` i of the buffer:
- **RAND and RANDC vars** are written back.
- **STATE vars** are read before the solve.

The table is built per object by the generated code (addresses differ between
objects). It can be cached in the object, since member addresses are stable, and
rebuilt only for `with` locals and dynamic arrays.

The solver does word-level reads and writes directly. There is no per-variable
`get_value` call and no text model.

## Arrays and queues with variable size

This is the one place where structure depends on run-time values. Implemented
2026-11-07 (`src/c/dvs_tmpl.c`, `dvs_randomize_arrays`; Verilator's side of the
design is its `docs/design/dvs_dynamic_arrays.md`).

- **Sizes first.** IEEE 1800-2023 18.5.8.1 solves the size constraints before the
  iterative ones, so inside an array's `foreach` its `size()` is a state value.
  A call with constrained sizes is therefore two problems: the sizes (an
  ordinary scalar problem, its own image) and then the elements, with every size
  fixed. Between the two, the caller allocates each array at its exact size. It
  never needs a maximum.
- **The element problem keeps the arrays.** The image declares array variables
  (`dvs_builder_add_array`) and writes the element constraints once, over a loop
  index: `EXPR_ELEM`, `EXPR_ARRAY_SIZE`, `EXPR_LOOP_INDEX`, `EXPR_REDUCE`, and
  `ForeachSpec` bodies. `dvs_randomize_arrays` takes each array as a packed
  buffer of its current size.
- **Strategy by reach**, chosen when the handle loads the image:
  - **element-local**: the elements appear only as `a[i]` in top-level foreachs
    over the array, whose bodies read nothing random but that element (STATE
    values, the index, sizes, `[i]` of STATE arrays). One problem over a single
    element is compiled once and solved per element, for any size: no
    expansion, no cache miss;
  - **otherwise expanded**: the template is instantiated into a plain problem for
    the sizes it reads (index arithmetic folded; a constant index out of range
    reads 0; a random index is a choice over the elements; reductions folded),
    compiled, and kept in a per-handle LRU of 4 keyed by those sizes.

  Chain, sum and all-different propagators that would solve more classes without
  expansion can come later behind the same call.
- A RAND value reaches the caller only when the whole call succeeds.

## Parts: constraints across objects

Implemented 2026-11-07 (format version 4; `src/c/dvs_parts.c`,
`dvs_randomize_parts`; Verilator's side is its
`docs/design/dvs_global_constraints.md`).

- **Image.** A part is one class's image plus `n_slots` (its handles to other
  objects), `ExportSpec`s (its layout: export index to variable) and
  `ImportSpec`s (a variable of kind `DVS_VAR_IMPORT` bound to export *k* at a
  slot path). Expressions read an import as any variable.
- **Call.** `dvs_randomize_parts(root)` takes a graph of `dvs_part_t` (handle,
  refs, arrays, slots); each slot is null, inactive or active. The walk follows
  active slots depth first, slots in order; a part reached again (by address)
  is one part, which also ends cycles. An import active all the way is the
  exported variable; any other import is a STATE variable read through the
  slots of the call, 0 through a null slot. A constraint or soft constraint
  that reads a null import where the STATE values do not rule the read out (a
  false `ITE` condition, a false `&&` or true `||` operand, from constants and
  STATE values with `!`, `&&`, `||`) fails the call (`DVS_RANDOMIZE_NULL`).
- **Soft bands.** Parts rank by a post-order walk (the root last, highest); a
  part reached again after its walk takes a later place. Each part's distinct
  soft priorities are ranked densely and placed after those of every
  higher-ranked part, so the soft count is unchanged.
- **Cache.** The key is the walk: each part's handle, each slot's state, and the
  walk index of each active slot's object. The assembled problem is rebuilt
  through the builder (`dvs_tmpl_assemble`: variables renumbered, arrays offset)
  and kept in an LRU of 4 on the root's handle. Arrays in parts go through the
  assembled handle's template unchanged (`dvs_randomize_parts_sizes`, then
  `dvs_randomize_parts`). A root with no slots is solved directly.
- **Slot arrays** (format version 5; dynamic arrays and queues of handles). An
  array of the image takes a slot (`ArraySpec.slot`,
  `dvs_builder_add_slot_array`): its size is the number of handles, its element
  `i` is 1 if handle `i` is not null, and an import path selects an element by a
  constant or by `DVS_PATH_LOOP(level)`. The slot's `dvs_slot_t` gives the
  handles (`n_elems`, `elems`); past them, up to the array's size of the call,
  the elements are null. The walk takes the elements in index order at the
  slot's place, and the key adds each slot array's size and the phase.
  Assembly instantiates a foreach over a slot array per element (loop index and
  element bit constants, each import mapped to its element's export or a STATE
  variable); the size phase has no such foreach. At most 65536 elements walked
  per call.

## Lifecycle

```
first call on a handle:
    load buffer (EXPLICIT: no elaboration) → apply/derive analysis
    → compile one context per variable group (engine per group)
every call:
    for each group:
        skip if no RAND var is enabled (all rand_mode bits clear)
        restore checkpoint      (dvs_solver_restore; bitblast: none needed)
        pin STATE values        (dvs_solver_pin_var; bitblast: assumptions on the
                                 state bits, dvs_bbsolver_solve_assuming)
        apply constraint_mode   (guard bits as assumptions)
        sample                  (seeded; dist first)
    write RAND results through refs
    randc: record the value per var in the handle's history
handle teardown: with the class's static data (process exit), or an explicit free
```

The context stays warm for the handle's lifetime. There is no fingerprinting:
reuse is by construction. Two open performance issues apply directly here:
- **Contexts get slower when reused** (S2): `unique_64` and `packed_struct` run
  2–4× slower on a reused context than on a fresh one. With buffers, reuse is the
  only path, so this must be fixed first.
- **Diversity pass on a warm bitblast encoding** (S1): the bitblast diversity pass
  should keep its per-handle structures (cones and similar) on the warm encoding,
  not rebuild them per call.

Thread safety: one handle may be used from several processes. The context belongs to
the handle, so calls serialize on a per-handle lock. That is still finer-grained than
today's global session mutex. Per-thread contexts are a later option.

## What moves from the SMT front end

The SMT front end's Verilator-mode logic falls into three kinds:

| Kind | Examples | Fate |
|---|---|---|
| Pipe workarounds | fingerprint cache (`_problem_fingerprint`, `cached_fp`), sticky shape probe, `vlt_hash_ignore`, identity guessing across `(reset)`, `check-sat-assuming` as a diversity request | **not ported**; the handle and analysis section replace them |
| Solver policy | engine choice by structure, CDCL → bitblast escalation on unknown, the enumeration sampler (`vlt_enum`, `_check_sat_enum`), search options (`use_lcg`, `phase_save`, `fair_pick`, `max_conflicts`) | **into the core**, shared with the API and SMT2 modes (API/SMT2 parity) |
| Lowering rewrites | mask/field rewrite, `distinct` → AllDifferent, `ite` self-rewrite | case by case: those that run on `dvs_problem_t` already live in the core; text-pattern ones get a builder-level equivalent or are emitted directly by Verilator |

## Stages

- **V2 (benchmark first, no Verilator change):** extend the session capture with:
  - the site id;
  - which declared names are state;
  - mode bits;
  - dist (unlowered).

  Build buffers from that, and replay through `dvs_randomize`. This becomes the
  `dvs-standalone` number and replaces the `api-cache` byte-hash proxy. Compile time
  is reported separately as first-call cost.
- **V3:** a Verilator emitter (`V3Randomize` → buffer bytes + ref-table builder) behind
  a flag, keeping the pipe as the fallback for constructs not yet covered.
- **V4:** parity.
  - Same distinct counts and entropy as the pipe path on simbench (quality gate M5).
  - Every model validated.
  - The buffer path fuzzed against z3 by emitting the same buffer as SMT-LIB
    (`dvs_smt2_emit`).

## Decisions (former open questions)

1. **Dist goes native from the start.** State-dependent weights and guards are
   expressions in `DistSpec`; the solver draws the bucket. This removes the per-call
   soft chain and gives exact weights. Verilator's C++ bucket pick stays only on the
   pipe fallback.
2. **Size classes: a template from the start, concrete contexts behind it.**
   (Revised 2026-11-07.) Verilator writes its image at verilation time, before any
   size is known, so the image carries the array constraints as a template. The
   handle solves element-local arrays per element and expands the rest into one
   context per concrete size seen, in a per-handle LRU of 4. The earlier wording
   (concrete sizes from a run-time capture, a template only if misses demand it)
   assumed the problem was captured at run time.
3. **Global constraints across sub-objects** map to mode bits plus per-object refs:
   a disabled var (`set_var_disabled`) is a rand_mode bit that is clear, and a null
   handle clears the mode bits of everything under it. Still unchecked against
   `V3Randomize`'s global-constraint paths; V3 verifies it, and sites it does not
   cover stay on the pipe.
4. **Both sections carry a version.** The problem section's magic and
   `DVS_PROBLEM_FORMAT_VERSION` are checked on load: on a mismatch `dvs_handle_new`
   refuses the buffer. That is a hard error, with no fallback to the SMT pipe: the
   generated code checks the version at compile time (a `static_assert` against the
   header), and a mismatch at run time is fatal, telling the user to re-verilate.
   (Decided 2026-11-06, replacing the pipe fallback; see Verilator's
   `docs/design/dv_solve_integration.md`, D2/Q11.) The analysis section is ignored on
   a mismatch, as above.
