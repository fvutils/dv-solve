> Appendix to [malloc_lifecycle_analysis.md](../malloc_lifecycle_analysis.md). Surveyed at 65acd8d; line numbers refer to that commit. Bugs listed here as "not fixed" may since be fixed: the analysis doc's bug table has their status.

# Raw heap allocation survey: SMT2 front end

Scope: `src/c/smt2/smt2_frontend.c`, `smt2_parser.c`, `smt2_eval.c`, `smt2_oracle.c`, `smt2_main.c`.
Branch `feat/dynamic-pool` @ 65acd8d. Read-only survey; no source edited.

Struct sizes, measured with a sizeof probe against `smt2_frontend.h`:
`Smt2Frontend` = 180,416 B, `Smt2Var` = 136 B (inline `name[128]`), `Smt2FunDef` = 1,200 B,
`Smt2ArrayValue` = 136 B, `Smt2Subst` = 56 B, `Smt2ArrayRead` = 24 B, `Smt2PushFrame` = 92 B.
`Smt2InSetAcc` holds `elems[4096]`, so it is about 16 KB.

Abstractions available (headers read):
- `dvs_alloc_t`: alloc/release/resize vtable. Every raw site here could route through it.
- `dvs_spool_t`: segmented bump pool. Growth never moves a backing, so addresses are stable. Has `mark`/`release`.
- `dvs_arena_t`: one doubling buffer with offset refs. Pointers are unstable across growth. Has `mark`/`release`/`reset`.
- `dvs_stack_t`: block-chained bump allocator with stable pointers and `push`/`pop` marks.
- The front end also has its own `SexprArena` (smt2_parser.c), a block chain with `reset` and no mark. **It has a leak bug; see Bug 1.**

Pattern key: SCRATCH, PHASE, SCOPE, VECTOR, OWNED, CHURN, OTHER (as defined in the task).
Every VECTOR site found grows by **doubling**. There is no +1 or small-increment growth anywhere in scope.

---

## 1. Site table

### smt2_parser.c

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| parser.c:21 | `_arena_new_block` | malloc(hdr+cap) | SexprArena block. Fixed `block_size`: 8 KB main loop, 16 KB persistent, 64 KB eval, 4 KB get_value_text | PHASE (per command / per eval / per reset) | each time a block fills. **After a reset, every block past the head is re-malloc'd (Bug 1)** | parser.c:66 `sexpr_arena_destroy` only | fix reset to walk the existing chain, or replace with `dvs_stack_t` (stable ptrs + mark) |
| parser.c:107 | `_list_push` | malloc(newcap*8) | child-list spill once a list has more than 16 children (inline buffer is 16) | SCRATCH (LIFO within `_parse_tok`) | per parsed list with >16 children (wide `and`/`concat`/`let`) | parser.c:121 `_list_free` at list close or on fail | per-parser reusable `dvs_stack_t` scratch (frames nest LIFO) |
| parser.c:110 | `_list_push` | realloc doubling | further spill growth | VECTOR / SCRATCH | log2(n/16) per wide list | parser.c:121 | same |
| parser.c:274 | `_parse_tok` | realloc doubling (first 16 frames = about 2.3 KB) | open-list frame stack | SCRATCH | **once per top-level list parse = per command** (also per rewrite-body parse and per get_value_text) | parser.c:293 / 308 | keep the frame stack in the lexer or arena across parses (OWNED, reused), or start from an inline frame array |

