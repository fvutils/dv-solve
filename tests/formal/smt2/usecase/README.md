# Use-case benchmarks

Scenario-driven SMT-LIB2 benchmarks for the shared-learning-core project. The design and
catalog are in `docs/usecase_benchmark_design.md`; the project is described in
`docs/shared_learning_core_project.md`.

Layout: `<family>/<case>_<variant>_<knobs>.smt2`, plus `<family>.json` sidecars (to be
written). The variants are `r` (RTL level), `sflat` and `sstruct` (SPL level). Every file
carries `(set-info :status …)`.

Seed files, written by hand on 2026-10-07; the generators replace them in phase U0:

| file | scenario | status |
|---|---|---|
| `arb/arb1_sstruct_n4_p3.smt2` | ARB-1: `next()` grants a ready channel of maximal priority (4 channels, 3-bit priority) | unsat |
| `dma/dma2_sstruct.smt2` | DMA-2: a chunk that completes the transfer leaves no words; the witness is a candidate DMA bug | sat |
