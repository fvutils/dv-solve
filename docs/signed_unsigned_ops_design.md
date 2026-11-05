# Signed and unsigned operators (design note, 2026-11-05)

Status (2026-11-05): steps 1-2 done; step 3 revised and done for the SV
builder (below). Step 4 in progress: 4a (CDCL takes typed operators in an
EXPLICIT problem, through elaboration's lowering, at widths up to 62) done;
the SMT front-end emits typed SDIV/SREM/SMOD; 4b slices 1-2 (unsigned 64-bit
compares, `/`, `%`, `>>` on CDCL) done; slice 3 (every unsigned 64-bit
operator stays on CDCL: div/rem/lshr, add/sub/mul/shl/neg, and/or/xor/not,
ite, and 64-bit concat) done; the soundness campaigns' protocol-door pin
failures were CDCL declines (64-bit concat, a `!` compared as a truth), now
compiled, and the campaigns are clean. Signed 64-bit done: `bvsdiv`/`bvsrem`/
`bvsmod` and `bvashr` compile on CDCL at up to 64 bits and the SMT front end
routes them there (only wider goes to bit-blast). Pins on AUTO's bit-blast
fall-back next. Plan: docs/solver_gap_plan.md, step 5.

## Why

The problem IR's binary operators (`dvs_binop_t`, `dv_solve.h`) carry no
signedness. Each consumer works it out again from operand types, and where
that runs out, the problem goes to bit-blast. Making signedness part of the
operator lets every engine read it directly, and lets CDCL take the full
unsigned 64-bit range. Today that range costs us:

- **SMT front-end:** any width-64 arithmetic goes to bit-blast ("wide-arith",
  `smt2_frontend.c:511`), and so do signed div/rem ("sdiv-srem", :2810),
  `bvsmod` (:2830), and sign-extends to 64 bits ("sext64", :2211).
- **CDCL compile:** it declines an unsigned 64-bit variable bounded at 2^63
  or above (`_rng64`, `dvs_compile.c:849`). It also declines unsigned
  div/mod, bitwise ops and shifts with a negative-looking bound (`_binop_exact`,
  :944-961). So `mul_div_64` fails on CDCL, and on the API before step 3a.
- **Ambiguity:** a bound of `INT64_MAX` read as "full range" by one engine
  and as 2^63-1 by another (fixed in bit-blast in step 3a).

## What exists already

- **Signedness on leaves and conversions.** It is recorded on constants
  (`ExprConst.is_signed`), variables (`VarSpec.is_signed`), extends
  (`sign_extend`) and the casts (`EXPR_SV_CAST`, `EXPR_CAST`). It is not on
  operator nodes: `ExprBinary` is `{kind, op, lhs, rhs}`, 16 bytes.
- **Four separate rebuilds of the SV typing rules (`dvs_sv.h:9-30`):**
  - SV elaboration (`dvs_sv_elaborate`, `dvs_sv.c:975`), which makes
    operands agree in width and signedness and inserts `SV_CAST`;
  - the bit-blaster's `bb_signed` (`dvs_bbsolver.c:414`);
  - the validator (`dvs_validate.c:140-232`);
  - the SMT emitter (`dvs_smt2_emit.c:160-219`), plus CDCL's
    `_expr_has_signed` (`dvs_compile.c:1299`).
- **The SMT front-end lowers signed operations into unsigned ones:**
  - `bvslt` becomes an MSB flip plus an unsigned compare
    (`_translate_signed_cmp`, :1911);
  - `bvsdiv`/`bvsrem` become a divide of the magnitudes plus a sign fix (:1766);
  - `bvashr` becomes cast, `ASHR`, cast (:3182).
- **CDCL already has an unsigned notion.**
  - `VAR_SIGNED` per variable. An unsigned 64-bit domain is held as a uint64
    pattern in the int64 slots (`var_lo64`/`var_hi64`), and compared unsigned
    by `var_b_lt`/`var_b_min`/`var_b_max` (`dvs_ctx.h:586-611`).
  - `+ - * <<` at width 64 are exact and wrap-aware (`bvadd/bvsub/bvmul/bvshl_64`,
    modular intervals, `dvs_prop_templates.c:2148-2273`).
  - `dvs_i128.h` has overflow-checked u64/i64 arithmetic and a portable i128.
  - The gap is the int64 interval code for `/ % & | ^ >>` and the bound
    comparisons, plus the compile guards above. The `-4` vs `2^64-4`
    ambiguity in intermediate results is documented at `dvs_ctx.h:595`.

## Proposal

### 1. Typed operators: new enum values, appended

Append typed variants after the current last value (`DVS_BIN_ASHR` = 18):

| new | meaning | SMT-LIB |
|---|---|---|
| `DVS_BIN_ULT ULE UGT UGE` | unsigned compare | `bvult` … |
| `DVS_BIN_SLT SLE SGT SGE` | signed compare | `bvslt` … |
| `DVS_BIN_UDIV UREM` | unsigned divide/remainder | `bvudiv bvurem` |
| `DVS_BIN_SDIV SREM SMOD` | signed (truncating) divide/remainder, floor-mod | `bvsdiv bvsrem bvsmod` |
| `DVS_BIN_LSHR` | logical right shift | `bvlshr` |

- `ASHR` keeps its meaning.
- `+ - * & | ^ <<` and `== !=` need no variants: their result bits are the
  same either way, and the wrap-aware CDCL propagators already cover them.
- Operator signedness says how operands are read. Overflow for `+ - *` is
  modular at the node's width either way.

Why appended enum values, and not a flag bit on `ExprBinary`:
- Values 0–18 keep their meaning and the node layout stays 16 bytes, so pyvsc,
  zuspec (`zuspec-be-sw` emits `DVS_BIN_*` and relies on SV typing) and the
  Python constants keep working unchanged.
- Each consumer switches on one value, with no "untyped / signed / unsigned"
  three-way state packed into a field.
- A signedness field would change an installed header's struct layout (or
  overload the `op` word with mask bits every switch must strip).

