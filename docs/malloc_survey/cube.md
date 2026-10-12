> Appendix to [malloc_lifecycle_analysis.md](../malloc_lifecycle_analysis.md). Surveyed at 65acd8d; line numbers refer to that commit. Bugs listed here as "not fixed" may since be fixed: the analysis doc's bug table has their status.

# Raw heap allocation survey: cube / costguided / oracle / SAT seam

Scope: `src/c/dvs_cube.c`, `dvs_costguided.c`, `dvs_oracle.c`, `dvs_oracle_core.c`,
`dvs_sat.c`, `dvs_sat_kissat.c`, `dvs_sat_cadical.c` (+ headers; the headers
contain no allocation sites). Bundled solvers under `src/c/sat/` excluded.
Read-only survey, branch `feat/dynamic-pool` @ 65acd8d. All line numbers were read.

Existing abstractions referred to below:
- `dvs_alloc_t` (DVS_ALLOC / DVS_RELEASE / DVS_RESIZE): sized alloc/release/resize; `DVS_RESIZE(NULL,...)` = malloc/realloc. No thread-safety contract.
- `dvs_spool_t`: segmented bump pool, 32-bit offsets, stable addresses, mark/release.
- `dvs_arena_t`: doubling bump arena, stable offsets / unstable pointers, mark/release/reset.
- `dvs_stack_t`: block-chained bump stack, stable pointers, push/pop marks.

Threading context (dvs_cube.c): the sequential path is single-threaded. The parallel
path has one shared `cube_ctx C` (queue `C.q`, counters) guarded by `C.mtx`; each
worker thread owns a private `dvs_sat_t` (CaDiCaL) and a `kids[]` buffer. Cube
literal arrays are allocated by one thread (main for seeds, any worker for
children) and freed by whichever worker pops them: **cross-thread frees are the
norm**. Worker `dvs_sat_t` instances are created on the worker thread (586, 710)
and freed by the main thread after join (882, 884): cross-thread free of the whole
instance including CaDiCaL's internal heap.

## 1. Site table

