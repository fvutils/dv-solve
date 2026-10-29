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

Verilator emits SV semantics (`dvs_sv.h`, see the builder SV-width decision), so the
flag stays clear and the core elaborates on first load.

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

Every construct keeps SV sizing and signedness from elaboration (`dvs_sv.h`): unsized
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

This is the one place where structure depends on run-time values.

- **Fixed size** (unpacked arrays, or a dynamic array whose size is state on this
  call): the buffer is emitted per **size class**, as a template. The element
  constraints are written once over a symbolic index, and the core instantiates them
  for a concrete size the first time it sees that size. The handle keeps a small map
  from size to compiled context.
  - Most benches have one size per site.
  - `foreach` unrolling moves from Verilator's per-call C++ loop into a one-time
    instantiation.
- **Random size** (`.size()` constrained): the buffer carries the size constraints as
  their own group, solved first. That is the same two-phase solve Verilator does
  today, but inside one `dvs_randomize` call and without rebuilding any text. The
  element phase then uses the size-class context for the chosen size.

The cap on distinct size classes per handle (queues that grow every call) is a small
LRU with a miss count; see decision 2 at the end.

## Lifecycle

```
first call on a handle:
    load buffer → elaborate (SV semantics) → apply/derive analysis
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
2. **Size classes: concrete sizes first, templates later.** V2 and the first V3 build one
   context per concrete size seen (from the capture or at run time), held in a small
   per-handle LRU (start at 4 entries; count misses). A symbolic index in the problem
   format is added only if the miss count shows queues that grow every call.
3. **Global constraints across sub-objects** map to mode bits plus per-object refs:
   a disabled var (`set_var_disabled`) is a rand_mode bit that is clear, and a null
   handle clears the mode bits of everything under it. Still unchecked against
   `V3Randomize`'s global-constraint paths; V3 verifies it, and sites it does not
   cover stay on the pipe.
4. **Both sections carry a version.** The problem section's version is checked on
   load. On a mismatch the handle refuses the buffer and the call takes the pipe
   fallback (the generated code always keeps it), so a simulation linked against a
   newer library still runs. The analysis section is ignored on a mismatch, as above.
