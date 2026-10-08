# Solving engines

dv-solve has two engines and picks one for each problem. Both give sound
answers (see {doc}`soundness`); they differ in speed and in how their
solutions are distributed.

**Constraint propagation and search (CDCL).** Works on variable domains
(ranges of possible values), narrowing them as it decides values and learning
from conflicts. It is tuned for constrained-random workloads: problems of
moderate size, solved many times with different seeds, where solutions should
be spread across the solution space.

**Bit-blasting.** Translates the problem into a Boolean formula over individual
bits and hands it to a SAT solver (kissat, or optionally CaDiCaL). It is
complete for bit-vectors and scales to the large, formal-verification style
problems that model checkers generate.

## How the engine is chosen

The Python, C and DPI APIs always use the CDCL engine. For SMT-LIB2 input,
the declared logic decides first:

| Logic | Engine |
|---|---|
| `QF_ABV`, `QF_UFBV`, `QF_AUFBV` | bit-blasting |
| `QF_BV`, `ALL`, or no `set-logic` | by the problem's operators (below) |

A `QF_BV` problem goes to the bit-blaster when it looks like a circuit: it
multiplies or divides, or its bitwise, shift, slice and arithmetic operators
outnumber its ordering comparisons (`bvult`, `bvsle` and the like) three to
one. CRC and hash functions, checksums and equivalence checks are like this.
Everything else, the ranges and orderings of constrained-random stimulus,
goes to CDCL. If CDCL has no answer within 20 ms, the problem moves to the
bit-blaster. In `--mode=verilator`, dv-solve chooses differently, by measuring
which engine is faster on each kind of `randomize()` call.

Some problems go to the bit-blaster whatever the logic, because the CDCL engine
can't handle them. These include variables wider than 64 bits and some
operators, such as signed division. When the CDCL engine can't reach an
answer on SMT-LIB2 input, dv-solve normally retries the problem with the
bit-blaster rather than report `unknown`.

## Simulation before SAT

Before it calls the SAT solver, the bit-blaster simulates the circuit on 512
input values at a time. When the inputs the assertions depend on are few
enough that trying every value takes under a millisecond, it does that, which
decides the problem without SAT: a model, or a proof that there is none.
Otherwise it tries a few hundred random inputs, which often find a model
quickly. If trying every input value would take at most about 2 s, that
exhaustive search then runs alongside the SAT solver, and whichever finishes
first answers. Both answers are exact. Simulation is off in `--mode=verilator`.

## Choosing an engine yourself

Use `--engine=cdcl`, `--engine=bitblast` or `--engine=auto` (the default) on
the command line, or set `DV_ENGINE` to `cdcl` or `bitblast`. A problem that
needs the bit-blaster still goes there even if you ask for CDCL.