| file:line | function | call | object / purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_cube.c:153 | q_push | realloc | cube queue `cube_t v[]` (24 B/entry) | VECTOR, doubling from 64; **head never compacted** (q_pop just bumps `head`), so the array holds every cube ever pushed, up to MAXCUBES = 4M (~96 MB) | per push; growth log2(total) times, parallel path grows **under C.mtx** (copying up to ~96 MB while every worker blocks) | 532, 798, 804, 815, 850, 891, 973, 983, 994 | Ring buffer (or compact when head > size/2) sized to the live frontier; DVS_RESIZE |
| dvs_cube.c:167 | make_child | malloc((n+1)*4) | literal array of a child cube | CHURN: one per child of every budget-hit cube; k children per split (k = or arity, up to 1024) | per re-split, hot path of the conquer loop; parallel: called under C.mtx from split_children (667) and unlocked from lookahead_children (642) | seq 498/499/500/513/514/518; par 651/657/669 (other thread); unwind 227/241/318/523/679; drain 532/797/803/814/890 | Fixed-size slots of L = nfold + MAXDEPTH + 1 int32 (bound known before the run) from a free-list owned by cube_ctx (lock already held at most alloc/free sites), with a small per-worker cache for the unlocked lookahead path. dvs_spool/arena mark-release does not fit (frees are not LIFO, cross-thread) |
| dvs_cube.c:197 | push_seed_cube | malloc(4) | 1-literal seed cube | PHASE (seed build), consumed individually | <= 4096 (CUBE_ORSPLIT_CAP) or 2 per check | by consumer, as 167 | same slot free-list as 167 |
| dvs_cube.c:301 | lookahead_children | malloc((n+1)*4) | probe buffer = cube lits + one disjunct | SCRATCH | once per budget-hit cube when DV_CUBE_LOOKAHEAD=1 (opt-in) | 312, 319, 325 | Per-worker buffer of capacity L allocated once alongside `kids` (599 / 487) |
| dvs_cube.c:386 | build_cartesian_frontier | malloc(32768*4) | flat disjunct-literal buffer (128 KB) | SCRATCH | once per cube check | 388, 392, 396, 409, 418 | scratch arena / stack mark-release (or keep; once per check) |
| dvs_cube.c:387 | build_cartesian_frontier | malloc(max_ors*4) | group sizes | SCRATCH | once per check | 388, 392, 396, 409, 418 | same scratch mark as 386 |
| dvs_cube.c:395 | build_cartesian_frontier | malloc(ng*8) | `or_group` sort keys | SCRATCH | once per check | 396, 409, 418 | same scratch mark |
| dvs_cube.c:405-407 | build_cartesian_frontier | malloc x3 | `or_set` lits/off/size (read-only shared by workers) | OWNED for the duration of the check (PHASE) | once per check | or_set_free (127) from 409, 437, 463, 972, 1010 | one allocation (three sub-arrays carved from one block) or a per-check arena; read-only so sharing is safe |
| dvs_cube.c:449 | build_cartesian_frontier | malloc(nfold*4) | cartesian seed cube literals | PHASE allocation burst (up to CARTCAP = 200,000 mallocs of ~20 B), freed individually | once per check, up to 200k | by consumer (as 167), 458 on push failure | slot free-list as 167; or carve all seeds from one block and mark "seed-owned" so frees of seeds are no-ops |
| dvs_cube.c:487 | run_sequential | malloc(kids_cap*24) | re-split child buffer | OWNED per run | once per sequential run | 533 | fine as is (or per-check arena) |
| dvs_cube.c:599 | cube_worker | malloc(kids_cap*24) | per-worker re-split child buffer | OWNED per worker run, thread-local | once per worker | 600 (fail), 688 | fine as is; put the 301 probe buffer next to it |
| dvs_cube.c:808-810 | run_parallel | calloc x3 | worker_arg[], port_arg[], dvs_thread_t[] | PHASE (per parallel run) | once per check | 812, 845, 892 | fine as is (tiny, once) |
| dvs_cube.c:941 | dvs_cube_check | malloc(8192*4) | candidate split-bit literals (32 KB) | OWNED for the check (shared read-only by workers) | once per check | 973, 1011 | per-check arena with 386/405 |
| dvs_cube.c:962 | dvs_cube_check | malloc(4096*4) | widest-`or` disjunct list (16 KB) | SCRATCH | once per check, fallback path only | 966 | scratch mark |
| dvs_cube.c:586 / 710 | cube_worker / cube_portfolio_worker | dvs_sat_new_backend(NULL, CADICAL) -> dvs_sat.c:91 + dvs_sat_cadical.c:133 + ccadical_init | per-worker private SAT instance | OWNED, created on worker thread, **freed on main thread** | once per worker per check | 588/600/712 (worker), 882/884 (main) | keep on malloc. Passing a context `dvs_alloc_t` here would require a thread-safe allocator; CaDiCaL heap is not injectable anyway |
| dvs_costguided.c:107 | dvs_solver_set_cost_guided | DVS_ALLOC | CostGuidedCtx | OWNED, already on ctx->alloc | once per ctx | dvs_ctx.c:435 | already pooled |
| dvs_costguided.c:228 | hpwl_cost_ctx_build_index | calloc(n_macros) | per-macro net count | OWNED (in caller's HPWLCostCtx) | once per index build | 290 (hpwl_cost_ctx_destroy), 242 | merge with 241/246 into one CSR block |
| dvs_costguided.c:241 | hpwl_cost_ctx_build_index | calloc(n_macros ptrs) | per-macro net-id row pointers | OWNED | once | 288 | CSR: replace with offsets[] |
| dvs_costguided.c:246 | hpwl_cost_ctx_build_index | calloc per macro | per-macro net-id rows | OWNED, n_macros separate allocations | once per build, n_macros calls | 287 | single CSR array (sum of counts is known after the first pass) |
| dvs_costguided.c:256 | hpwl_cost_ctx_build_index | calloc(n_macros) | fill cursors | SCRATCH | once | 279 (fail path 257 frees the index, fill is NULL) | scratch / stack |
| dvs_costguided.c:335 | costguided_greedy_place | malloc(nm*4) | placement order | SCRATCH | once per greedy call (also once per LNS call) | 364, 489 | scratch mark |
| dvs_costguided.c:585 | dvs_solver_lns_optimize | malloc(nm*8) | working positions | SCRATCH (per call) | once per LNS call | 591 (no), 749 | scratch mark |
| dvs_costguided.c:590 | dvs_solver_lns_optimize | malloc(neigh_sz*4) | neighborhood buffer | SCRATCH | once per LNS call | 591, 748 | scratch mark |
| dvs_oracle.c:118 | dvs_oracle_create | calloc | dvs_oracle_t | OWNED | once | 123, 130 | fine |
| dvs_oracle.c:190 | dvs_solver_set_oracle | calloc | per-context oracle mirror | OWNED | once per attach | 182 (_octx_free) | fine |
| dvs_oracle.c:194 | dvs_solver_set_oracle | strdup | label | OWNED | once per attach | 181 | fine |
| dvs_oracle.c:214 | _grow_vars | realloc | OVar[] (8 B/var) | VECTOR, doubling from 64 | grows on declare; **freed and re-grown on every compile** (_octx_clear 171) | 171, 172 | keep the buffer across compiles (memset instead of free) |
| dvs_oracle.c:266 | _push | realloc | OEntry[] (mirrored adds/pins/excludes) | VECTOR, doubling from 16 | per pin/exclude/add | 179 | fine (DVS_RESIZE if moved to an allocator) |
| dvs_oracle.c:277-282 (via dvs_oracle_core.c:34) | _value_assert | DvsOBuf (realloc) | one `(assert (= vN #b..))` string per pin / exclusion | SCOPE-ish (removed by depth on restore, but depth-0 pins are removed out of LIFO order on reset and exclusions persist), each **>= 4096 B for a ~40 B string** | per pin / exclusion while an oracle is attached (every randomize in a Verilator-style loop) | 160, 399, 447, 267 | Store entries in one append-only text buffer with (offset,len,kind,depth) records; restore = truncate to depth mark; reset = compact. At minimum shrink to fit / strdup the final string |
| dvs_oracle.c:429-433 | dvs_oracle_on_add | DvsOBuf | declarations + asserts of an incremental add | SCOPE-ish (as above) | per incremental add | 160, 447 | same text buffer |
| dvs_oracle.c:298 | _check | DvsOBuf x3 | script (full copy of base + every entry), pins, extra JSON | CHURN per oracle query | per solve with oracle attached | 358 | persistent per-octx scratch buffers, `dvs_ob_clear` instead of free |
| dvs_oracle_core.c:34 | dvs_ob_add | realloc | DvsOBuf growth | VECTOR, doubling, **minimum 4096 B** | every buffer | dvs_ob_free (62) | lower initial cap (64-256) or exact-size first allocation |
| dvs_oracle_core.c:52 | dvs_ob_printf | malloc(n+1) | oversize printf (>512 B) | SCRATCH | rare | 58 | format straight into the buffer (reserve n+1 then vsnprintf) |
| dvs_oracle_core.c:163 / 172 | _split_cmd | calloc / realloc | argv vector | VECTOR doubling from 8, OWNED | once per oracle | 173, 533 | fine |
| dvs_oracle_core.c:176 | _split_cmd | strndup | argv token | OWNED | once per token | 173, 533 | fine |
| dvs_oracle_core.c:360 | _take_resp | malloc | one solver response | CHURN per response | per oracle query (1-3 responses) | 417, 631, 732 | return a view into inbuf, or reuse a per-orc response buffer |
| dvs_oracle_core.c:386 | _read_resp | strdup | unterminated last line at EOF | OTHER (error path) | rare | as 360 | fine |
| dvs_oracle_core.c:415 | _read_verdict | strdup | first error response (taint) | OWNED until next query | rare | 699, 535 | fine |
| dvs_oracle_core.c:449 | _expand_pattern | DvsOBuf / strdup | run directory path | OWNED | once | 534 (as run_dir), 583, 589 | fine |
| dvs_oracle_core.c:547 | dvs_orc_create | calloc | DvsOrc | OWNED | once | 539 | fine |
| dvs_oracle_core.c:558-564 | dvs_orc_create | strdup x4 | tag, mode, verilator_hash, cmdline | OWNED | once | 534-535 | fine |
| dvs_oracle_core.c:577 | dvs_orc_create | malloc(strlen+32) | alternate run dir | OWNED | rare | 589 / 534 | fine (but see bugs) |
| dvs_oracle_core.c:688 | dvs_orc_check | DvsOBuf x3 | errs, witness, check_text (second full copy of the script) | CHURN per query | per oracle query | 805 | persistent per-orc scratch buffers; send script+pins+query without building check_text, build it only when a bundle is written |
| dvs_oracle_core.c:752 | dvs_orc_check | open_memstream | details JSON | CHURN per query | per query | 796 | persistent DvsOBuf |
| dvs_oracle_core.c:769 / 774 | dvs_orc_check | DvsOBuf / open_memstream | failure bundle JSON | CHURN, failures only | rare | 788, 784 | fine |
| dvs_oracle_core.c (inbuf via _drain, ~292) | _drain | DvsOBuf | receive buffer | OWNED, reused (cleared) | per process | 536 | fine |
| dvs_sat.c:91 | dvs_sat_new_backend | xalloc | dvs_sat_t (~800 B incl. 64-deep push stacks) | OWNED | once per SAT instance | 112, 146 | already on dvs_alloc_t when one is given |
| dvs_sat.c:163 | rec_append | xalloc + memcpy + xfree | clause-DB replay log (every literal added, incl. 0s) | VECTOR, doubling from 4096 | per literal while recording (cube parallel prep, or DV_DUMP_CNF); can reach millions of literals | 167 (old block), 144 | DVS_RESIZE (lets realloc/mremap grow in place; header says growth must use it) |
| dvs_sat.c:362 | assume_append_raw | xalloc + memcpy + xfree | assumption queue | VECTOR, doubling from 8, reused across solves (assume_n reset at 260) | per assume; growth rare | 366, 141 | DVS_RESIZE; otherwise fine |
| dvs_sat_kissat.c:147 | dvs_sat_kissat_create | ki_xalloc | kissat_impl_t | OWNED | once per instance | 34, 153 | fine; kissat internals already routed through `alloc` (kissat_init_with_alloc) |
| dvs_sat_cadical.c:133 | dvs_sat_cadical_create | cd_xalloc | cadical_impl_t | OWNED | once per instance | 40, 139 | fine; CaDiCaL internals use its own new/delete (no injection, documented at 10-13) |
| dvs_sat.c:72-80, kissat.c:21-28, cadical.c:27-34 | xalloc/xfree, ki_*, cd_* | wrappers | "allocator or malloc" helpers duplicated three times | OTHER | n/a | n/a | one shared helper next to dvs_alloc_resize (which already treats NULL as malloc) |

## 2. Pooling proposals, ranked by likely impact

Context for the ranking: everything in scope except the cube conquer loop and
the oracle runs at most once per check. The cube loop's unit of work is a
CaDiCaL solve of thousands of conflicts, and an oracle query is an IPC round trip
to z3/bitwuzla, so in both cases malloc time is small next to the work it
wraps. Most of the wins here are memory and lock hold time, not malloc CPU.

1. **Cube queue: ring buffer instead of a grow-only array (dvs_cube.c:150-162).**
   `q_pop` only advances `head`, so `v[]` keeps every cube ever enqueued. That is
   up to 4M x 24 B, about 96 MB, all of it dead entries. In the parallel path the
   doubling `realloc` runs under `C.mtx` and copies the whole array while every
   worker waits. A ring buffer, or compacting when `head > size/2`, bounds it by
   the live frontier and makes growth rare. This is the biggest memory and
   contention item in scope.

2. **Cube literal slots (167, 197, 449, plus probe buffer 301).** Cube literal
   arrays are allocated and freed once per cube, from many threads (allocated by
   one thread and freed by another). Their maximum length is known before the run:
   `L = nfold + MAXDEPTH + 1` (bit splits add 1 per depth, or-pins add 1 per
   group, and depth is capped). Use fixed L-int32 slots from a free-list owned by
   `cube_ctx`. Most alloc/free sites already hold `C.mtx` (651, 657, 667-669, 679).
   For the unlocked lookahead path (642) add a small per-worker slot cache, and
   allocate the probe buffer once per worker next to `kids`. This removes malloc
   and free from inside the global critical section, and removes up to 200k tiny
   mallocs when the cartesian seeds are built. The seeds could also be carved
   from one block. Arena or spool mark/release does **not** fit, because frees
   are neither LIFO nor same-thread. A further option is to store a cube as
   (parent ref, extra literal) with refcounts, so a child costs O(1) instead of
   O(n). That needs atomics, so it is only worth doing if cube depth gets large.

3. **Oracle text storage (dvs_oracle.c:262-283, 429-433; dvs_oracle_core.c:31-33).**
   The DvsOBuf minimum capacity of 4096 B means every mirrored pin or exclusion
   holds 4 KB for about 40 B of text. With an oracle attached in a
   randomize-per-call loop that is about 100x memory overhead, plus one malloc and
   free per pin. Proposal: one append-only text buffer per octx, with entries
   stored as (offset, len, kind, depth). `on_restore(cp)` then becomes "drop
   entries deeper than cp", plus a compaction pass when depth-0 pins are removed
   on reset. Out-of-order removals (reset drops depth-0 pins and keeps depth-0
   exclusions) mean it is not a pure mark/release. The cheap fix is to lower the
   initial cap, or shrink-to-fit in `_value_assert`.

4. **Oracle per-query buffers (dvs_oracle.c:298/358, dvs_oracle_core.c:688/752/805).**
   Each check rebuilds `script`, which is a full copy of the mirrored problem, and
   then `dvs_orc_check` copies it again into `check_text`. Keep persistent
   per-octx and per-orc scratch buffers (clear, do not free), send
   script/pins/query in pieces, and build `check_text` only when a failure bundle
   is written. Low CPU impact because of the IPC, but it halves the peak memory
   per query.

5. **SAT recording log: use DVS_RESIZE (dvs_sat.c:159-173, 357-373).** Growth by
   alloc+memcpy+release goes against the dvs_alloc.h rule that growth must use
   DVS_RESIZE. `rec_buf` holds the whole CNF literal stream when the cube engine
   records for cloning, so each doubling copies tens of MB. `DVS_RESIZE` lets
   glibc realloc/mremap grow in place. Small effort, moderate win on large
   instances.

6. **Per-check scratch arena for the cube setup buffers (386, 387, 395, 405-407,
   941, 962).** These are about 180 KB allocated once per check: a few scratch
   buffers plus the read-only `or_set`/`bits` that workers share. One per-check
   `dvs_arena_t`/`dvs_stack_t` (mark at entry, reset at exit) would clean them up
   in one place. Negligible performance impact; it is mainly a tidiness and
   OOM-path simplification.

7. **costguided CSR index (228/241/246/256) and the LNS/greedy scratch (335, 585,
   590).** Replace the n_macros separate row callocs with one CSR block. The
   scratch buffers are once per call and do not matter for performance. Note that
   `hpwl_cost_ctx_*` is documented (dvs_costguided.h:122-129) as malloc-owned by
   the caller's struct, so moving it to ctx->alloc is an API change.

8. **Do not pool:** worker `dvs_sat_t` instances (cross-thread free, CaDiCaL heap
   not injectable), kissat/cadical impl structs, the oracle and DvsOrc config
   strings. These are all once per object. One small cleanup: fold the three
   copies of the "allocator or malloc" helper into one.

## 3. Bugs noticed (not fixed)

Soundness (wrong UNSAT), all needing OOM or a solver-init failure to trigger:

- **B-a: parallel cube pool can report UNSAT on an undrained queue.**
  `cube_worker` returns early without touching `active` / `unresolved` when its
  private instance cannot be built (587-590) or its `kids` malloc fails (600). If
  every spawned cube worker exits this way (OOM or `ccadical_init` failure) and
  there is no portfolio worker, the queue is never drained. After join,
  `result == C.result == DVS_BB_UNSAT` (provisional, 781), `C.unresolved == 0`, so
  `run_parallel` returns **UNSAT** (866-871) and the leftover cubes are freed at
  890. Fix: after the join, treat a non-empty `C.q` (or a "worker failed to
  start" flag) as `unresolved`.
- **B-b: partial seed frontier counted as exhaustive.** In
  `build_cartesian_frontier` the enumeration loop `break`s on malloc or q_push
  failure (450, 458) and still returns `pushed` when `pushed >= 2` (462-468).
  `build_frontier` likewise `break`s at 342/348 and returns a partial count.
  The caller treats the seeds as a full cover, so if all the seeds pushed are
  UNSAT the result is UNSAT although part of the product was never enqueued.
  Fix: if `pushed < product` (or `< n_or`), mark unresolved or return 0.
- Minor, related: if the cartesian enumeration pushes exactly one cube before OOM
  (462), that cube stays in `q` while the caller falls back to `build_frontier`
  on the same queue (960-966). This is sound (extra region) but leaves
  `total_cubes` out of step.

Memory safety / crashes:

- **dvs_costguided.c:642-653 (LNS fallback), heap overflow.** The fallback
  always writes 2 entries into `unfrozen`, which holds `neigh_sz` entries
  (590). With `neighborhood_size == 1`, or `n_macros == 1` (which clamps
  `neigh_sz` to 1 at 575), it writes 4 bytes past the allocation. With
  `nm == 1` the `do { } while (!unique)` loop never terminates. With `nm == 0`
  it computes `% nm` and divides by zero (619 / 648).
- **dvs_costguided.c:696, stack overflow.** `int32_t new_pos[2*256]` is indexed
  by `actual_neigh`, which is bounded only by the caller's `neighborhood_size`.
  A neighborhood larger than 256 writes past the stack array. (The `trial[512]`
  guard at 704-705 also silently disables LNS for more than 256 macros. That is a
  functional limit, not memory-unsafe.)
- **dvs_oracle_core.c:577.** `malloc` result is not checked before `sprintf(alt, ...)`.
- **dvs_oracle_core.c:360 (_take_resp).** On malloc failure the response has
  already been consumed from `inbuf` (363-365) but NULL is returned. The caller
  reads that as "nothing buffered yet" and the response is silently lost, so
  the verdict wait ends in a timeout.
- **dvs_oracle_core.c:176 (_split_cmd).** An unchecked `strndup` returning NULL
  ends the argv early. The tokens after it are leaked by `_free_orc` (533 stops
  at the first NULL).
- dvs_oracle_core.c:564. `strdup(cfg->cmdline)` is unchecked. A NULL cmdline
  only reaches `%s` in error messages (610, 621) and `dvs_json_cstr`, which
  handles NULL.
- dvs_costguided.c:585. `malloc(0)` when `nm == 0` may return NULL, giving -1
  for an empty problem (only after greedy_place returned 0).

Other:

- dvs_sat.c:121. `DV_DUMP_CNF` turns on recording for **every** SAT instance,
  including every cube worker. Each worker then keeps a second full copy of the
  replayed CNF. This is a diagnostic-only path.
- dvs_sat.c:377 / 363. A transient OOM while queuing an assumption permanently
  taints the instance (`tainted = 1`). That is sound (solve returns UNKNOWN), but
  the instance never recovers.
- dvs_oracle.c:171. `_octx_clear` frees `vars` on every compile and it is
  regrown at once. This is churn, not a bug.