### smt2_eval.c (one evaluator per query: per get-value with a non-trivial term, and per model check = per check-sat)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| eval.c:1315 | `smt2_eval_new` | calloc(1,sizeof Smt2Eval) | evaluator object | PHASE (per query) | per check-sat with the model check on (default outside Verilator mode); per non-simple get-value | eval.c:1329 | keep one evaluator in `fe` and add `smt2_eval_reset` |
| eval.c:1319 (+ :124 `_alloc`) | `smt2_eval_new` / `_alloc` | SexprArena with 64 KB blocks | all values, sorts, memo arg copies | PHASE | first block on first `_alloc` of each evaluator (64 KB per query) | eval.c:1326 destroy (never reset) | arena reset in `smt2_eval_reset` (after Bug 1 is fixed) |
| eval.c:610 | `_memo_put` | calloc(nc) doubling from 256 (about 10 KB) | define-fun memo hash (rehash on grow) | VECTOR (hash) / PHASE | first memo insert per evaluator, then on each doubling | eval.c:619 (old table), :1327 | keep in the persistent evaluator; clear by generation counter |
| eval.c:713 | `_sym` | calloc(nc) doubling from 256 | global-name resolution cache | VECTOR (hash) / PHASE | first symbol per evaluator | eval.c:721, :1328 | same |

### smt2_frontend.c: per-command and translation sites

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| fe.c:166 | `_cmd_alloc` | malloc(sz) | hand-rolled per-command pool (pointer list, cap `SMT2_CMD_ALLOC_MAX`=512) | PHASE (per command) | per call below | fe.c:177 `_cmd_alloc_reset`, called at `_dispatch` start (8449), destroy (8133), soft reset (8208) | **per-command `dvs_arena_t`/`dvs_stack_t`, reset at dispatch.** Removes the 512 cap and Bug 4 |
| fe.c:703, 707 | `_make_array_value` | `_cmd_alloc` x2 | transient dense array value (136 B) + `elems[2^aw]` (up to 1024 x 4 B = 4 KB) | PHASE | per store / ite / as-const term on a dense array | via `_cmd_alloc_reset` | per-command arena |
| fe.c:2425 | `_tr_select` | `_cmd_alloc` | nested-array slice view | PHASE | per select on a nested array | via `_cmd_alloc_reset` | per-command arena |
| fe.c:2328, 2329 | `_let_bind` | malloc x2 (n x 24 B, n x 8 B) when a let has >8 bindings | parallel-let value and name buffers | SCRATCH | per `let` term with >8 bindings (z3/yosys dumps) | 2330 / 2343 / 2359 / 2362 | one combined alloc from a translation scratch stack (mark/pop), or reserve in `subst_stack` |
| fe.c:2830 | `_tr_distinct` | malloc(n x TaggedExpr) | operand buffer | SCRATCH | per `distinct` term | 2834 / 2837 / 2850 | translation scratch stack |
| fe.c:3291, 3302 | `_sexpr_deep_copy` | malloc(64 x 16 B = 1 KB), realloc doubling | DFS worklist | SCRATCH | **per deep copy:** every level-0 declare/define (`_decl_record`, always on, including Verilator mode), every assert when the model check or produce-assertions is on (`_record_checked`), every define-fun body (3891), define-sort (3608), core record (8274) | 3297 / 3303 / 3310 | fe-resident reusable worklist (OWNED VECTOR) or scratch stack |
| fe.c:3241-3276 | `_sexpr_copy_node` | `sexpr_arena_alloc` into `persistent_arena` | copied assertion/declaration terms | SCOPE in practice (counts are truncated on pop) but never reclaimed until reset | per deep copy | arena reset in soft reset; destroy | `persistent_arena` with **mark/release per push frame** (`dvs_stack_t` or `dvs_spool_t`) |
| fe.c:2078 | `_rewrite_body` | `sexpr_arena_alloc` into `persistent_arena` (+ parse into it) | operator-rewrite text and body cache (`SMT2_MAX_REWRITES`=96) | OWNED, but see Bugs 2 and 3 | per new (op,width,k). **Once the cache is full, per application** | soft-reset arena reset (dangling); destroy | session arena (not reset), or separate small arena that survives reset |
| fe.c:4134 | `_try_translate_in_set` | **malloc(~16 KB `Smt2InSetAcc`)** | IN_SET element accumulator | **CHURN: per assert** whose top is `(= #b1 X)` / `(= X #b1)`, the common Verilator/yosys shape | per assert | 4140 | fe-resident buffer (OWNED, allocated once), or scratch-stack mark/pop |
| fe.c:4288 | `_record_named` | malloc(len+1) | `:named` label string | SCOPE (LIFO via `_truncate_named` on pop) | per named assert | 4298 (pop / reset / destroy) | persistent arena with push-frame marks (same as the deep copies) |
| fe.c:4283 | `_record_named` | realloc doubling from 16 | `named[]` pointer table | VECTOR / OWNED | log2(n) | 8091 / 8211 | keep capacity across soft reset |

