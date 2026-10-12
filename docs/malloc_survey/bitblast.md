> Appendix to [malloc_lifecycle_analysis.md](../malloc_lifecycle_analysis.md). Surveyed at 65acd8d; line numbers refer to that commit. Bugs listed here as "not fixed" may since be fixed: the analysis doc's bug table has their status.

# Heap-allocation survey: bit-blast / AIG engine

Scope: `src/c/dvs_bbsolver.c`, `dvs_bbword.c`, `dvs_bbsum.c`, `dvs_bitblast.c`, `dvs_aig.c`,
`dvs_aig_cnf.c`, `dvs_aigsim.c` (headers in scope contain no allocation sites).
Branch `feat/dynamic-pool`, surveyed 2026-10-12. Read-only; nothing was edited.

## Context: lifetimes and existing abstractions

- **Lifetime of the whole engine.** A `dvs_bbsolver_t` owns the AIG, the CNF encoder, the
  bit-blast arena, bbsum, bbword and the SAT instance. On a cache miss the SMT2 frontend builds a
  fresh one for each check-sat (`smt2_frontend.c:5550`, `5440`, `5666`, `5744`, `5862`) with
  `alloc == NULL`, which means plain malloc. It is freed in `dvs_bbsolver_free`
  (`dvs_bbsolver.c:1778-1804`). `dvs_bbsolver_check_maxsat` builds up to n_softs+2 of them, one
  after another (`3366`, `3396`, `3441`, `3457`). So most "OWNED" sites below are really
  **per-check-sat PHASE** memory.
- **Allocator use today.** Nothing in scope uses `dvs_arena_t`, `dvs_spool_t` or `dvs_stack_t`.
  - `dvs_aig.c`, `dvs_aig_cnf.c` and `dvs_bitblast.c` go through `dvs_alloc_t` via local
    `xalloc/xfree/xrealloc` helpers. They fall back to malloc when `alloc == NULL`.
  - Those local `xrealloc` helpers are alloc + memcpy + free (`dvs_aig.c:49`,
    `dvs_aig_cnf.c:40`). They never use `DVS_RESIZE`, so they get no in-place growth.
  - `dvs_bitblast.c:31-48` has its own chunked bump arena: 4 KB chunks, pointer-stable, freed only
    in `dvs_bitblast_free`.
  - `dvs_bbsolver.c` is mixed. About half its buffers go through `xalloc(S->alloc)` and half
    through raw `malloc/calloc/realloc/free`.
  - `dvs_bbword.c`, `dvs_bbsum.c` and `dvs_aigsim.c` use raw libc only. bbword and bbsum store
    `alloc` and never use it.
  - The net effect breaks the `dvs_alloc.h` promise that "a custom allocator sees every byte".
- **Already good.** `reach_begin` (`dvs_bbsolver.c:490-505`) uses epoch stamps, so it does not
  re-clear per query. `dvs_aig_cnf`'s DFS `stack` and `encoded` bitmap are persistent and grow by
  doubling.

Pattern key: SCRATCH, PHASE, SCOPE, VECTOR, OWNED, CHURN and OTHER, as defined in the task. "Per
check-sat" means once per `dvs_bbsolver_new` → `free` lifetime.

## 1. Site table

