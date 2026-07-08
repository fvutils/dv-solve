#!/usr/bin/env bash
# Phase-0 proof-of-concept: compile a Verilator randomize test once, then run it
# under z3 (baseline) and dv-solve, capturing each solver's SMT transcript and
# comparing results.
#
# Usage: tests/formal/verilator/run_phase0.sh
set -u

# --- Locations --------------------------------------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
HERE="$REPO_ROOT/tests/formal/verilator"
TEST_SV="$HERE/phase0/t_smoke_unsigned.sv"
WORK="$HERE/phase0/obj"

VERILATOR="${VERILATOR:-$HOME/projects/verilator/verilator-covergroup-rt/bin/verilator}"
Z3="${Z3:-$REPO_ROOT/packages/python/bin/z3}"
DVSOLVE="${DVSOLVE:-$REPO_ROOT/build/dv-solve-smt2}"

echo "== Phase-0 config =="
echo "  verilator : $VERILATOR"
echo "  z3        : $Z3"
echo "  dv-solve  : $DVSOLVE"
echo "  test      : $TEST_SV"
echo

for tool in "$VERILATOR" "$Z3" "$DVSOLVE"; do
  if [ ! -x "$tool" ]; then echo "ERROR: not executable: $tool" >&2; exit 2; fi
done

# --- 1. Compile once (solver-agnostic; VERILATOR_SOLVER is read at run time) --
rm -rf "$WORK"
mkdir -p "$WORK"
echo "== Compiling with Verilator (--binary) =="
"$VERILATOR" --binary --timing -Wno-fatal \
  --Mdir "$WORK" -o t_smoke_unsigned \
  "$TEST_SV" > "$WORK/verilate.log" 2>&1
if [ $? -ne 0 ]; then
  echo "ERROR: verilation/build failed; see $WORK/verilate.log" >&2
  tail -30 "$WORK/verilate.log" >&2
  exit 1
fi
EXE="$WORK/t_smoke_unsigned"
if [ ! -x "$EXE" ]; then echo "ERROR: no executable produced: $EXE" >&2; exit 1; fi
echo "  built: $EXE"
echo

# --- 2. Run under each solver -----------------------------------------------
run_under() {
  local name="$1" solver_cmd="$2"
  local trans="$WORK/transcript_$name.smt2"
  local out="$WORK/run_$name.log"
  echo "== Run under $name =="
  echo "  VERILATOR_SOLVER=\"$solver_cmd\""
  VERILATOR_SOLVER="$solver_cmd" "$EXE" \
    +verilator+solver+file+"$trans" > "$out" 2>&1
  local rc=$?
  if grep -q "All Finished" "$out"; then
    echo "  RESULT: PASS (rc=$rc)"
  else
    echo "  RESULT: FAIL (rc=$rc)"
    echo "  ---- last 20 lines of $out ----"
    tail -20 "$out" | sed 's/^/    /'
  fi
  echo "  transcript: $trans"
  echo "  stdout log: $out"
  echo
  return $rc
}

run_under "z3"      "$Z3 --in"
Z3_RC=$?
run_under "dvsolve" "$DVSOLVE --interactive"
DV_RC=$?

# --- 3. Compare the emitted SMT (Verilator's output should be identical) ------
echo "== Compare emitted SMT (Verilator -> solver requests) =="
TZ3="$WORK/transcript_z3.smt2"
TDV="$WORK/transcript_dvsolve.smt2"
if [ -f "$TZ3" ] && [ -f "$TDV" ]; then
  if diff -q "$TZ3" "$TDV" >/dev/null 2>&1; then
    echo "  emitted SMT is IDENTICAL under both solvers (as expected)"
  else
    echo "  emitted SMT DIFFERS (diversity is data-dependent; showing first 20 diff lines):"
    diff "$TZ3" "$TDV" | head -20 | sed 's/^/    /'
  fi
else
  echo "  (transcript(s) missing; check +verilator+solver+file+ support)"
fi
echo

echo "== Summary =="
echo "  z3      : $([ $Z3_RC -eq 0 ] && echo PASS || echo FAIL) (rc=$Z3_RC)"
echo "  dvsolve : $([ $DV_RC -eq 0 ] && echo PASS || echo FAIL) (rc=$DV_RC)"
[ $Z3_RC -eq 0 ] && [ $DV_RC -eq 0 ]
exit $?