### smt2_frontend.c: session tables (VECTOR / OWNED, all doubling)

All of these are freed in destroy (8089-8131) **and in soft reset (8180-8214)**. Soft reset runs per Verilator `randomize()`, so in Verilator mode each table is freed and regrown on every randomize.

| file:line | function | call | object | growth | frequency | freed where | suggested |
|---|---|---|---|---|---|---|---|
| fe.c:195 | `_bump_n_vars` | realloc | `vars[]` (136 B each) | doubling from 16 | log2(N/16) per session or per reset | 8101 / 8184 | retain across soft reset (truncate `n_vars`) |
| fe.c:213 | `_add_var` | realloc | `vars[]` | doubling from 16 | same | same | same |
| fe.c:249 | `_var_index_put` | calloc rehash | name hash (`var_idx`), 4 KB start | doubling from 1024 | first lookup, then on each doubling | 259 (old), 8102 / 8185 | retain; memset on reset |
| fe.c:336 | `_fun_index_put` | calloc rehash | `fun_idx` | doubling from 256 | per doubling | 346, 8103 / 8186 | retain |
| fe.c:403 | `_subst_reserve` | realloc | let/define-fun subst stack (LIFO by depth) | doubling from 64 | rare | 8105 / 8188 | already a stack; retain across reset |
| fe.c:1554 | `_fun_memo_put` | calloc rehash | define-fun memo (1024 x ~56 B, about 57 KB zeroed) | doubling from 1024; cleared per command by generation (1498) | first memoized apply per session or reset | 1567 (old), 8106 / 8189 | retain across reset (already generation-cleared) |
| fe.c:3603 | `_cmd_define_sort` | realloc | `sort_aliases[]` | doubling from 8 | per define-sort | 8107 / 8190 | retain |
| fe.c:3827 | `_cmd_define_fun` | realloc | `funs[]` (1,200 B each) | doubling from 16, cap 8192 | per define-fun | 8104 / 8187 | retain |
| fe.c:4481 | `_record_checked` | realloc | `checked[]` | doubling from 64 | per assert (model check on) | 8108 / 8213 | retain |
| fe.c:7403 | `_push_reserve` | realloc | push frames | doubling from 16 | per push depth growth | 8129 / 8205 | retain |
| fe.c:8269 | `_core_record` | realloc | `core_hist[]` | doubling from 64 | per assert-shaping command (unsat cores on) | 8092 / 8212 | retain |
| fe.c:8298 | `_decl_record` | realloc | `decl_hist[]` | doubling from 64 | **per level-0 declare/define, always on** | 8109 / 8214 | retain |
| fe.c:4739 | `_flush_aux` | realloc | `aux_problems[]` | doubling from 8 | per aux hand-off | 8098 / 8182 | retain |
| fe.c:6055 | `_shape_slot` | calloc(256 x 32 B) | engine-routing cost table | one-shot | once per process (carried across reset) | 8089 | fine as is (OWNED) |