| file:line | function | call | object/purpose | pattern | frequency | freed where | suggested pool |
|---|---|---|---|---|---|---|---|
| dvs_bitblast.c:38 | bb_alloc_bits | xalloc | 4 KB (or larger) bump chunk holding every `dvs_bv_t.bits` vector | PHASE (own arena) | once per 4 KB of bit vectors; every bb op allocs from it | dvs_bitblast.c:73 (destroy, walks chunk list) | Keep a bump allocator, but it must stay pointer-stable (bits pointers live in the cache, in bbsum/bbword keys and in vars) → `dvs_stack_t`, or a per-solve arena that is pointer-stable; **not** `dvs_arena_t` (it moves). Use geometric chunk growth. **No NULL check**. |
| dvs_bitblast.c:60 | dvs_bitblast_new | xalloc | bitblaster struct | OWNED | per check-sat | dvs_bitblast.c:76 | per-solve arena |
| dvs_aig.c:63 | ensure_nodes_cap | xrealloc (alloc+copy+free) | AIG node array (`dvs_aig_data_t`, 16 B/node) | VECTOR, ×2 from 64 | log2(N) times per check-sat (checked on every node creation) | dvs_aig.c:170 | `DVS_RESIZE` (lets realloc/mremap grow in place); stays a single owned vector |
| dvs_aig.c:75 | buckets_init | xalloc | unique-table buckets, 64 initial | OWNED | per check-sat | dvs_aig.c:171 / replaced in resize | per-solve arena. **No NULL check before memset (76)** |
| dvs_aig.c:92 | buckets_resize | xalloc (+xfree old at 106) | bucket rehash | VECTOR, ×2 | log2(N) per check-sat | dvs_aig.c:106, 171 | as is (or `DVS_RESIZE` + in-place rehash). **No NULL check before memset (93)** |
| dvs_aig.c:152 | dvs_aig_new | xalloc | AIG struct | OWNED | per check-sat | dvs_aig.c:173 | per-solve arena |
| dvs_aig_cnf.c:54 | ensure_bitmap (for `encoded`) | xrealloc | encoded-node bitmap | VECTOR, ×2 from 64 B | log per check-sat | dvs_aig_cnf.c:259 | fine; `DVS_RESIZE`. **NULL result stored, cap advanced, old block leaked** |
| dvs_aig_cnf.c:54 via 299 (+ release at 275-276) | dvs_aig_cnf_encode (top_level) → ensure_bitmap(`visited`) | xfree, then xrealloc regrow from 64 B | per-flatten visited bitmap (2 bits/node, up to the highest id touched) | **CHURN** | **every top-level `assert_top`**: per constraint, per var bound (1660), per soft, per dist, per **alldiff pair** (2109, O(n²)). Each call frees the bitmap, regrows it by doubling (≈log2(maxid/32) allocs) and memsets up to maxid/4 bytes | dvs_aig_cnf.c:275 (next call), 261 | **Do not free.** Keep it persistent and clear only the bits set, by replaying the leaf/visit list or with per-node epoch stamps like `reach_begin`. Today this is O(K·N) bytes of memset for K top-level asserts on an N-node AIG |
| dvs_aig_cnf.c:71 | ensure_stack | xrealloc | DFS work stack | VECTOR, ×2 from 64 | log per check-sat | dvs_aig_cnf.c:260 | fine (persistent). **No NULL check; leaks old on failure** |
| dvs_aig_cnf.c:248 | dvs_aig_cnf_new | xalloc | encoder struct | OWNED | per check-sat | dvs_aig_cnf.c:262 | per-solve arena |
| dvs_aig_cnf.c:311 | dvs_aig_cnf_encode (top_level) | xrealloc | `leaves` buffer, ×2 from 16 | CHURN (SCRATCH per call) | every top-level `assert_top` (same as above) | dvs_aig_cnf.c:330 | Push leaves onto the persistent `e->stack` above a base mark (the code already uses that trick for the visit stack), or a persistent scratch vector on `e`. **No NULL check** |
| dvs_aigsim.c:67 | dvs_aigsim_new | calloc | sim struct | PHASE | per solve with presim on (default outside Verilator mode, `smt2_frontend.c:5565,5751`), via `_solve_with_sim` 2542 | dvs_aigsim.c:60 (via 2564/2603) | per-solve scratch arena (mark/release around `_solve_with_sim`) |
| dvs_aigsim.c:71,72 | dvs_aigsim_new | malloc | asserts copy, packed assert lits | PHASE | per presim solve | dvs_aigsim.c:59 | same arena |
| dvs_aigsim.c:73,74 | dvs_aigsim_new | calloc (N+1)×4 B each | L/R child arrays (copy of AIG children) | PHASE | per presim solve, O(N) | dvs_aigsim.c:58 | Same arena, or better **share** one cached L/R snapshot with `_diversify` (see P2) |
| dvs_aigsim.c:75,76 | dvs_aigsim_new | calloc / malloc (N+1)×4 B | `slot` map, cone DFS stack | SCRATCH | per presim solve | dvs_aigsim.c:125 / 128 | scratch arena mark/release |
| dvs_aigsim.c:101-105 | dvs_aigsim_new | malloc ×4, calloc | inputs, gate_id, ga, gb, `val` (W words × (in+gates)) | PHASE | per presim solve | dvs_aigsim.c:58-59 | same arena (val is the largest, W×8 B per node) |
| dvs_aigsim.c:257 | dvs_aigsim_model | calloc N+1 | model valuation → becomes `S->node_val` | OWNED (handed off) | per SAT-by-sim verdict | via S->node_val (free at next solve, 2495-2496) | reusable node_val buffer (see P5) |
| dvs_bbword.c:59 | _insert (rehash) | calloc | open-address slot table, ×2 from 256 | VECTOR | log per check-sat (2 tables) | dvs_bbword.c:66, 88 | per-solve arena; route via `W->alloc` |
| dvs_bbword.c:72 | _insert | realloc | key array, ×2 from 256 | VECTOR | log per check-sat | dvs_bbword.c:87 | same |
| dvs_bbword.c:130 | dvs_bbword_new | calloc | bbword struct (`alloc` stored, unused) | OWNED | per check-sat | dvs_bbword.c:146 | per-solve arena |
| dvs_bbword.c:159 | _shape_slot | realloc | shapes payload, sized to tab cap | VECTOR | log | dvs_bbword.c:142 | same. **OOM leaves key without payload → OOB in `_shape`** |
| dvs_bbword.c:352 | _remember_val | realloc | vals payload | VECTOR | log | dvs_bbword.c:143 | same |
| dvs_bbword.c:364,367 | _remember_val | realloc ×2 | terms / coefs (parallel), ×2 from 1024 | VECTOR | log | dvs_bbword.c:144-145 | same |
| dvs_bbsum.c:45 | dvs_bbsum_new | calloc | bbsum struct (`alloc` unused) | OWNED | per check-sat | dvs_bbsum.c:58 | per-solve arena |
| dvs_bbsum.c:185 | _rehash | calloc | slots, ×2 from 256 | VECTOR | log | dvs_bbsum.c:192, 57 | same |
| dvs_bbsum.c:207 | _remember | realloc | entries, ×2 from 256 | VECTOR | log | dvs_bbsum.c:55 | same |
| dvs_bbsum.c:214 | _remember | realloc | terms, ×2 from 1024 | VECTOR | log | dvs_bbsum.c:56 | same |
| dvs_bbsolver.c:236 | assert_top | realloc | assert_tags (session only), ×2 from 64 | VECTOR (truncated on pop = SCOPE-ish) | log | dvs_bbsolver.c:1798 | Merge with assert_nodes (244) into one struct array; they are parallel arrays on different allocators and with different caps |
| dvs_bbsolver.c:244 | assert_top | xalloc + memcpy + xfree(249) | assert_nodes, ×2 from 64 | VECTOR | log | dvs_bbsolver.c:249, 1801 | `DVS_RESIZE` |
| dvs_bbsolver.c:492 | reach_begin | calloc n_vars×4 | epoch-stamped reach marks | VECTOR (exact size, rare) | when n_vars grows | dvs_bbsolver.c:494, 1791 | fine (good pattern); route via S->alloc |
| dvs_bbsolver.c:602 | note_subst | realloc | new_subst list (session), ×2 from 256 | VECTOR | log | dvs_bbsolver.c:1797 | fine |
| dvs_bbsolver.c:676 | try_record_slice | realloc | slices, ×2 from 64; truncated on pop | VECTOR / SCOPE | log | dvs_bbsolver.c:1799 | fine |
| dvs_bbsolver.c:708 | collect_substs_from | realloc (CS_PUSH) | AND-spine flatten stack, ×2 from 32 | CHURN (SCRATCH) | **per constraint** (940 in run_subst_pass; 3287 per root in sess_add) | dvs_bbsolver.c:709, 736 | Persistent scratch vector on S (not reentrant here), or a scratch arena mark/release |
| dvs_bbsolver.c:770,772 | fanout_reserve | realloc ×2 | uses[] / fo_seen[], 1 B per **pool byte** (sized to cache_cap) | VECTOR (follows cache_cap growth) | per solve / per rebase growth | dvs_bbsolver.c:775, 889-890, 1794-1795 | fine as vector; see OTHER note on sparse sizing |
| dvs_bbsolver.c:807 | count_fanout_from | realloc (FO_EDGE) | DFS stack, ×2 from 256 | SCRATCH | per solve (2130) and per sess_add batch (3288) | dvs_bbsolver.c:886, 889 | scratch arena or persistent vector |
| dvs_bbsolver.c:899 | count_fanout | malloc n×4 | constraint roots | SCRATCH | per solve | dvs_bbsolver.c:908 | scratch arena |
| dvs_bbsolver.c:923-925 | run_subst_pass | xalloc ×3 | subst[], resolving[], constraint_skip[] | OWNED (per check-sat) | per solve | dvs_bbsolver.c:1789, 1790, 1800 | per-solve arena |
| dvs_bbsolver.c:928 | run_subst_pass | calloc | rhs_var[] | OWNED | per solve | 528 (stack-low drop), 1792 | per-solve arena |
| dvs_bbsolver.c:930 | run_subst_pass | malloc | slice_head[] | OWNED | per solve | dvs_bbsolver.c:1793 | per-solve arena |
| dvs_bbsolver.c:1013 | bv_for_var (sliced var) | malloc width×4 | temp bit array for slice splice | SCRATCH | per sliced var, once (memoized) | dvs_bbsolver.c:1029 | Allocate directly from the bitblast arena: `dvs_bb_value_bits` copies into it anyway (1028), so build in place or use a persistent scratch |
| dvs_bbsolver.c:1202 | bb_binary (AND/OR) | realloc (BB_PUSH) | iterative AND/OR eval stack, ×2 from 64 frames (≥1 KB) | **CHURN** (reentrant SCRATCH) | per uncached AND/OR expression entered via bb_expr: every and/or top-level constraint, plus nested and/or under ite/not/compare operands | dvs_bbsolver.c:1203, 1223, 1225, 1232 | One persistent stack on S used with a base index (reentrant like `dvs_aig_cnf`'s `saved`), or dvs_stack push/pop |
| dvs_bbsolver.c:1695 | bbsolver_new_ex | xalloc | solver struct | OWNED | per check-sat | 1709, 1803 | — |
| dvs_bbsolver.c:1741 | bbsolver_new_ex | xalloc | vars[] (bb_var_t) | OWNED | per check-sat | dvs_bbsolver.c:1787 | per-solve arena. **No NULL check: memset(NULL) at 1742** |
| dvs_bbsolver.c:1761 | bbsolver_new_ex | xalloc | memo cache, **16 B per problem-pool byte** (indexed by expr byte offset) | OWNED / OTHER (sparse direct map) | per check-sat | dvs_bbsolver.c:1788 | Arena; consider a dense node index / hash. Today it is 16× the pool size, mostly empty |
| dvs_bbsolver.c:1902 | _diversify | malloc N+1 | new model `nv` → S->node_val | OWNED (handed off) | per SAT result with seed≠0, N ≤ 200k: check, resolve, sess_solve, solve_assuming (per cube), rediversify (per cached Verilator/SMT re-randomize) | free at next solve (2696, 3148, 3166, 3331, 2890, 2941, 3259, 1802) | reusable node_val buffer (P5) |
| dvs_bbsolver.c:1903,1904 | _diversify | malloc (N+1)×4 ×2 | L/R child copies | PHASE | same as above | dvs_bbsolver.c:1998-1999 (and early-out paths 1905/1936/1958) | cached AIG snapshot on S (P2) |
| dvs_bbsolver.c:1928-1933 | _diversify | calloc/malloc ×6 | cnt, fo_off, inq, wl, trail, areq: O(N) each | PHASE | same | dvs_bbsolver.c:1991-1997 | cnt/fo_off are CSR build (cacheable); inq/wl/trail/areq are scratch → scratch arena or persistent vectors |
| dvs_bbsolver.c:1955 | _diversify | malloc acc×4 | fo_adj (CSR fanout) | PHASE | same | dvs_bbsolver.c:1993 | cacheable with fo_off (P2) |
| dvs_bbsolver.c:2396,2397 | portfolio (bb_port) | calloc ×2 | worker args, thread handles | SCRATCH | per portfolio solve, tiny | dvs_bbsolver.c:2399, 2416, 2434 | leave (or stack/VLA) |
| dvs_bbsolver.c:2809 | dvs_bbsolver_or_split_lits | xalloc best_k×4 | disjunct list | SCRATCH | per cube split call | dvs_bbsolver.c:2820 | scratch arena |
| dvs_bbsolver.c:2853 | dvs_bbsolver_or_groups | xalloc k×4 | disjunct list | CHURN-lite (in a loop) | per surviving top-level `or` per call | dvs_bbsolver.c:2865 | one buffer of `per_group_cap`, allocated once outside the loop |
| dvs_bbsolver.c:2922 | install_worker_model | malloc N+1 | model from portfolio winner → node_val | OWNED (handed off) | per portfolio SAT | via node_val | reusable node_val buffer |
| dvs_bbsolver.c:3013 | bb_grow_vars | xalloc + memcpy + xfree(3018) | vars[] regrow to **exactly `need`** | VECTOR, **exact growth (no geometric)** | every `dvs_bbsolver_assert` (3112) / `rebase` (3092) that adds vars: incremental lazy-array lemmas (`smt2_frontend.c:5027,5648`), session batches | dvs_bbsolver.c:3018, 1787 | `DVS_RESIZE` with doubling capacity, separate from n_vars. Today it is O(n²) copying when vars arrive a few at a time |
| dvs_bbsolver.c:3022 | bb_grow_vars | xalloc + xfree(3026) | subst[] regrow exact | VECTOR, exact | same | 3026, 1789 | same |
| dvs_bbsolver.c:3033 | bb_grow_vars | realloc | slice_head[] regrow exact | VECTOR, exact | same | 1793 | same |
| dvs_bbsolver.c:3042 | bb_grow_vars | xalloc + xfree(3046) | resolving[] regrow exact | VECTOR, exact | same | 3046, 1790 | same |
| dvs_bbsolver.c:3053 | bb_grow_vars | realloc | rhs_var[] regrow exact | VECTOR, exact | same | 3058, 1792 | same |
| dvs_bbsolver.c:3082 | dvs_bbsolver_rebase | xalloc + memcpy + xfree(3087) | memo cache regrow to max(need, 2×cap) | VECTOR (geometric) | per rebase that extends the pool | 3087, 1788 | `DVS_RESIZE` |
| dvs_bbsolver.c:3195-3197,3202 | dvs_bbsolver_sess_begin | xalloc ×2, calloc, malloc | subst/resolving/rhs_var/slice_head, sized max(n_vars,1) | OWNED (session) | once per session | 1789-1793 (sizes mismatched when n_vars==0) | per-session arena |
| dvs_bbsolver.c:3219 | dvs_bbsolver_sess_push | realloc | frames[] stack, ×2 from 8 | SCOPE (LIFO push/pop; capacity kept) | per push, realloc log(depth) | dvs_bbsolver.c:1796 | fine |
| dvs_bbsolver.c:3277 | dvs_bbsolver_sess_add | calloc n | per-batch skip flags | SCRATCH | per sess_add batch (per check-sat in session mode) | dvs_bbsolver.c:3298 (**leaked if a later early return is added**; none today) | scratch arena / persistent vector |
| dvs_bbsolver.c:3375,3376 | dvs_bbsolver_check_maxsat | xalloc ×2 | priorities, keep flags | SCRATCH (but see bug: `keep` escapes) | per maxsat call | dvs_bbsolver.c:3378-3379, 3480-3481 | — |
| dvs_bbsolver.c:3426 | dvs_bbsolver_check_maxsat | xalloc | preference order | SCRATCH | per conflicting maxsat call | 3442, 3449, 3477 | — |
| (node_val frees) 1802, 2495, 2537, 2696, 2890, 2928, 2941, 3148, 3166, 3259, 3331 | various | free | per-solve model valuation; one owner, replaced every solve | OWNED, but churned once per solve | per solve / per rediversify / per cube | — | keep one N+1 capacity buffer on S plus a valid flag (P5) |

## 2. Pooling proposals, ranked by likely performance impact

**P1. The `visited` bitmap in `dvs_aig_cnf_encode` (top-level) is freed and rebuilt on every
top-level assertion. Highest impact; this is algorithmic, not just allocator overhead.**
`dvs_aig_cnf.c:274-276` frees the bitmap and then regrows it from 64 B by doubling. `ensure_bitmap`
memsets every byte up to bit 2·maxid. Every `assert_top` pays this cost:

- one per constraint;
- one per bounded variable (`assert_var_bounds`, 1660);
- one per soft and per dist;
- one per **alldiff pair**: `_bb_encode_alldiff` (2109) asserts O(n²) pairs.

The total is about K × (log2(N/32) mallocs + N/4 bytes of memset). Two examples:

- A 64-var alldiff on a 100k-node AIG: about 2k calls, about 50 MB of memset and about 20k
  malloc/free.
- A 10k-constraint instance: about 10k × N/4 bytes.

Fix: keep the bitmap and clear only the bits set. Those are exactly the ids pushed during the walk,
so record them in a touched list, or switch to per-node `uint32` epoch stamps as `reach_begin`
already does. Also move `leaves` (311) onto the persistent `e->stack` above a mark. That removes
both CHURN sites.

**P2. Per-solve O(N) buffers in `_diversify` and `dvs_aigsim_new`; cache an AIG "snapshot" on S.**

- `_diversify` (1902-1955) allocates 10 arrays of N+1 entries and rebuilds the L/R child copy and
  the fanout CSR on **every** SAT result. That includes every cube SAT (`solve_assuming`, 2892) and
  every `rediversify`, which serves cached repeat-randomize in the frontend (`smt2_frontend.c:5382,
  5537`) on an AIG that has **not changed**.
- `dvs_aigsim_new` (73-76, 101-105) builds another L/R copy, plus slot/stack/val, per presim solve.

Proposal:

- Keep `L/R/fo_off/fo_adj` on S, keyed on `dvs_aig_num_nodes`. Rebuild only when N grows, or
  extend incrementally, since node ids are append-only.
- Keep `inq/wl/trail/areq/cnt` as persistent capacity vectors, or take them from a per-solve
  scratch arena with mark/release (`dvs_arena_t` is fine: these are index arrays, so moving between
  phases does not matter).
- Better still, let aigsim and diversify read children straight from the AIG node array through an
  accessor instead of copying.

Expected effect: removes O(N) memory traffic per randomize in the cached and cube paths, up to the
N ≤ 200k cap.

**P3. One per-check-sat arena for the whole bbsolver object graph.** The frontend builds and
destroys an entire `dvs_bbsolver_t` per check-sat on a cache miss, and MaxSAT builds n+2 of them.
Each one makes about 30 separate allocations: the struct, vars, cache, subst, resolving, skip,
rhs_var, slice_head, the AIG nodes and buckets, cnf bitmaps and stack, bitblast chunks, and the
bbsum/bbword structs and tables. None of these is freed before the solver dies, except vectors that
regrow.

- Give `bbsolver_new_ex` a pointer-stable block arena (`dvs_stack_t`, or chunked like
  `dvs_bitblast.c`) for the fixed-size objects. Leave the growable vectors (AIG nodes, cache,
  tables) on `DVS_RESIZE` through the same `dvs_alloc_t`.
- Route bbword, bbsum, aigsim and the raw-malloc bbsolver fields through `S->alloc`. bbword and
  bbsum already store `alloc` and ignore it.
- Payoff: fewer malloc calls per solve (small compared with SAT time on hard instances, but it
  matters in the Verilator easy band), plus full allocator visibility as `dvs_alloc.h` intends.
- The bitblast arena itself is fine. Use geometric chunk sizes instead of a fixed 4 KB, so large
  AIGs make fewer malloc calls and waste fewer abandoned chunk tails.

**P4. Exact-size regrowth in `bb_grow_vars` (3013-3060).** On every incremental
`dvs_bbsolver_assert` or `rebase` that introduces new vars, vars, subst, slice_head, resolving and
rhs_var are each reallocated to exactly `need` and fully copied. The lazy array refinement loop and
session batches add a few vars at a time, so this is O(V²) copying plus 5 malloc/free per assert.
Fix: separate `cap_vars` from `n_vars` and grow by doubling with `DVS_RESIZE`. This also fixes the
size-accounting bugs below.

**P5. Reentrant scratch stacks that are malloc'd and freed per expression or constraint.**

- `bb_binary` AND/OR stack (1202): at least 1 KB per uncached AND/OR subtree.
- `collect_substs_from` (708): per constraint.
- `count_fanout_from` (807): per solve or batch.
- `count_fanout` roots (899).
- `bv_for_var` slice bits (1013).
- `sess_add` skip (3277).
- `or_groups` ds (2853): in a loop.

Proposal: one `dvs_arena_t` scratch on S used with `mark`/`release` around each use. It is
reentrant by construction, since nested AND/OR evaluation takes a nested mark. Alternatively, a
persistent vector with a base index, as `dvs_aig_cnf` already does with `saved`/`leaf_start`.
Moderate impact: these run once per constraint or boolean subexpression during encoding, which is
cheap relative to SAT but shows up in the many-small-solves (Verilator) regime.

**P6. `node_val` is freed and re-malloc'd on every solve.** About 12 sites free it, and 1902, 2922
and `dvs_aigsim.c:257` allocate it again. Keep one buffer with capacity ≥ N+1 plus a "valid" flag.
`dvs_aigsim_model` and `install_worker_model` would fill a caller-provided buffer. Low-to-moderate
impact (one O(N) malloc per randomize), but it is trivial to do together with P2.

**P7. Minor or none.**

- Hash tables and vectors in bbword, bbsum, aig and cnf all grow by doubling. They are fine apart
  from allocator routing and `DVS_RESIZE` in place of alloc+copy+free.
- `frames[]` (3219) is already a correct SCOPE stack.
- maxsat scratch, portfolio `wa/th` and `or_split_lits` are rare and tiny.
- The memo cache (1761) and `uses/fo_seen` (770) are sized per **pool byte**: 16 B + 2 B per byte
  of problem pool, almost all empty. That is a memory-footprint issue rather than churn. A dense
  per-node index would cut it about 10×.

## 3. Bugs noticed (reported, not fixed)

1. **Suspected wrong-model (soundness): `dvs_bbsolver_rediversify` after a non-SAT-solver model**
   (`dvs_bbsolver.c:2934-2945`). It frees `node_val` and calls `_diversify`. With
   `S->node_val == NULL`, `_diversify` reads the base model from the SAT solver via
   `dvs_aig_cnf_value` (1918-1919). If the last SAT verdict came from simulation or the affine
   check (`_solve_with_sim`: 2493-2496, 2537), or from `install_worker_model` (portfolio), the
   SAT solver never produced a model. Its values are then stale or undefined, and the starting
   valuation is never checked against the assertions; only flips are. The frontend calls
   `rediversify` on the cached-SAT path (`smt2_frontend.c:5537`) for a solver with presim on
   (5565). Even when `_diversify` early-returns (seed 0 or N > 200k), readback falls back to the
   unsolved SAT instance. **Needs a repro.** Likely trigger: a non-Verilator-mode repeat check-sat
   on a small instance that simulation solves.
2. **Dangling `soft_keep` in the returned solver** (`dvs_bbsolver_check_maxsat`). Lines 3398,
   3443 and 3459 set `bb->soft_keep = keep`, and `keep` is freed at 3481 while `*out_bb` still
   points at it. It is read only in `_bb_encode` (2154-2158), so the pointer is latent. It becomes
   a use-after-free if the returned solver is ever re-encoded, rebased or re-checked.
3. **`dvs_aig_cnf.c:274-275` releases with size 0.** `visited_cap` is zeroed *before*
   `xfree(e->alloc, e->visited, e->visited_cap)`. That is harmless with malloc but wrong for any
   sized `dvs_alloc_t` (pool accounting, `DVS_DEBUG_ALWAYS_MOVE` fill).
4. **Unchecked allocation results (crash or leak on OOM):**
   - `dvs_bitblast.c:38`: chunk dereferenced without a check.
   - `dvs_aig.c:75-76, 92-93`: memset of a possibly NULL bucket array.
   - `dvs_aig_cnf.c:54-57, 71-75, 311-315`: NULL stored into `*buf`, `e->stack` or `leaves`, cap
     advanced, old block leaked. The local xrealloc frees the old block only on success.
   - `dvs_bbsolver.c:1741-1742`: `memset(S->vars)` with no NULL check.
5. **Sized-release mismatches in `dvs_bbsolver.c`** (wrong only for a sized custom allocator):
   - `sess_begin` (3191-3196) allocates subst/resolving with `n = max(n_vars,1)`, but they are
     freed with `n_vars` (0) at 1789-1790 and as `old` (0) at 3018/3026/3046.
   - `bb_grow_vars` partial OOM (3021-3049): `vars` (and possibly subst or slice_head) is already
     regrown to `need`, but `n_vars` stays `old`, so 1787-1790 release with the wrong size.
6. **`dvs_bbword.c` OOM leaves an index without a payload.** `_insert` adds the key before the
   payload array grows. If the realloc at 159 fails, `_shape_slot` returns NULL, but the key stays
   in `shape_tab`. A later `_shape()` (169-171) returns `&W->shapes[i]` past `cap_shapes`, which
   is an out-of-bounds read. The vals path sets `W->off` at 353, which guards it. The shapes path
   has no such guard. `_remember_val` 364-368 also partially grows terms but not coefs; that case
   is benign.
7. **Allocator bypass.** `dvs_bbword_t.alloc` and `dvs_bbsum_t.alloc` are stored and never used.
   Many bbsolver fields mix `xalloc(S->alloc)` with raw libc. The `assert_nodes` (xalloc) and
   `assert_tags` (realloc) parallel arrays sit on different allocators with different capacities.
