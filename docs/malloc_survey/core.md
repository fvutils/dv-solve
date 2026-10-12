> Appendix to [malloc_lifecycle_analysis.md](../malloc_lifecycle_analysis.md). Surveyed at 65acd8d; line numbers refer to that commit. Bugs listed here as "not fixed" may since be fixed: the analysis doc's bug table has their status.

# Raw heap allocation survey: dv-solve core (non-SAT, non-bitblast, non-SMT2)

Scope: `src/c/*.c` excluding `sat/`, `smt2/`, `dvs_bb*`, `dvs_aig*`, `dvs_bitblast*`,
`dvs_cube*`, `dvs_costguided*`, `dvs_oracle*`, `dvs_sat*`. Files with raw
malloc/calloc/realloc/free: dvs_contradiction.c (43 call lines), dvs_affine.c (43),
dvs_sv.c (33), dvs_dpi.c (8), dvs_shape.c (6), dvs_compile.c (6), dvs_builder.c (6),
dvs_smt2_emit.c (5), dvs_diffcycle.c (5), dvs_block_alloc.c (3), dvs_stack.c (2),
dvs_arena.c (2). No `strdup` anywhere in scope, and no `xrealloc`-style wrappers beyond
the per-file `_alloc`/`xalloc` shims noted below. Branch: feat/dynamic-pool @ 65acd8d.

## 0. The existing abstractions: where they get their memory

| abstraction | file | how it gets memory | growth | release model |
|---|---|---|---|---|
| `dvs_alloc_t` | dvs_alloc.h | vtable alloc/release/resize. Default `dvs_malloc_alloc` (dvs_block_alloc.c:10-31) = malloc/free/realloc | `dvs_alloc_resize` (dvs_block_alloc.c:37-58) uses `->resize` if set, else alloc+copy+release | individual |
| `dvs_block_alloc_t` | dvs_block_alloc.c | `DVS_ALLOC(backing, block_size)` per block **plus one extra 16-byte `_blk_rec_t` DVS_ALLOC per block** (l.122) for the all-blocks list | fixed-size blocks; free list, unlimited by default (`max_cached`=0) | `put` caches; `trim`/capped `put` releases with O(n) `_all_list_remove` (l.89-100) |
| `dvs_stack_t` | dvs_stack.c | blocks from a `dvs_block_alloc_t`; **control struct from raw `malloc`** (l.52), freed with `free` (l.68), ignoring the backing allocator | chain of blocks; a request larger than one block fails (l.94-96) | push/pop mark (LIFO), blocks go back to the block cache |
| `dvs_spool_t` | dvs_spool.c | `DVS_ALLOC` of 64 KiB slots (or exact size for >64 KiB objects); seg[]/back[] tables via `DVS_RESIZE` with doubling | segmented, never moves | mark/release; keeps up to 4 spare slots (DVS_SPOOL_SPARES) |
| `dvs_arena_t` | dvs_arena.c | `xalloc` = `DVS_ALLOC` or raw `malloc` when alloc==NULL (l.13-18) | doubling, **alloc+memcpy+free instead of DVS_RESIZE** (l.20-38), so it never gets in-place realloc | mark/reset/release; pointers unstable, offsets stable |

The solver context (dvs_ctx.c:26-47, 192-217) is built on these: ctx struct via
`DVS_ALLOC`, a 64 KiB `dvs_block_alloc_t` feeding `ctx->dynamic` (a `dvs_stack_t`,
which holds the trail), and `ctx->pool` (a `dvs_spool_t`). Var tables, level marks,
the scope log, phase-save etc. go through `ctx->alloc` with doubling growth.

**`dvs_arena_t` has no production user.** `dvs_arena_create` is called only from
tests/c/test_dvs_arena.c. It is the most natural fit for many PHASE sites below,
but it would need (a) `DVS_RESIZE` growth and (b) a pointer-returning convenience
API (current API is offset-based because pointers move on growth). For scratch
use, a non-moving variant is better; `dvs_spool_t` already is one.

## 0b. Is the core solve loop malloc-free?