### smt2_frontend.c: arrays

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| fe.c:772 | `_declare_array_const` | calloc(136 B) | sparse array value | SCOPE/OWNED (freed on pop of its scope) | per array declare | pop 7631, destroy 8120, reset 8197 | spool with push-frame mark |
| fe.c:789, 798 | `_declare_array_const` | calloc + malloc(2^aw x 4 B, <= 4 KB) | dense array value + `elems[]` | SCOPE/OWNED | per dense array declare | 800 (OOM), pop 7627/7631, destroy, reset | spool with mark (two objects become one alloc) |
| fe.c:852, 855, 858 | `_sparse_elem` | realloc x3 parallel | `sparse_idx` / `sparse_idx_hi` / `sparse_varid` | VECTOR doubling from 8 (cap 4096) | per new constant index | promote 1271-1273, pop 7628-7630, destroy, reset | one struct-of-3 block (one realloc instead of three) |
| fe.c:909, 1239 | `_anode_new`, `_promote_sparse` | realloc | `anodes[]` pointer table | VECTOR doubling from 16 | log2 | 8125 / 8201 | retain |
| fe.c:915 | `_anode_new` | calloc(136 B) **each node** | abstract-array DAG node (BASE/STORE/CONST/ITE) | **SCOPE:** freed in LIFO order past the push mark (7459), destroy 8124, reset 8200 | per abstract store/ite/const term (hash-consed) | 7459 / 8124 / 8200 | **`dvs_spool_t` with a mark in each push frame** (stable addresses fit the pointers held by TaggedExpr, areads and aeqs). Pop becomes one release instead of N frees |
| fe.c:1038 | `_aread_hash_reserve` | calloc rehash | read-dedup hash | doubling from 64 at 0.75 load | per doubling | 1040, 1081 (OOM drop), 8127 / 8203 | retain |
| fe.c:1117, 1249, 5005 | `_abs_find_or_create_read`, `_promote_sparse`, `_abs_read_inplace` | realloc | `areads[]` | doubling from 32 | log2 | 8126 / 8202 | retain |
| fe.c:1306 | `_abs_array_eq` | realloc | `aeqs[]` | doubling from 16 | log2 | 8128 / 8204 | retain |

### smt2_frontend.c: per check-sat / solve

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| fe.c:87 / 106 | `_memout_open` / `_memout_finish` | `open_memstream` (libc malloc/realloc) + free | buffered response | PHASE | **per check-sat** when the model check is on (8425); per non-simple get-value (7104, 7113); per reset-assertions (8326) | 106 | a reusable fe-owned `DvsOBuf`-style buffer with `fmemopen`, or keep the memstream across check-sats |
| fe.c:8372 | `_model_check` | `smt2_eval_new` (calloc + 64 KB block + ~10 KB memo + ~10 KB syms) | model checker evaluator | PHASE (per check-sat) | per sat check-sat with the model check on | 8404 | **one persistent evaluator in `fe` + reset** |
| fe.c:7208 / 7252 | `_get_value_body` | `smt2_eval_new` lazily | expression get-value | PHASE (per get-value) | per get-value containing a non-symbol or define-fun term (yosys-smtbmc cover loop) | 7267 / 7272 | same persistent evaluator |
| fe.c:5188 (`_LEM_PUSH`), 5280 | `_array_refine` | realloc doubling from 64 | lemma list | SCRATCH | **per refinement round** of the array CEGAR loop (many per check-sat) | 5185-5329 (all exits) | check-sat scratch stack, or fe-retained vector cleared per round |
| fe.c:5250 | `_array_refine` | malloc(n_areads x 32 B) | congruence sort buffer | SCRATCH (CHURN in the CEGAR loop) | per refinement round | 5251 / 5277 / 5288 | same (size is known up front; retained buffer) |
| fe.c:5358 | `_array_diversify` | malloc(4096 x 4 B = 16 KB) | split-literal pool | SCRATCH | per seeded array check-sat | 5377 / 5384 / 5391 / 5401 | check-sat scratch |
| fe.c:5633 | `_check_sat_enum` | malloc(k x 4) | new constraint roots | SCRATCH | per incremental check-sat on the Verilator enum path | 5654 | check-sat scratch |
| fe.c:5758 | `_session_sync` | malloc(k x 4) | new constraint roots | SCRATCH | per check-sat in the bitblast session (BMC) | 5787 | check-sat scratch |
| fe.c:7803 | `_check_sat_assuming_bb` | malloc((n+1) x 4) | assumption roots | SCRATCH | per check-sat-assuming on bitblast | 7814 / 7818 / 7831 | check-sat scratch |
| fe.c:4573, 4593, 5414 | `_ensure_problem`, `_ensure_compiled`, `_check_sat_array` | free(problem) (allocated in the builder by `dvs_builder_finalize`) | whole-problem pool copy | PHASE (per check-sat when dirty; **every check-sat on the array path**) | per check-sat | here; 8094 / 8180 | out of scope (builder), but it is the largest per-check-sat alloc: see `finalize_append` |
| fe.c:5481, 5503, 5643, 5667 | `_free_bb`, `_check_sat_bitblast_on`, `_check_sat_enum` | free of finalize results | bb problem copies. `_check_sat_enum` (5620) **finalizes the full problem on every call**, even on the incremental path | PHASE | per check-sat (Verilator enum) | here | use `finalize_append` (as `_session_sync` does) |
| fe.c:4657, 4663 | `_retention_end`, `_start_cdcl_retention` | free vbase / vproblem | retained validation copies | OWNED | per retention end / compile | 8130 / 8206 | fine |
| fe.c:4745 | `_flush_aux` | free(aux) on OOM | aux problem | OTHER | OOM only | — | — |

