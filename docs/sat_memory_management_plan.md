# SAT backend memory management: what to adopt, where dv-solve is ahead

Date: 2026-05-25
Status: design proposal — companion to [[bitwuzla_adoption_plan]]

## Why this doc exists

If we adopt CaDiCaL or Kissat as a SAT backend (Phase B of the bitwuzla
adoption plan), we want to know: which of dv-solve's existing
memory-management patterns are *already better* than what those solvers do,
which patterns of theirs we should copy in wholesale, and where the
crossover would let us eventually replace them with a dv-solve-native SAT
core.

Source trees reviewed:
- `resources/kissat/` (Kissat, C)
- `resources/cadical/` (CaDiCaL, C++)
- `src/c/zsp_alloc.h`, `zsp_block_alloc`, `zsp_pool`, `zsp_stack`, `zsp_trail`

## Headline comparison

| Topic                       | Kissat                          | CaDiCaL                       | dv-solve today                       |
|-----------------------------|---------------------------------|-------------------------------|--------------------------------------|
| Learnt clause storage       | Word arena, 32-bit refs         | Heap `new`, moving-copy GC    | `db->clauses[ci]` indirection, no arena |
| Watch list                  | 4-byte tagged words, binaries inlined, blocking literal | 16-byte `Watch{clause*,blit,size}` | bare clause indices, no blocking, no inlined binaries |
| Variable / literal layout   | Pure SoA                        | Mostly SoA + small `Var` struct | Rich per-var structs; linked-list trail |
| Reallocation                | Move arena, fixup only watches when non-compact | Forwarding pointers via union in clause | Nothing moves |
| Reduction / GC              | Slide-compact arena + 3-tier glue policy | Two-space copying GC + glue tiers | None — learnt clauses live forever |
| Stack / vector primitive    | `STACK(T)` macros w/ shrink hook | `std::vector` (mostly)        | `zsp_stack` *with checkpoint marks*  |
| Allocator indirection       | Hard-wired libc                 | Hard-wired `new`/`delete[]`   | **Pluggable vtable** (`zsp_alloc_t`) |
| Trail records               | literal + reason                | literal + reason ptr          | literal + reason + prop_ref + bound delta + provenance flags + checkpoint marks |

## Where dv-solve is already ahead

These are real, observable advantages in the existing code — keep them.

1. **Pluggable allocator vtable** (`src/c/zsp_alloc.h:13-18`). Neither kissat
   (libc-hardwired) nor cadical (`new`/`delete[]`-hardwired) lets you
   redirect allocations. dv-solve can already host a SAT backend under its
   own arena, point everything at hugepages, or use NUMA-pinned allocators
   per worker. Phase B should *not* leak the backend's hardcoded
   `malloc`/`new` into dv-solve — wrap their entry points.
2. **Checkpoint-aware stack and trail** (`src/c/zsp_trail.h:79-83`
   `LevelMark`, `zsp_stack_mark_t`). The pattern "remember the stack top at
   level N, pop everything above it in O(1)" is generalized across trail,
   dynamic stack, and variable count. Kissat truncates the trail by index
   but does not generalize. *Extending the checkpoint pattern to the clause
   arena and watch arena* (i.e. record arena top per decision level) would
   give constant-time backtrack across all per-decision data — neither
   backend does this, and it is the natural dv-solve idiom.
3. **Trail records carry rich semantics**
   (`TRAIL_FLAG_SINGLETON`, `TRAIL_FLAG_FROM_CLAUSE`, `prop_ref`,
   `old_value` 32/64 — `src/c/zsp_trail.h:64-74`). Kissat's trail is
   literal+reason; cadical's is literal+Var ptr. dv-solve's trail already
   encodes bound deltas and clause-vs-propagator provenance — a SAT layer
   bolted on top can *reuse this trail* instead of duplicating its own.
4. **Pool-with-offset model** (`src/c/zsp_pool.{c,h}`). Conceptually
   identical to kissat's arena+reference scheme, but currently used only
   for IR / propagator graph. Extending it to clauses unlocks kissat-grade
   compactness while preserving the pluggable allocator.

## What to copy in (highest leverage first)

These are the actual gaps. Adopting them does not threaten the dv-solve
strengths above — they are additive.

### 1. Clause arena with 32-bit references (Kissat model)

- Replace the `Clause *db->clauses[ci]` indirection with a word arena.
- Clause "pointer" becomes a 32-bit offset (`cref_t`) into the arena.
- Watches, reasons, learned-clause queues all hold `cref_t`, never raw
  pointers.
- The arena is just a `zsp_stack`/`zsp_pool` instance — we already have it.

Win: 4-byte handles, cache-friendly propagation walks, compaction is legal
(arena can grow / move with no fixup), trivially survives `realloc`.

Constraint: 2^31 ref ceiling — for DV-scale problems this is fine.

### 2. Watch lists with blocking literal + inlined binaries

- Watch entry is 4 bytes (kissat) or 8 bytes — tagged: either an inlined
  binary clause (just the other literal) or `(clause_ref, blocking_lit)`.
- Binary clauses live entirely in the watch list; no clause object.
- Blocking literal is checked *before* the clause dereference; in the vast
  majority of propagations the dereference is skipped.

This is probably the single biggest constant-factor win in CDCL — kissat
attributes a large fraction of its speed to it.

### 3. Three-tier glue reduction + clause compaction

- Per-learnt-clause: glue, usage counter, "tier" (core/tier2/local).
- Periodic `reduce()` deletes local-tier clauses with low usage.
- Periodic `compact()` slides surviving clauses down in the arena;
  references are rewritten because they're offsets (cheap when watches
  also use offsets).

