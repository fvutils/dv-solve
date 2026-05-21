# Tier 2: Sequential RTL Benchmarks

## Quick Start

```bash
# Generate Tier 2 SMT2 files (direct path via zuspec-be-fv)
direnv exec . python tests/formal/tier2/generate_tier2_smt2.py

# Run BMC baselines (z3 + bitwuzla)
direnv exec . pytest tests/formal/tier2/test_tier2_bmc_baseline.py -v

# Run k-induction baselines
direnv exec . pytest tests/formal/tier2/test_tier2_kind_baseline.py -v
```

## Benchmark Models

All models are Zuspec RTL components using `@zdc.sync` with embedded
formal properties (`assert` for assertions, `zdc.cover()` for cover goals).

| Model | Description | Registers | BMC Depths | Properties |
|-------|-------------|-----------|------------|------------|
| `counter_overflow` | N-bit up-counter | count | 1-20 | count < 256 |
| `fifo_ptr_valid` | Circular FIFO | wr_ptr, rd_ptr, count | 1-16 | count <= DEPTH |
| `fsm_onehot` | 4-state one-hot FSM | state | 1-32 | state is one-hot |
| `alu_pipeline` | 3-stage pipelined ALU | s1_*, s2_*, result | 1-8 | result==0 when !vld |
| `regfile_rdwr` | 4-entry register file | r0-r3, rd_data | 1-4 | r0 < 256 |
| `arbiter_fairness` | 4-port priority arbiter | grant, last_grant | 1-16 | grant is one-hot |
| `shift_register` | 8-bit SIPO shift reg | dout | 1-16 | (cover only) |
| `timer_watchdog` | Programmable timer | counter, expired | 1-32 | counter < 256 |

## Solver Support

| Solver | Tier 2 Support | Notes |
|--------|:-------------:|-------|
| z3 | Yes | Full QF_UFBV + declare-sort support |
| bitwuzla | Yes | Full QF_UFBV support |
| boolector | No | Does not support `declare-sort` |
| dv-solve-smt2 | No | Requires Phase 4 (define-fun, declare-sort) |

## Adding a New Model

1. Create a new `.py` file under `tests/formal/tier2/models/`.
2. Follow the `counter_overflow.py` pattern: `zdc.Component` base,
   explicit `clk`/`reset` ports, `@zdc.sync(clock=..., reset=...)`.
3. Add an entry to `tests/formal/tier2/models/benchmark_config.py`.
4. Run `generate_tier2_smt2.py` to generate SMT2 files.
5. Run the baseline tests to verify.

## Generation Details

The generation script (`generate_tier2_smt2.py`) applies two important
transformations beyond the standard `generate_bmc_smt2`:

1. **Reset at state_0:** Asserts `reset=true` at `state_0` and
   `reset=false` at all subsequent states, ensuring the first transition
   properly initializes registers.

2. **Skip assertion at state_0:** The assertion disjunction checks
   states 1..depth, skipping state_0 (which has unconstrained register
   values before the reset transition takes effect).