### smt2_frontend.c: get-value / unsat-core / reset-assertions / oracle hook

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| fe.c:6649, 6650 | `_emit_abs_array` | malloc x2 ((n_areads+1) x 8) | idx/val dedup buffers | SCRATCH | per get-value of an abstract array | 6674 / 6675 | query scratch (one alloc) |
| fe.c:6717, 6718 | `_emit_sparse_array` | calloc x2 | outer-key buffers | SCRATCH | per get-value of a nested sparse array | 6766 / 6767 | query scratch |
| fe.c:7048 | `_fprint_any` | malloc doubling past 32 inline frames | print stack | SCRATCH | only for terms nested >32 deep | 7051 / 7067 | fine (rare) |
| fe.c:7710 | `_core_replay_unsat` | malloc(180 KB `Smt2Frontend`) + full init/destroy | scratch frontend | SCRATCH | per replay; minimisation does one replay per named assertion | 7723 | allocate once per `_core_minimise` and soft-reset between replays (solve cost dominates) |
| fe.c:7740 | `_core_minimise` | calloc(n_named) | recorded flags | SCRATCH | per get-unsat-core | 7764 | fine |
| fe.c:7782 | `_cmd_get_unsat_core` | calloc(n_named) | drop flags | SCRATCH | per get-unsat-core | 7793 | fine |
| fe.c:8316, 8317 | `_cmd_reset_assertions` | SexprArena (16 KB block) + malloc(n x 8) | replay copies | SCRATCH | per reset-assertions | 8333 / 8334 | fine (rare) |
| fe.c:8558, 8567 (8576 Win) | `smt2_frontend_get_value_text` | SexprArena (4 KB) + `open_memstream` | oracle's get-value text | SCRATCH; the result buffer is OWNED by the caller | per oracle sat check | arena at 8586; buf freed by oracle.c:329 | debug path, low priority |
| fe.c:8062 | `smt2_frontend_init` | `sexpr_arena_init(persistent_arena, 16 KB)` | persistent term copies | OWNED / SCOPE | once | 8110 destroy; 8218 reset | see Bug 1 and proposal P5 |

### smt2_oracle.c (DV_ORACLE debug harness only)

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested |
|---|---|---|---|---|---|---|---|
| oracle.c:138 | `smt2_oracle_from_env` | calloc | oracle object | OWNED | once | 141 (fail), 508 | fine |
| oracle.c:165 | `_add_decl` | realloc doubling from 64 | `decls[]` | VECTOR | log2 | 506 | fine |
| oracle.c:177 | `_add_decl` | malloc(len+3), **not NULL-checked** | printable decl name | SCOPE (truncated on pop / reset) | per declare | 191 `_truncate_decls` | small string arena with marks |
| oracle.c:272 | `_build_pins` | malloc((n_decls+1) x 4) | pinned-decl index | SCRATCH | per sat check | 289 / 294 / 330 | fine |
| oracle.c:271, 345 + dvs_ob_* (dvs_oracle_core.c:29, doubling from 4096) | `_build_pins`, `_check` | `DvsOBuf` req/query/pins/partial | text buffers | SCRATCH (4 bufs per check) | per check | 293 / 379 | reuse fe-owned `DvsOBuf`s, clear per check |
| oracle.c:292 | `_build_pins` | `smt2_frontend_get_value_text` result | get-value reply | SCRATCH | per sat check | 329 | — |
| oracle.c:455, 456 | `smt2_oracle_after` (push) | realloc x2 doubling from 16 | scope marks | VECTOR | per push growth | 506 | fine (see Bug 8) |
| oracle.c session `DvsOBuf` | `_forward` | `dvs_ob_add` doubling | flattened assertion script | OWNED | per forwarded command | 507 | fine |