**Mostly yes, with the following exceptions.** Trail entries are bump-allocated from
`ctx->dynamic` (dvs_trail.c:10), whose blocks come from the block cache, so after
warm-up no malloc. `dvs_search.c` and `dvs_propagate.c`, `dvs_explain.c`,
`dvs_conflict.c`, `dvs_nogood.c`, `dvs_shave.c`, `dvs_clause_prop.c` contain no raw
allocator calls (dvs_prop_templates.c "free" hits are all comments). What is
reachable per decision, propagation or conflict:

| site | reachable | what | verdict |
|---|---|---|---|
| dvs_placement.c:648 / 655 | **per propagator fire** of NoOverlap2D with >64 rectangles (`NO_STACK_RECTS`=64, l.508) | `DVS_ALLOC(ctx->alloc, nr*32 B)` + `DVS_RELEASE` on every fire, i.e. malloc/free per propagation | **CHURN in the hot loop.** The only true per-propagation malloc found. Size is fixed per propagator, so allocate it once in the propagator record at build time (spool), or keep a ctx-owned grow-only scratch buffer |
| dvs_search.c:1997 `_insert_hole` | per value exclusion during search | `dvs_spool_alloc` of a HoleEntry | already pooled (SCOPE: reclaimed by checkpoint restore) |
| dvs_lcg.c:128 `_arena_alloc` (used by `clause_db_add` l.165 and `_add_watch` l.133) | per learnt clause | spool bump | pooled, but **never reclaimed within a solve**: `clause_db_gc` (l.185-196) only NULLs pointers; bytes come back only at `lcg_reset` (l.331). Memory grows monotonically with conflicts |
| dvs_lcg.c:157 | per learnt clause | `clauses[]` grow, doubling | VECTOR, amortized, fine |
| dvs_lcg.c:443 | per conflict analysis | `mk_ent` grow, ×1.5 + 64 | VECTOR, amortized, fine |
| dvs_lcg.c:533-541 | per conflict analysis | undo log, doubling from 256, alloc+copy+release | VECTOR, amortized, fine |
| dvs_search.c:945, 1032, 1085, 1103; dvs_ctx.c:367 | per solve start | box_val, LCG ctx, keep_val, value_fails, phase_save; grown only when capacity is short | effectively once per context (OWNED, grow-only) |
| dvs_checkpoint.c:148-182 | **per `dvs_solver_restore`**, when holes were added inside the scope; DPI's `dvs_dpi_solve_h` restores on every call | `_HoleCarry` vector, doubling from 16, DVS_RESIZE then DVS_RELEASE at end | SCRATCH churn per randomize; use a ctx-retained grow-only scratch |
| dvs_ctx.c:380 `dvs_scope_log_bound` | per bound change logged under a checkpoint | doubling from 64 | VECTOR, fine |

So the decision/propagation loop is malloc-free except **NoOverlap2D with >64 rects**
(every fire) and, per conflict, amortized LCG vector growth. The learnt-clause spool
is never reclaimed until `lcg_reset`.

## 1. Site table

Frequency key: **hot** = per propagation/decision; **per-rand** = per Verilator/DPI
randomize (incremental add / restore); **per-compile** = once per `dvs_solver_compile`;
**per-bb-solve** = per bitblast solve; **diag** = contradiction/diagnostic API only
(callers are tests/unit/test_contra_*.py, no production C caller); **dump** = SMT2
emitter (oracle and `dvs_problem_write_smt2`).

