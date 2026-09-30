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

For SMT-LIB2 input, the declared logic decides:

| Logic | Engine |
|---|---|
| `QF_BV`, `ALL`, or no `set-logic` | CDCL |
| `QF_ABV`, `QF_UFBV`, `QF_AUFBV` | bit-blasting |

Some problems go to the bit-blaster whatever the logic, because the CDCL engine
can't handle them. These include variables wider than 64 bits and some
operators, such as signed division. When the CDCL engine can't reach an
answer, dv-solve normally retries the problem with the bit-blaster rather than
report `unknown`.

## Choosing an engine yourself

Use `--engine=cdcl`, `--engine=bitblast` or `--engine=auto` (the default) on
the command line, or set `DV_ENGINE` to `cdcl` or `bitblast`. A problem that
needs the bit-blaster still goes there even if you ask for CDCL.