### smt2_main.c

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested |
|---|---|---|---|---|---|---|---|
| main.c:84, 93 | `_read_all` | malloc(4 KB) + realloc doubling | whole batch input | OWNED / VECTOR | once per batch run | 94 (fail), 344 | fine (mmap would avoid the copy) |
| main.c:115 | `_grow_push` | realloc doubling from 256 | interactive command buffer, reused (len reset per command) | OWNED VECTOR | log2 of the largest command | 413 | fine, already reused |
| main.c:218 | `_dispatch_checked` | `open_memstream` | captured check-sat answer (oracle on) | SCRATCH | per check-sat with the oracle on | 230 | debug path |
| main.c:295 / 351 | `_run_batch` / `_run_interactive` | `sexpr_arena_init(8 KB)` + **`sexpr_arena_reset` per command** (314 / 382) | parse arena | PHASE (per command) | per command | 342 / 411 | correct use of a PHASE arena, **but defeated by Bug 1** |

---

## 2. Pooling proposals, ranked by likely performance impact

**P1. Fix `SexprArena` reset so it reuses its chain (bug fix and the biggest win). Sites: parser.c:21/47/73. Four arenas are affected: main parse, persistent, eval, get_value_text.**
`sexpr_arena_reset` rewinds `current` to `head` but `sexpr_arena_alloc` never moves on to `current->next`. When the head fills, it mallocs a new block and overwrites `head->next`, which orphans the old chain. Effects:
- (a) every block past the first is re-malloc'd on every command or reset;
- (b) the old ones leak (Bug 1).

Fix options: advance through `next` blocks before allocating, or replace `SexprArena` with `dvs_stack_t` (stable pointers, push/pop marks, block reuse).

Calls saved: in a BMC stream, every command whose s-expression exceeds 8 KB (more than about 250 nodes) saves (blocks - 1) mallocs. In Verilator mode about 5 mallocs plus about 80 KB per `(reset)` (measured, see Bug 1). The eval arena is never reset, so it gains only once P3 makes the evaluator persistent.

**P2. Soft reset: keep capacities instead of freeing (Verilator hot loop). About 17 tables, fe.c:8180-8214.**
Each `(reset)`, which Verilator sends once per `randomize()`, frees:
- `vars`, `var_idx`, `fun_idx`, `funs`, `subst_stack`, `fun_memo`;
- `sort_aliases`, `anodes`, `areads`, `aread_hash`, `aeqs`, `push`;
- `named`, `core_hist`, `checked`, `decl_hist`, `aux_problems`.

Then `memset`s the 180 KB struct and regrows everything:
- `vars` takes log2(N/16) reallocs of 136 B x N;
- `var_idx` is a 4 KB calloc;
- `fun_memo` is a 57 KB zeroed calloc on the first memoized define-fun;
- `decl_hist` is regrown from 64, and so on.

Proposal: carry `{ptr, cap}` across the reset the way `builder` and `persistent_arena` already are, set counts to 0, and memset only the hash tables. `fun_memo` is already generation-cleared.

Estimate: 15-30 malloc/free pairs and 60-250 KB of zero-fill saved per randomize, plus the 180 KB struct memset (which could shrink by moving the cold inline arrays out).

**P3. One persistent `Smt2Eval` per frontend, with `smt2_eval_reset`. Sites: eval.c:1315/1319/610/713; fe.c:8372, 7208, 7252.**
Every sat check-sat with the model check on (the default outside Verilator mode) and every non-trivial get-value currently does: calloc, a 64 KB arena block, a 10 KB memo calloc, a 10 KB syms calloc, then frees them all.