### dvs_contradiction.c (all diag)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_contradiction.c:61 | `_build_gated_problem` | malloc(sizeof(problem)+pool_used+n_hard*64+4096) | gated copy of the problem | PHASE (one per analysis) | diag, 1 per call | `_gated_free` l.125 (or l.64/69/96 on error) | arena per analysis |
| dvs_contradiction.c:68 | `_build_gated_problem` | malloc(n_hard*4) | cid_map | PHASE | diag | `_gated_free` l.125; l.96 on error | same arena as above |
| dvs_contradiction.c:229 | `_quickxplain` | malloc((n_bg+n_c1)*4) | new background set | SCRATCH, LIFO through recursion | diag, per recursion node (each node also runs a full solve) | l.249 same frame | scratch stack mark/release (`dvs_stack`); cost negligible next to the solve |
| dvs_contradiction.c:240 | `_quickxplain` | malloc((n_bg+n_mus2)*4) | second background set | SCRATCH | diag | l.247 | as above. Note: size can be 0 (`n_bg==0 && n_mus2==0`); `malloc(0)` may return NULL on non-glibc and then the c1 recursion is silently skipped |
| dvs_contradiction.c:370 | `contra_analyze_unsat` (level-0 path) | malloc(bsz) | sub-problem without constraint i | CHURN: malloc, full pool memcpy, new solver ctx, free, **per constraint** (≤256) | diag | l.419 (l.374 on error) | `bsz` is identical every iteration: hoist one buffer out of the loop, or copy the pool once and reuse via mark/release |
| dvs_contradiction.c:430 | `contra_analyze_unsat` | malloc(mus_n*4) | result MUS ids | OWNED (returned) | diag | `contra_result_free` l.700 | leave (API-owned) |
| dvs_contradiction.c:470 | `contra_analyze_unsat` | malloc(core_n*4) | result MUS ids (skip_minimization) | OWNED | diag | l.700 | leave |
| dvs_contradiction.c:497 | `contra_analyze_unsat` | malloc(mus_n*4) | result MUS ids | OWNED | diag | l.700 | leave |
| dvs_contradiction.c:513 | `contra_analyze_unsat` | malloc(mus_size*sizeof(Relax)) | relaxations | OWNED | diag | l.522 on failure, else l.701 | leave |
| dvs_contradiction.c:565 | `_format_text` | malloc(512+256*(mus+relax)) | text proof | OWNED | diag | l.700 | leave, but see bug B-1 |
| dvs_contradiction.c:639 | `_format_json` | malloc(same estimate) | JSON proof | OWNED | diag | l.701 | leave, but see bug B-1 |
| dvs_contradiction.c:727 | `contra_explain_soft` | calloc(n_relaxed, entry) | result entries | OWNED | diag | `contra_soft_diag_free` l.993 | leave |
| dvs_contradiction.c:748 | `contra_explain_soft` | malloc(buf_size) | sub-problem: all hard constraints plus one soft | CHURN per relaxed soft (≤64) | diag | l.837 (l.752 on error) | one reusable buffer; size differs only by the +1 |
| dvs_contradiction.c:805 | `contra_explain_soft` | malloc(mus*4) | entry conflict ids, copied out of sub_result | OWNED | diag | l.990 | **steal** `sub_result.mus_constraint_ids` instead of copy+free |
| dvs_contradiction.c:818 | `contra_explain_soft` | malloc(strlen+1) + strcpy | proof text copy (a hand-rolled strdup) | OWNED | diag | l.990 | steal `sub_result.proof_text` |
| dvs_contradiction.c:826 | `contra_explain_soft` | malloc(n*sizeof(Relax)) | hard relax copy | OWNED | diag | l.991 | steal `sub_result.relaxations` |
| dvs_contradiction.c:859 | `contra_explain_soft` (alternatives) | malloc(bsz) | sub-problem: conflict set plus S_j | CHURN, O(n_relaxed²) iterations, each with a new solver ctx | diag | l.922 (l.862 on error) | one reusable buffer |
| dvs_contradiction.c:929 | `contra_explain_soft` | malloc(n_alt*4) | alternative ids | OWNED | diag | l.991 | leave |
| dvs_contradiction.c:967 | `contra_explain_soft` (T-36) | realloc(proof_text, len+80) | append a "shares conflict" note | VECTOR, +80 per matching pair (small fixed increment) | diag, O(n²) | l.990 | fine at diag scale; a `DvsOBuf`-style doubling buffer if it ever matters |
| dvs_contradiction.c:1098 | `_build_relaxed_subproblem` | malloc(buf_size) | relaxed sub-problem: full pool copy plus a rewritten constant | CHURN: per binary-search step (≤64) × per MUS constraint, each with a new solver ctx | diag | caller l.1254 / l.1293; l.1102/1144/1155 on error | copy the base pool **once**, take a mark, and per step release to the mark and append only the new const/root (SCOPE); or at least reuse one buffer |