### 2. Untyped operators stay valid input; elaboration types them

The SV-typed operators (`LT`, `DIV`, `MOD`, `RSHIFT`, …) remain the SV
builder's input. `dvs_sv_elaborate` already settles every operand's type, so
it rewrites each one to its typed form. After elaboration, an EXPLICIT problem
holds only typed operators wherever signedness matters.

- `RSHIFT` on an unsigned operand becomes `LSHR`.
- The signed `>>` lowering (cast, shift, cast) can stay or become `LSHR` on a
  reinterpretation.
- The SMT front-end emits typed operators directly and drops its lowerings:
  the MSB-flip compare, the magnitude divide, the `ASHR` cast pair.
- **Invariant:** downstream of elaboration, no engine reconstructs operator
  signedness from types. `bb_signed`, the validator's type function for
  operators, and `_expr_has_signed` collapse to reading the operator. The
  validator still types the original, un-elaborated problem for SV callers.

### 3. CDCL takes the full unsigned 64-bit range

- **Storage stays the same:** an unsigned 64-bit domain is a uint64 pattern in
  the int64 slots, with `VAR_SIGNED` clear.
- **The operator picks the arithmetic.** Where it disagrees with an operand
  variable's flag (a signed compare of an unsigned variable), compile adds a
  reinterpretation aux variable, reusing the existing "`bvadd` with zero" path
  (`dvs_compile.c:2245-2283`).
- **Exact intermediates:** propagators compute in `dvs_i128` (or u64 with
  overflow flags), and clamp to the variable's representable range before
  tightening. That removes the `-4` vs `2^64-4` ambiguity.
- **Unsigned 64-bit paths** for `UDIV UREM LSHR & | ^` and the `U*` compare
  propagators. Signed counterparts for `SDIV SREM SMOD SLT…`; SDIV/SREM/SMOD
  are new to CDCL.
- **`_rng64` and `_binop_exact`** stop refusing by bound value and dispatch on
  the typed operator. The three repeated binop-to-propagator dispatch sites
  (`dvs_compile.c` :1100, :1359, :3115) become one table.
- **Audit** every bound read that uses signed `<` or `-` on 64-bit values:
  `dvs_search.c` (value picking), `dvs_placement.c`, `dvs_explain.c`,
  `dvs_lcg.c`, `dvs_costguided.c`.