The reset would: reset the arena (needs P1), and clear `memo` and `syms` by generation. The host callbacks are the same per fe, but the `syms` cache resolves values from the current model, so it must be cleared per query.

Saves 4+ mallocs and about 85 KB of zero-fill per check-sat. Add the reusable memstream (fe.c:87, also per check-sat) for one more malloc/realloc chain.

**P4. Per-command PHASE arena replacing `_cmd_alloc`, plus a translation scratch stack. Sites: fe.c:166/703/707/2425 (PHASE) and 2328/2329/2830/3291/4134 (SCRATCH).**
Replace `_cmd_alloc`'s malloc-per-object with a `cmd_arena` (dvs_arena or dvs_stack) reset at `_dispatch`. That also removes the 512-object cap and its leak (Bug 4).

Add `fe->scratch`, a `dvs_stack_t` with push/pop marks, for the LIFO temporaries: let buffers, distinct args, deep-copy worklist, and the 16 KB `Smt2InSetAcc`.

The biggest single item is **fe.c:4134: a 16 KB malloc/free on every `(assert (= #b1 ...))`**. Next is the 1 KB deep-copy worklist (fe.c:3291) on every level-0 declaration (`_decl_record`, always on, including Verilator mode) and every assert when the model check is on.

Estimate: 2-3 malloc/free pairs per assert and 1 per declaration.

**P5. Scope-marked pools for push/pop-scoped objects. Sites: fe.c:915 (anodes), 772/789/798 (array values), 4288 (named), 3241-3276 (persistent copies); oracle.c:177.**
Abstract-array nodes are calloc'd one at a time and freed one at a time in LIFO order on pop (7459). A `dvs_spool_t` gives stable addresses, which the TaggedExpr/areads/aeqs pointers need, and lets pop do one `dvs_spool_release(push[d].mark)`.

The same push-frame mark should cover:
- the `persistent_arena` deep copies (`checked`, `core_hist`, `named` strings). Today these are truncated on pop but never reclaimed, so push/pop-heavy sessions grow without bound (Bug 6);
- the declared array values.

Impact is moderate: it is proportional to abstract-array terms and pops. Much of the value is bounded memory rather than speed.

**P6. Per-check-sat scratch for solve-path temporaries. Sites: fe.c:5188/5280/5250 (refine), 5358, 5633, 5758, 7803, 6649/6650, 6717/6718.**
`_array_refine` runs once per CEGAR round, many times per check-sat. Each run reallocs the lemma vector from 64 and mallocs an n_areads x 32 B sort buffer. The `roots` buffers are one per check-sat.

A retained fe-owned buffer, or a check-sat `dvs_stack_t` mark/pop, removes about 2-4 malloc/free pairs per refinement round. This is the hottest raw-malloc loop in scope, but only on the array engine.

Related, out of scope (builder): `_check_sat_enum` (fe.c:5620) finalizes the full problem on every check-sat even when it then only rebases incrementally. Switching to `dvs_builder_finalize_append` as `_session_sync` does would remove a whole-pool copy per Verilator-enum check-sat.

**P7. Parser frame stack and list spill. Sites: parser.c:274, 107, 110.**
One 2.3 KB malloc/free per command for the frame stack, plus a spill per list with >16 children. Keep the frame stack in the parser or arena across calls, sized to the deepest nesting seen. Small win per command, but it is in every command.

**P8. Minor.**
- Three parallel sparse-index reallocs (fe.c:852-858) could be one block.
- `_core_replay_unsat` (fe.c:7710) could reuse one sub-frontend across replays.
- Oracle `DvsOBuf`s (debug only) could be reused across checks.

---

## 3. Bugs noticed (reported, not fixed)

