# E2.3 calibration (monolithic portfolio, mcm/68) — 2026-07-20

Isolate "backend diversity" before building clause sharing. `--engine=bitblast`,
`DV_PORTFOLIO=2`, single seed, 500 s cap, sequential. New knob `DV_PORT_BACKENDS`
(zsp_bbsolver.c `bb_port_assign`): `cadical` forces all workers CaDiCaL w/ spread
seeds; `kissat` all kissat; unset = default kissat/CaDiCaL mix.

| config | backends | mcm/68 | wall | RSS |
|---|---|---|---|---|
| C1 | kissat + cadical (current B1) | **timeout** | >500 s | 580 MB |
| C2 | cadical × 2 (seed-diverse) | **sat** | 439 s | 639 MB |

**Findings (n=1, heavy-tail — directional only):**
1. The documented 323 s 2-backend reference did NOT reproduce (C1 >500 s). Not a
   regression (C1 uses the unchanged mixed path) — raw heavy-tail variance. **Any
   monolithic timing claim needs 5-seed medians; single-seed portfolio numbers are
   near-meaningless on mcm/68.**
2. At this seed cadical×2 BEAT kissat+cadical — kissat looks like dead weight on
   mcm/68 (never wins; two diverse cadical shots > cadical+kissat). Inverts the
   checklist's premise that the kissat/cadical pair is the strong diversity baseline.
3. **Both monolithic configs (~440–500 s) are ~4× slower than cube W=1 (108 s).**
   The monolithic portfolio is a dominated branch on mcm; clause sharing on it could
   not beat the cube path.

**Decision:** paused the clause-sharing build. E0.3 + this calibration show
parallelism/portfolio is OFF-AXIS for mcm — the cube partition already cracks /68
sequentially, and the unsolved wall (/140, /176) is a per-solve STRENGTH gap
(word-level preprocessing), which sharing does not address. See the strategic
reconsideration in the checklist / session notes.