### dvs_affine.c (per-bb-solve: called from dvs_bbsolver.c:2527 `_solve_with_sim`, up to twice per solve via `dvs_affine_check` l.586-596)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_affine.c:313-315 | `_find_tables` | 3× calloc(n1) (u8, u64, u64) | cone mark, two support signatures | SCRATCH | per bb-solve (2nd pass only) | l.353 (l.316 on error) | same per-call arena as `_check` |
| dvs_affine.c:382-403 | `_check` | **22 calloc/malloc**, all sized from n1 = AIG nodes + 1: L, R, xa, xb, col, col_node, fidx, seen, stack ((SM_LEAVES+2)·n1·4 = 40 B/node), sm_off, sm_n, sm_c, sm_pool (32 B/node), smark, lin, slot, order, eqs, cl_lits, cl_start, conj, cmark | analysis tables | **PHASE** (aggregate and dump: every one freed together at `done:` l.578-582) | per bb-solve, ×2 when the first pass is undecided (and the second pass rebuilds L/R/xa/xb identically) | l.578-582 | **one arena per `dvs_affine_check`**: sum the sizes and make one allocation (≈150-200 B/AIG node), reset between the two `_check` passes, and **share L/R/xa/xb across passes** |
| dvs_affine.c:479-481 | `_check` | calloc(n_order·W·8), calloc(n_order), malloc(W·8) | XOR forms, consts, temp row | PHASE | per bb-solve | l.580-581 | same arena |
| dvs_affine.c:499-503 | `_check` | 5× malloc/calloc ((n_cols+1)·W·8 etc.) | Gaussian elimination rows/pivots | PHASE | per bb-solve | l.582 | same arena |
| dvs_affine.c:548 | `_check` | calloc(n1) | model node values | OWNED when returned (ownership passes to `S->node_val`, dvs_bbsolver.c:2537, freed with `free`); else SCRATCH | per bb-solve | l.569 / l.572, or caller | leave as malloc (crosses module ownership) |

### dvs_sv.c (SystemVerilog elaboration + mask rewrite; called per compile from dvs_compile.c:3937/3939 and **per incremental add** from dvs_compile.c:4455/4457, and by bbsolver at dvs_bbsolver.c:1705/3102)

Note: SMT2 problems are flagged EXPLICIT (smt2_frontend.c:947), so `dvs_sv_elaborate`
returns at l.907 without allocating on the Verilator/SMT2 path. **`dvs_sv_rewrite_masks`
has no EXPLICIT check and runs on every incremental add**, including the Verilator path.

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_sv.c:126 | `_memo_put` | calloc(nc·16), first nc=1024 (**16 KiB**), then doubling; rehash; free old l.134 | memo hash (ref,W,S,mode) | VECTOR (doubling) inside a PHASE | per elaborate / rewrite_masks call: rewrite_masks reaches it through `_type` (l.354/425), so **≈16 KiB calloc per incremental add** | entry functions l.961, 1028, 1993, 2006 | ctx-retained or caller-retained scratch: clear and reuse instead of calloc, or start small (64) |
| dvs_sv.c:162 | `_alloc` (first COW) | malloc(sizeof(problem) + src capacity + used/2 + 4096) then memcpy of the whole used pool | copy-on-write problem copy | OWNED by caller (returned elaborated problem) | per compile; per add only when a rewrite fires | `dvs_sv_release` l.1036 (caller), l.963/2007 on error | it is the problem buffer, so leave it OWNED. But on the add path the source is the frontend's *full* retained problem (`vproblem`), so one mask rewrite on a new assert copies the **entire problem** per add. Better: build the new nodes in a side arena, or append in place into the source's finalize_reserve slack |
| dvs_sv.c:178 | `_alloc` (grow) | realloc(d, ...), doubling (`cap*2`, min base+bytes+4096) | grow COW copy | VECTOR, doubling | rare | as above | fine |
| dvs_sv.c:793-794 | `_in` | 2× malloc(n·4) | item refs for IN_SET / IN_RANGES | SCRATCH, but **per IN node** | per compile/add, per inside-set node (memoized, so once per node) | l.795 (error), l.826-827 | scratch stack (LIFO; `_in` may recurse via `_val`) or the memo arena |
| dvs_sv.c:928-930 | `dvs_sv_elaborate` | 3× calloc(nv) (u16, u8, u8) | var width/signed/known by id | PHASE | per compile; returns early for EXPLICIT | l.960 | **one allocation** (4 B/var). Shared with rewrite_masks: both build the identical table from vars_head |
| dvs_sv.c:1014-1016 | `dvs_sv_elaborate_more` | 3× calloc(nv) | same | PHASE | per bbsolver incremental assert (dvs_bbsolver.c:3102) | l.1027 | same: one allocation, or keep it in `S` across asserts |
| dvs_sv.c:1614 | `_fd_extract` | realloc(use), doubling from 64 | extract-use list | VECTOR in PHASE | per compile (field split runs only when `fn==NULL`) | l.1859 / 1941 | scratch arena |
| dvs_sv.c:1839 | `_fd_split` | calloc(nv·sizeof(FdWord)) | per-var cut words | PHASE | per compile | l.1859 / 1940 | scratch arena |
| dvs_sv.c:1966-1968 | `dvs_sv_rewrite_masks` | 3× calloc(nv) | var width/signed/known | PHASE | **per compile and per incremental add (per-rand)**; nv = max var id + 1 of the problem passed in, which on the retained path is the whole problem | l.2005 | one allocation; better, a ctx-cached var-type table (the add path already has `_ctx_var_type`) |
| dvs_sv.c:1998 | `dvs_sv_rewrite_masks` | free(memo) before `_fd_split` | — | — | per compile | — | — |