1. **`SexprArena` reset leaks every block past the head, and re-mallocs them (parser.c:44-54 vs 73-80). Confirmed.**
   After `sexpr_arena_reset`, `current == head`, and the allocator only tries `current`. When the head is full it mallocs a block and does `arena->current->next = nb`, dropping the old `head->next` chain.
   - A standalone ASan harness (`sexpr_arena_alloc` x10 per iteration with a reset between) leaked 18 blocks over 3 iterations.
   - End to end with `build-asan/dv-solve-smt2 --mode=verilator`: 300 `declare-const` + check-sat + `(reset)` per round.

   | rounds | leaked (ASan) | allocations | RSS, release build `build/dv-solve-smt2` |
   |---|---|---|---|
   | 50 | 4.0 MB | 245 | 9 MB |
   | 200 | 16.3 MB | 995 | 23 MB |
   | 800 | 65.3 MB | 3980 | 78 MB |

   That is about 80 KB and 5 blocks leaked per `(reset)`. The leak stack is `_sexpr_copy_node` ← `_sexpr_deep_copy` ← `_decl_record` into `persistent_arena`. Per-randomize memory growth in Verilator mode.
   The same leak hits the main-loop parse arena (main.c:314/382) on every command larger than 8 KB of nodes.

2. **Operator-rewrite cache dangles into a reset arena (fe.c:8171-8174 + 8218 + 8243-8244).**
   Soft reset carries `rewrites[]` (text + body pointers) across `(reset)`. The comment says they are "parsed into persistent_arena, which is kept", but the same function calls `sexpr_arena_reset(&parena)`, so later allocations overwrite them.
   The `_rewrite_body` lookup then runs `strcmp` on stale, overwritten memory (UB; may read past a NUL-less region). It re-parses on a mismatch and appends another entry, so stale entries fill the 96-slot cache within a few dozen randomizes.
   A stale body that is still read as valid needs its text to match by accident; that is unlikely but not impossible. Found by reading only (no reproducer).

3. **Rewrite cache full → re-parse into `persistent_arena` on every application (fe.c:2077-2092).**
   Once `n_rewrites == SMT2_MAX_REWRITES`, every use of bvcomp/bvnand/bvredor etc. allocates and parses its text again with no bound (until reset), after a 96-entry linear `strcmp` scan. Bug 2 makes it easy to hit.

4. **`_cmd_alloc` past 512 allocations leaks permanently (fe.c:165-173).**
   The comment says it "leaks until smt2_frontend_destroy()", but the pointer is not recorded anywhere, so destroy cannot free it. Reachable with a command holding more than 256 dense-array store/ite terms (2 allocs each).

5. **`smt2_oracle.c:177`: `malloc` result not checked.** The `sprintf` at 178/179 writes through NULL on OOM.

6. **Model printing on OOM gives a silently wrong value.**
   - `_emit_abs_array` (fe.c:6649-6661): if either malloc fails, `n` stays 0 and the array prints as all-default with no stores, a wrong model presented as valid.
   - `_emit_sparse_array` (fe.c:6717-6727) does the same for nested arrays: calloc failure means `n_outer = 0` and all stores are dropped.

   Both should report an error, like the term-eval path does.
7. **`persistent_arena` is never reclaimed on pop (fe.c:3241ff, 4486, 8274).** `n_checked` and `n_core_hist` are truncated on pop, but their deep copies stay. Push/pop-heavy sessions grow memory without bound until reset. This is unbounded growth, not a leak; it is the motivation for P5.
8. **Oracle push partial-realloc failure (oracle.c:455-459).** If one of the two reallocs fails, the loop breaks with `n_scope` short of the requested pushes, so a later `pop` truncates the wrong decls in the oracle's replay. Debug tool only.
9. **Perf.** In Verilator mode, `_decl_record` (fe.c:8295) deep-copies every level-0 declaration (1 KB worklist malloc + arena copy) on every randomize. Only `reset-assertions` needs these copies, and Verilator never sends it. This is the source of the Bug 1 leak volume. It could be recorded lazily or as pointers into a retained arena.

Scratch artifacts used for verification (not deliverables):
- `scratchpad/sz.c`: sizeof probe;
- `scratchpad/arena_leak.c`: arena reset leak harness;
- `scratchpad/vlt_reset.smt2`: generator for the reset-loop session.
