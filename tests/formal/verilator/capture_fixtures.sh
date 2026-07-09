#!/usr/bin/env bash
# Capture the SMT transcript a Verilator randomize test emits, and sanitize it
# into a standalone cross-check fixture.
#
# Usage: capture_fixtures.sh <test.v> [<fixture_name>]
#   Compiles <test.v> with the covergroup-rt Verilator (--binary), runs it under
#   z3 with transcript capture, and sanitizes the transcript into
#   $OUT_DIR/<fixture_name>.smt2 (default OUT_DIR = tests/formal/smt2/verilator).
#
# The transcript is whatever Verilator SENDS the solver, so it is independent of
# which solver actually runs it; z3 is used because it always answers. The
# sanitizer keeps the first self-contained (check-sat) constraint query.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
VERILATOR="${VERILATOR:-$HOME/projects/verilator/verilator-covergroup-rt/bin/verilator}"
Z3="${Z3:-$REPO_ROOT/packages/python/bin/z3}"
SANITIZE="$REPO_ROOT/tests/formal/verilator/sanitize_transcript.py"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/tests/formal/smt2/verilator}"

TEST_V="${1:?usage: capture_fixtures.sh <test.v> [name]}"
NAME="${2:-$(basename "$TEST_V" .v)}"

if [ ! -x "$VERILATOR" ]; then echo "SKIP $NAME: no verilator at $VERILATOR" >&2; exit 3; fi
if [ ! -f "$TEST_V" ]; then echo "SKIP $NAME: no test file $TEST_V" >&2; exit 3; fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- 1. Compile (solver-agnostic) -------------------------------------------
if ! "$VERILATOR" --binary --timing -Wno-fatal -Wno-lint \
      --Mdir "$WORK" -o sim "$TEST_V" > "$WORK/verilate.log" 2>&1; then
  echo "SKIP $NAME: verilation/build failed" >&2
  tail -5 "$WORK/verilate.log" >&2
  exit 4
fi
[ -x "$WORK/sim" ] || { echo "SKIP $NAME: no sim binary" >&2; exit 4; }

# --- 2. Run under z3 with transcript capture --------------------------------
# Verilator pipes SMT to the solver's stdin, so z3 must be in interactive mode
# (--in). The transcript captured is what Verilator SENDS, independent of z3's
# answers.
TRANS="$WORK/transcript.smt2"
VERILATOR_SOLVER="$Z3 --in" "$WORK/sim" +verilator+solver+file+"$TRANS" \
  > "$WORK/run.log" 2>&1
if [ ! -s "$TRANS" ]; then
  echo "SKIP $NAME: empty transcript (test may not invoke the SMT solver)" >&2
  exit 5
fi

# --- 3. Sanitize into a fixture ---------------------------------------------
mkdir -p "$OUT_DIR"
if python3 "$SANITIZE" "$TRANS" "$OUT_DIR/$NAME.smt2" --name "$NAME"; then
  echo "OK   $NAME -> $OUT_DIR/$NAME.smt2"
else
  echo "SKIP $NAME: no self-contained query in transcript" >&2
  exit 6
fi