### dvs_compile.c

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_compile.c:2670 (macro `CC_PUSH`, l.2667-2674) | `_compile_constraint` (AND spine) | realloc(stk), doubling from 32 (first alloc 128 B) | explicit stack flattening an AND spine (B10 fix) | SCRATCH + VECTOR | per compile and per add: once per AND-rooted constraint, and again for every nested AND leaf (recursion through `_compile_constraint` l.2687) | l.2671 (OOM), 2688, 2692 | inline `dvs_expr_t buf[32]` on the C stack with heap fallback, or a ctx scratch stack mark/release (LIFO across the recursion) |
| dvs_compile.c:3928, 3932 | `_solver_compile` | free / malloc(sizeof(problem)+capacity) | step-checker problem copy | OWNED global | per compile, **only under `DVS_STEP_CHECK`** | next compile l.3928 (never at exit: a deliberate debug global) | leave |

(Not raw malloc, but reached from the same per-add path through `ctx->alloc`, which is
malloc by default: `_add_names_known` dvs_compile.c:4658 `mine`, `_kw_push` l.4551
(seen hash, 1 KiB start, doubling) and l.4574 (stack, 1 KiB start). All freed at
l.4695-4697. That is three malloc/free pairs per incremental add.)

### dvs_builder.c

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_builder.c:16 / 23 | `_alloc` / `_release` | malloc/free fallback when `b->alloc==NULL` (the SMT2 frontend creates the builder with NULL, smt2_frontend.c:8061) | shim | — | — | — | — |
| dvs_builder.c:36 (via `_new_block`) | `_new_block` | `_alloc(sizeof(BuilderBlock)+4096)` | builder pool block | OWNED until reset or destroy | per 4 KiB of builder data. **`dvs_builder_reset` (l.115-118) frees every block except the first**, and the SMT2 frontend resets after hand-offs (smt2_frontend.c:4670, 4690, 7579), so blocks are re-malloc'd every cycle | `_free_blocks` from reset l.117, destroy l.144 | **CHURN per reset.** Keep the freed blocks on a free list (or take them from a `dvs_block_alloc_t`) |
| dvs_builder.c:60 | `_index_block` | **raw realloc** even when `b->alloc` is set; doubling from 16 | block index | VECTOR | per new block | destroy l.145 (raw `free`) | use `DVS_RESIZE(b->alloc, ...)`: this bypasses the allocator contract in dvs_alloc.h |
| dvs_builder.c:88 | `dvs_builder_create` | malloc(sizeof builder) | builder | OWNED | per frontend | l.146 | leave |
| dvs_builder.c:242 | `dvs_builder_finalize` | `_alloc(total)` + memset of the whole thing + memcpy | finalized problem | OWNED by caller | per finalize; `finalize_since` (l.499) uses it per hand-off on the non-retained path, i.e. per randomize | caller (`free`, or `dvs_builder_free_problem` l.524) | leave OWNED; the memset is redundant except for gaps |
| dvs_builder.c:324 | `dvs_builder_finalize_append` | realloc(prev), growth to used + max(used/2, 4096) | retained problem growth | VECTOR (×1.5) | per hand-off when out of headroom | caller | fine |
| dvs_builder.c:351 | `dvs_builder_finalize_reserve` | `_alloc(total)` | problem with headroom | OWNED | per first finalize | caller | leave |