- **Then drop** "wide-arith", "sext64" and "sdiv-srem"/"smod" from the SMT
  front-end's bit-blast routing. Bit-blast gets typed operators too: its
  signed div/mod, unsupported today (`dvs_bbsolver.c:1944`), becomes a
  standard encoding.

## Revision (2026-11-05): typed operators lower during elaboration

About 300 places in compile, bit-blast and elaboration recognise patterns on
the SV operators (orderings, ranges, samplers). Rewriting SV comparisons into
typed ones would have kept results correct but silently switched those
matchers off. So it goes the other way:

- **Typed operators are the input language.** Any caller may use them, with
  SMT-LIB semantics.
- **Elaboration lowers each typed operator** (`_typed_node`, `dvs_sv.c`) to
  SV operators on operands cast to its signedness:
  - a comparison becomes the SV comparison;
  - `LSHR` becomes `>>` of an unsigned value;
  - division and remainder are guarded at a zero divisor, the way the
    SMT-LIB front end guards `bvudiv`;
  - `SMOD` is fixed up from the remainder.

  Every engine then sees the operators its matchers know.
- **The validator evaluates the typed operator itself,** independently of the
  lowering, so it checks the lowering.
- **Bit-blast still encodes typed operators natively.** That serves EXPLICIT
  problems that carry them: the SMT-LIB front end's.
- **The SMT front-end keeps its lowerings** until CDCL takes typed operators
  (step 4). An EXPLICIT problem skips elaboration, so CDCL would see the typed
  operator itself; emitting SDIV/SREM/SMOD was tried and cost pure CDCL its
  division by a variable (plan, step 5 notes).
- **The invariant becomes:** after elaboration, signedness is carried by
  operand types (casts), uniformly. The reconstructions agree because there
  is one lowering, rather than because no engine reconstructs. CDCL's full
  unsigned 64-bit range needs its propagators and compile guards to handle
  unsigned 64-bit operands (step 4), not new operators inside the engine.

## Work, in order

| # | change | files | check |
|---|---|---|---|
| 1 | Enum values, builder/Python constants, `names[]` dump table, shape counter, oracle emitter (`dvs_smt2_emit.c`) | `dv_solve.h`, `problem.py`, `dvs_validate.c`, `dvs_shape.c`, `dvs_smt2_emit.c` | existing suites unchanged |
| 2 | Validator and bit-blaster evaluate typed ops (bit-blast: signed div/rem/mod) | `dvs_validate.c`, `dvs_bbsolver.c` | bit-exact tests against the soundness IR evaluator (`tests/formal/soundness`) for every typed op at widths 1, 8, 63, 64 |
| 3 | Elaboration emits typed ops; the SMT front-end emits them directly | `dvs_sv.c`, `smt2_frontend.c`, `doors.py` | soundness campaign; the SMT-LIB conformance tests; `test_const_wide.py` |
| 4 | CDCL: typed dispatch, exact intermediates, unsigned/signed 64-bit propagators, compile guards | `dvs_compile.c`, `dvs_prop_templates.c`, `dvs_ctx.h` | `test_dvs_wide_arith.c`, `test_prop_exhaustive.c` extended to typed ops, feature-coverage W rows, `mul_div_64` and `memmap3` on CDCL |
| 5 | Bound-read audit; drop the wide-arith/sext64/sdiv routing | `dvs_search.c`, `dvs_placement.c`, `dvs_explain.c`, `dvs_lcg.c`, `dvs_costguided.c`, `smt2_frontend.c` | simbench gap benches; rand-core suite on both front-ends |

Steps 1–3 change no engine's behaviour: they type what each engine already
computes. Step 4 is the CDCL work, and step 5 removes the workarounds.

## Decisions (2026-11-05)

1. **Typed operators are public:** any caller may use them, SV callers
   (pyvsc, zuspec) included. The untyped SV operators remain valid input, so
   no SV caller has to change.
2. **`SMOD` is a real IR operator,** encoded directly by bit-blast and CDCL,
   not lowered by the front-end.
3. **Wider than 64 bits is out of scope here, but wanted soon:** CDCL's
   strength should reach wide fields too. Typed operators are the first step:
   a CDCL search tier above 64 bits (today tier 2 has bounds but no search)
   follows as its own plan item.
