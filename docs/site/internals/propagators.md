# Propagators

The CDCL engine (see {doc}`architecture`) enforces constraints with
*propagators*. Compiling turns each constraint into a few of them, joined by
auxiliary variables; each one narrows the domains of its variables whenever a
domain it depends on changes. This page lists them by what they enforce. Like
the rest of this section, it describes the implementation and may change.

## How a propagator works

A propagator reasons about **bounds**: it reads the lowest and highest value
each of its variables can still take and raises lower bounds or lowers upper
bounds that no solution can use. When a bound would cross the other, the
constraint can't hold and the search backtracks. When the constraint holds
for every value left, the propagator retires until the search backtracks
past that point.

Propagators exist in two storage variants, chosen by how a variable's bounds
are held, not by its declared width. Narrow variables (below 32 bits, or
signed 32-bit) keep their bounds inline; unsigned 32-bit and 33–64-bit
variables keep 64-bit bounds. The rules are the same for both.

For clause learning, a propagator also *explains* each narrowing: it names
the bounds of other variables that forced it. An explanation may cite more
than was strictly needed, which only weakens what is learned, but never
less, which could make the learned clause wrong. A propagator that can't
explain a narrowing makes the search fall back to plain backtracking for that
conflict.

## Comparison

| Relation | Propagation |
|---|---|
| `x <= y`, `x < y` | `x` is at most `y`'s upper bound (less one for `<`); `y` is at least `x`'s lower bound (plus one) |
| `x == y` | both take the intersection of their ranges |
| `x != y` | once one is fixed, its value is removed from the other's range if it lies at an end |

## Arithmetic

Two families. The general one works on exact integer values, as the
SystemVerilog rules produce when an expression is wide enough not to wrap;
the modular one works on values that wrap at a fixed width.

| Relation | Propagation |
|---|---|
| `r == a + b` | `r`'s range is the sum of the operand ranges, and each operand is narrowed from `r` and the other |
| `r == a * b` | `r` lies between the products of the operands' ends; with one operand fixed, the other is narrowed from `r` |
| `r == a / b`, `r == a % b` | from the operands' ranges, with division by zero kept out; signed and truncating when an operand is signed |
| `r == -a` | `r`'s range is `a`'s, negated |
| modular `+`, `-`, `*`, `<<`, and `+` of a constant | reason over the range as an interval that may wrap past the top of the width |

## Bitwise and shifts

| Relation | Propagation |
|---|---|
| `r == a & b` | exact when both are fixed; otherwise, with a non-negative operand, `r` lies between 0 and that operand's upper bound |
| `r == a \| b` | exact when both are fixed; otherwise, with both non-negative, `r` is at least the larger lower bound |
| `r == a ^ b` | exact when both are fixed; with one operand a constant power of two, propagates both ways |
| `r == ~a` | the complement within the width, reversing the range (`-a-1` for a signed `a`) |
| `r == a << s` | for non-negative values, from `a`'s range and the shift amount's |
| `r == a >> s` | logical for unsigned `a`, arithmetic for signed; the shift amount is limited to the width |

## Control

| Relation | Propagation |
|---|---|
| `r == (c ? a : b)` | once `c` is decided, `r` and the chosen arm share a range; before that, `r` stays within both arms' ranges, and `c` is decided if `r` excludes one arm |
| `g == (x <= y)`, `g == (x == y)` | *reification*: once `g` is decided, the comparison (or its negation) is enforced; once the comparison is decided by the ranges, `g` is set |
| `g → x <= k` (or `>=`) | *implication*: enforced only when the guard `g` is 1; used for soft constraints |

Any propagator can also be *guard-gated*: it acts only while a given 0/1
variable is 1, retires when it is 0, and waits while it is undecided.

## Structure

| Relation | Propagation |
|---|---|
| `x inside {...}` | narrows `x` to the smallest and largest member; the gaps become holes |
| `r == a[hi:lo]` | `r` follows the selected bits of `a`, and a fixed `r` narrows `a` |
| `r == {hi, lo}` | `r` follows from the parts; `hi` follows from `r`, and so does `lo` once `hi` is fixed |
| an OR of up to 16 comparisons | once all comparisons but one are false, the last is enforced; a variable that every comparison bounds is kept within the union of those bounds. Longer ORs are compiled through guard variables |
| all-different, up to 16 variables of up to 32 bits | a fixed variable's value is removed from the others; fails when the variables have fewer values between them than there are variables, and narrows around sets of variables that use up a range (Hall intervals) |

## Aggregates

These come only from the Python and C APIs.

| Relation | Propagation |
|---|---|
| `r == x0 + x1 + ...`, up to 64 terms | `r`'s range is the sum of the terms' ranges, and each term is narrowed from `r` and the rest |
| `r == $countones(a)` | `r` lies between the bits that must be set and the bits that may be set |
| `r == $clog2(a)` | `r` follows from `a`'s range and `a` from `r`'s |