### dvs_dpi.c (DPI path; compile once, solve per randomize)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_dpi.c:36 | `_b64_decode` | malloc(decoded_len+3) | decoded problem buffer | OWNED (becomes `h->problem_buf`) | per compile | l.47 (bad input), l.111/117/125 on error, l.234 at release | leave |
| dvs_dpi.c:122 | `dvs_dpi_compile_b64` | calloc(1, DpiHandle) | handle | OWNED | per compile | l.235 | leave |

The per-randomize DPI entry points (`dvs_dpi_solve_h`, `pin_var_h`, `checkpoint_h`,
`restore_h`, `get_value_h`, l.148-228) make **no raw allocations**. Per randomize they
reach `dvs_solver_restore` (the `_HoleCarry` vector, dvs_checkpoint.c:148, when holes
were added in the scope), `dvs_solver_checkpoint` (grow-only), and the solve-start lazy
growth listed in section 0b.

### dvs_shape.c (one-shot QF_BV routing, smt2_frontend.c:6007; not Verilator mode)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_shape.c:31 | `_push` | realloc(stack), doubling from 1024 | DFS stack | VECTOR in PHASE | per routing decision | l.195 | scratch arena |
| dvs_shape.c:158 | `_top` | realloc(st), doubling from 64 | AND-spine stack | **SCRATCH per constraint**: malloc and free (l.171) for every constraint root | per routing, × n_constraints (only when a root is an AND) | l.171 | hoist into `Walk` and reuse across constraints |
| dvs_shape.c:182 | `dvs_shape_compute` | calloc(pool_used/8) | visited bitmap | PHASE | per routing | l.196 | scratch arena |

### dvs_smt2_emit.c (dump / oracle)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_smt2_emit.c:39-40 | `_map_put` | 2× calloc(nc) (keys u64, vals u32), doubling from 256 | 4 open-address maps (vtypes, types, uses, memo) | VECTOR in PHASE | per emit | old: l.48; final: `_map_free` l.57 via l.857 | one arena per emit; combine k/v into one allocation |
| (dvs_oracle_core.c:34, out of scope) | `dvs_ob_add` via `DvsOBuf t={0}` at dvs_smt2_emit.c:468 | realloc, first 4096, doubling | temporary text per memo-missed define-fun | CHURN: **a 4 KiB malloc/free per emitted define-fun node** | per node, per emit | `dvs_ob_free(&t)` l.477 | reuse one scratch `DvsOBuf` in `E` (clear instead of free) |

### dvs_diffcycle.c

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_diffcycle.c:270-273 | `diffcycle_build` | malloc(rels: (2·lim+1)·sizeof(DiffRel)), malloc(n_vars·4), calloc(n_vars), malloc(n_vars·4) | relation list, union-find, cycle flags, var relabel | PHASE/SCRATCH (all freed together at `out:` l.380) | per compile (dvs_compile.c:4431) | l.380 | one allocation, or a ctx scratch arena reset per compile. Cannot use `ctx->pool` mark/release, because the propagator it builds (l.348) must persist after the scratch |

### Allocator implementations (in scope)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_block_alloc.c:12/18/24 | `_malloc_*` | malloc/free/realloc | default allocator | — | — | — | — |
| dvs_block_alloc.c:122 (DVS_ALLOC) | `dvs_block_alloc_get` | 16 B `_blk_rec_t` per fresh block | tracking record | OWNED | per fresh block | destroy l.183 | could embed the record in the block's tail, or allocate records in slabs |
| dvs_stack.c:52 / 68 | `dvs_stack_create` / `_destroy` | raw malloc/free | 32 B control struct | OWNED | per ctx | destroy | `DVS_ALLOC(dvs_block_alloc_backing(ba))`: the raw call bypasses the allocator contract |
| dvs_arena.c:14 / 17 | `xalloc` / `xfree` | malloc/free when `alloc==NULL` | arena buffer | — | — | — | grow through `DVS_RESIZE` (see section 0) |