Without this, a long BMC run accumulates learnt clauses indefinitely and
RSS blows up. We have no story for this today.

### 4. Flat trail (for the SAT layer specifically)

- The current linked-list trail (32 bytes per entry with a `prev` pointer)
  is *correct* for variable-width entries but loses SoA locality on
  backtrack.
- A SAT-only sub-trail can be a flat `unsigned[]` of literals plus a
  parallel `int8[]` of reasons — backtrack becomes a `memcpy` / decrement.
- Keep the rich linked trail for the bit-vector / propagator layer; only
  the bit-blasted SAT core needs the flat one.

### 5. `STACK(T)` shrink hook (cheap one-liner)

Kissat's `SHRINK_STACK` rounds *down* to a power of two when utilization
drops; neither cadical nor dv-solve's `zsp_stack` does this. For long
sessions with many push/pop cycles (e.g. CRT randomization in a loop) this
keeps RSS bounded. ~10 lines of code in `zsp_stack`.

## Crossover techniques (dv-solve-specific)

This is where dv-solve can *beat* both backends rather than just match them.

### A. Checkpoint-keyed arena marks

Extend `LevelMark` to record:
- arena top (clauses learned this level can be reclaimed on backtrack-past)
- watch arena top (per-literal watches added this level can be popped)

This gives "throw away all learning above level N" in O(1). Kissat and
CaDiCaL both replay through every clause when forced to discard.
Practically: when CDCL learning at deeper levels turns out to be
unproductive (e.g. a heavy theory conflict invalidates the recent
sub-tree), we can throw out all of it cheaply. *Neither backend can do
this.*

### B. Unified trail across SAT and bit-vector layers

Today bit-vector propagators write to the rich trail; if we bit-blast we'd
naively get a second trail. Instead: make the SAT core's flat trail be a
*view* over a window of the rich trail, encoded via a `TRAIL_FLAG_SAT`
discriminator. The reason field reuses `prop_ref` to point either at a
clause arena offset or a propagator id. Single backtrack, single learning
loop, single explanation engine.

### C. Allocator-routed clause arena

Because `zsp_alloc_t` is a vtable, the clause arena's backing buffer can
be:
- a regular libc allocation (default),
- a hugepage-backed mapping (for very large BMC),
- a per-worker NUMA pool (for parallel portfolios),
- a checkpoint-snapshot region (so the entire SAT state can be saved /
  restored across an interactive session).

None of these are possible in stock kissat/cadical without patching their
allocators.

### D. Trail-driven incremental clause GC

Because the trail records each clause's contribution (`prop_ref`), we know
exactly which learnt clauses fired at which levels. A clause that never
fired in N restarts is a strong reduction candidate — no need for the
LBD/usage heuristic to discover it. Effectively: use the trail as the
clause activity oracle. Both backends rely on heuristic counters because
they don't have provenance.

## Suggested adoption sequence

1. **Pre-Phase-B prep** (does not require SAT yet):
   - Add `cref_t` arena layer on top of `zsp_pool`/`zsp_stack`.
   - Add shrink hook to `zsp_stack`.
   - Extend `LevelMark` with arena top — even before we have a clause
     arena, this is correctness-free since the field is unused.
2. **Phase B clause DB**: migrate existing `db->clauses[]` to the arena;
   watches still index by clause-id but the underlying storage is now
   compactable.
3. **Phase B watches**: rewrite watches as 4-byte tagged words with
   blocking literal and binary inlining.
4. **Phase B reduction**: glue + usage + tier policy, periodic `reduce()`
   and `compact()`.
5. **Phase B SAT integration**: wire CaDiCaL/Kissat *through* our
   allocator vtable. Don't let their `malloc`/`new` escape.
6. **Crossover A/B/C/D** as research items once a SAT path is working
   end-to-end. The crossover items are what would eventually let us
   replace the external solver with a dv-solve-native CDCL core that
   inherits our checkpoint/trail strengths.

## What NOT to copy

- CaDiCaL's two-space copying GC — kissat's slide-compact is simpler and
  doesn't require ~50% headroom during GC.
- CaDiCaL's `std::vector` everywhere — fine for them, but C and `zsp_stack`
  is the established dv-solve idiom.
- Hard-wired `malloc`/`new` paths from either solver — route through
  `zsp_alloc_t`.

## Key file references (for implementers)

dv-solve infrastructure to extend:
- Allocator vtable: `src/c/zsp_alloc.h:13-18`
- Block allocator (equal-size): `src/c/zsp_block_alloc.{c,h}`
- Bump pool with 32-bit offsets: `src/c/zsp_pool.{c,h}`
- Stack with checkpoint marks: `src/c/zsp_stack.c`, mark type in
  `src/c/zsp_trail.h:7`
- Trail with rich entries: `src/c/zsp_trail.h:64-83`
- Current clause DB usage: `src/c/zsp_clause_prop.c:11-130`

Kissat reference points:
- Arena + references: `resources/kissat/src/arena.{h,c}`, `reference.h`,
  `clause.h:19-37`
- Watches and per-literal vectors: `resources/kissat/src/watch.h:18-153`,
  `vector.{h,c}`
- Allocator wrappers: `resources/kissat/src/allocate.c:47-161`
- Stack macros: `resources/kissat/src/stack.h:6-148`

CaDiCaL reference points:
- Clause layout + forwarding pointer union: `resources/cadical/src/clause.hpp:30-165`
- Two-space GC arena: `resources/cadical/src/arena.{hpp,cpp}`
- Watch struct: `resources/cadical/src/watch.hpp:31-74`
- Var struct: `resources/cadical/src/var.hpp:10-18`