## 2. Pooling proposals, ranked by likely performance impact

1. **NoOverlap2D >64-rect scratch (dvs_placement.c:648/655).** This is the only allocation
   inside the propagation loop: a malloc/free pair on every fire. Fix: size the
   8·nr int32 buffer once at propagator build (from `ctx->pool`, persists with the
   propagator), or use a ctx-owned grow-only `prop_scratch`. Impact: large on
   placement-heavy problems; zero elsewhere.

2. **Per-incremental-add (Verilator per-randomize) scratch in the compile/add path.**
   Per `dvs_solver_add_constraint`, these allocate and free: `dvs_sv_rewrite_masks` (3 n-var
   callocs, dvs_sv.c:1966-1968, plus a 16 KiB memo calloc, dvs_sv.c:126);
   `_add_names_known` (3 ctx-alloc pairs, dvs_compile.c:4551/4574/4658); and
   `_compile_constraint` AND-spine stack (dvs_compile.c:2670). Fix: a **ctx-owned scratch
   arena** (non-moving: a second `dvs_spool_t` or `dvs_stack_t` on ctx), marked at
   entry to `_solver_add_constraint` / `_solver_compile` and released at exit.
   Also cache the var-type table on ctx rather than rebuilding it from vars_head.
   Bigger lever: skip `dvs_sv_rewrite_masks` setup when the new roots contain no
   masks, and avoid the **full problem copy** (dvs_sv.c:162) when one fires on the
   retained `vproblem`. That copy is O(problem) per add. Impact: medium on the
   Verilator loop. It removes 6-8 malloc/free pairs and ≥16 KiB of zeroing per add, but
   the memory note says kissat cold-solve dominates per-randomize cost, so measure first.

3. **Checkpoint-restore hole carry (dvs_checkpoint.c:148-182).** This is a malloc/free per
   restore whenever holes were added inside the scope, and DPI restores on every
   `solve_h`. Fix: a ctx-retained grow-only `carry` buffer. Impact: small to medium per
   randomize, trivial to do.

4. **Builder block recycling (dvs_builder.c:36 via reset l.115-118).** In the SMT2
   frontend's non-retained mode, each hand-off resets the builder, which frees every
   block but the first. The next cycle mallocs them again. Fix: keep the freed blocks on a
   free list (or back the builder with a `dvs_block_alloc_t`). Also route `blocks[]`
   (l.60) through `DVS_RESIZE`. Impact: small to medium per randomize, proportional to
   assertion volume per cycle.

5. **Affine pre-check tables (dvs_affine.c:382-403, 479-481, 499-503, 313-315).** About 30
   allocations, all PHASE, freed at one label. Fix: compute the total and make one
   allocation (or one arena), and share L/R/xa/xb between the two `_check` passes
   (the second pass currently re-derives them). Impact: medium on bitblast
   one-shot solves of large AIGs. That is roughly 30 malloc+memset passes over n-node
   arrays ×2, against the ~0.9 ms startup floor noted in memory.

6. **Diffcycle (dvs_diffcycle.c:270-273), dvs_sv elaborate tables (928-930), field split
   (1614, 1839), shape walk (31, 158, 182).** All per-compile or per-routing PHASE scratch. They fold
   into the same ctx scratch arena as item 2, or one-shot allocations. Impact: small. The
   `_top` per-constraint churn (dvs_shape.c:158) is the only per-item one.

7. **SMT2 emitter (dvs_smt2_emit.c:39-40, `DvsOBuf t` at 468).** A 4 KiB malloc/free per
   define-fun node; reuse one scratch OBuf. Impact: only when the oracle or SMT2 dump is
   on.

8. **Contradiction analysis (dvs_contradiction.c:370, 748, 859, 1098).** Each loop iteration
   mallocs the same-size problem buffer and memcpy's the whole pool. Hoist the buffer
   and copy the base pool once, then mark/release per iteration (SCOPE). Steal rather
   than copy sub-result arrays (805/818/826). Impact: negligible next to the
   solver-context creation per iteration (`dvs_solver_new` + compile each time). The
   real win there would be reusing one context with assumptions. Diagnostic only.

Housekeeping:
- `dvs_arena_t` is dead in production. Either adopt it (item 2/5/6 scratch) after switching
  `grow_to` to `DVS_RESIZE`, or delete it.
- Allocator-contract violations ("a custom allocator sees every byte", dvs_alloc.h):
  dvs_stack.c:52/68, dvs_builder.c:60/145, dvs_arena.c fallbacks.

## 3. Bugs and hazards noticed (not fixed)

- **B-1 heap buffer overflow in `_format_text` / `_format_json`** (dvs_contradiction.c:569-572,
  643-646). `APPEND` does `pos += snprintf(buf+pos, cap-pos, ...)`. On truncation
  snprintf returns the would-be length, so `pos` passes `cap`, `cap - pos` wraps to a
  huge `size_t`, and the next APPEND writes past the buffer. `cap` budgets 256 B per
  MUS entry, but `info[j].name` / `source_file` are caller strings of any length.
  A long constraint name or path overflows the heap.
- **B-2 NULL deref on OOM** (dvs_contradiction.c:429-436 / 469-475 / 495-503 → 532-538).
  `result->mus_size` is set before the `mus_constraint_ids` malloc. If that malloc
  fails, `_format_text`/`_format_json` still run because they test only `mus_size > 0`,
  and they dereference `result->mus_constraint_ids[i]` (l.577, 652).
- **B-3 `malloc(0)` treated as failure** (dvs_contradiction.c:240-241). With
  `n_bg + n_mus2 == 0`, an implementation that returns NULL for malloc(0) makes QuickXplain
  skip the c1 half silently. That gives a wrong (non-minimal or incomplete) MUS. Not
  triggered on glibc.
- **B-4 `CC_PUSH` OOM reported as "dropped"** (dvs_compile.c:2671). On realloc failure,
  `_compile_constraint` returns 0 ("constraint dropped / uncompiled") rather than an
  error. The ctx pool overflow flag is not set, so `_nomem_check` does not convert it
  to NOMEM. The constraint then relies on the uncompiled fallback (validation/escalation)
  instead of failing loudly. Soundness depends on every caller handling rc>0.
- **B-5 `_map_put` failures ignored** (dvs_smt2_emit.c:217, 478, 708, 810). The return
  value is never checked. A failed `vtypes` insert drops a declared variable's type,
  and a failed `uses` insert drops a use-count. Either can change the emitted SMT2
  silently, not only its speed.
- **B-6 DPI decoded buffer is not validated** (dvs_dpi.c:107-114). The decoded bytes are cast
  to `dvs_problem_t*` and compiled without checking `buf_len >= sizeof(dvs_problem_t)`
  or `pool.capacity/used <= buf_len - header`. `problem_size` is stored but never used.
  Malformed base64 input causes out-of-bounds reads.
- **B-7 `dvs_builder_finalize_append` API hazard** (dvs_builder.c:300-311). When `prev` is
  non-NULL but `b->alloc` is set, `old > used` (after a reset), or no blocks remain, it returns a
  **new** buffer and neither frees nor mentions `prev`. A caller that writes
  `p = finalize_append(b, p, ..)` leaks the old one. The current frontend frees or
  retires `vproblem` before every reset (smt2_frontend.c:4652-4664), so it is latent.
  Also, the header (dvs_builder.h:86) says "the caller frees it with free()", which is
  wrong when the builder has a custom allocator.
- **B-8 learnt-clause memory is never reclaimed within a solve** (dvs_lcg.c:185-196). `clause_db_gc` only
  NULLs `clauses[i]`, leaving their bytes and watch entries in the spool until
  `lcg_reset`. Memory grows with conflicts, and watch lists still point at dead
  clauses (each traversal must skip them).
- **B-9 `dvs_stack_alloc` hard-fails requests above one block** (dvs_stack.c:94-96;
  64 KiB blocks from dvs_ctx.c:30/195). The failure returns NULL. Fine for trail entries,
  but it rules `dvs_stack` out as a general scratch stack for n-var-sized arrays.
- Minor: dvs_sv.c:162-170 copies `sizeof(problem)+used` but places new nodes after
  `capacity`. The gap `[used, capacity)` in the copy is uninitialized, which is harmless
  unless something scans it. dvs_affine.c `_check` allocates
  `(SM_LEAVES+2)·n1·4 = 40 B/node` for `stack` even on the XOR-only first pass.
