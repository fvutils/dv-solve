#!/usr/bin/env bash
# Run Verilator's own constraint/randomization tests through both z3 and
# dv-solve (--mode=verilator), comparing pass/fail. Compiles each test once
# (solver-agnostic) then runs it under each solver.
#
# Usage: run_suite.sh [test_basename ...]        # defaults to $DEFAULT_SET
#        TESTS_FILE=list.txt run_suite.sh         # one basename per line
#
# Output: one line per test:  <RESULT>  <test>   [notes]
#   BOTH_PASS   - z3 pass, dv-solve pass  (covered)
#   DV_FAIL     - z3 pass, dv-solve fail  (GAP: dv-solve can't handle it)
#   DV_HANG     - z3 pass, dv-solve timed out
#   BOTH_FAIL   - both fail (test/env issue, not a dv-solve gap)
#   Z3_FAIL     - z3 fails too (env/flags issue; inconclusive)
#   COMPILE_FAIL- verilation/build failed
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
VT=~/projects/verilator/verilator-covergroup-rt/test_regress/t
VERILATOR="${VERILATOR:-$HOME/projects/verilator/verilator-covergroup-rt/bin/verilator}"
Z3="${Z3:-$REPO_ROOT/packages/python/bin/z3} --in"
DV="${DV:-$REPO_ROOT/build/dv-solve-smt2} --interactive --mode=verilator"
WORKROOT="${WORKROOT:-$REPO_ROOT/tests/formal/verilator/suite_obj}"
RUN_TIMEOUT="${RUN_TIMEOUT:-25}"
COMPILE_TIMEOUT="${COMPILE_TIMEOUT:-180}"

run_one() {
  local t="$1"
  local sv="$VT/$t.v"
  [ -f "$sv" ] || sv="$VT/$t.sv"
  if [ ! -f "$sv" ]; then echo "MISSING      $t"; return; fi
  local work="$WORKROOT/$t"
  rm -rf "$work"; mkdir -p "$work"
  # Compile once.
  if ! timeout "$COMPILE_TIMEOUT" "$VERILATOR" --binary --timing -Wno-fatal \
        --Mdir "$work" -o sim "$sv" > "$work/verilate.log" 2>&1; then
    echo "COMPILE_FAIL $t"; return
  fi
  [ -x "$work/sim" ] || { echo "COMPILE_FAIL $t   (no exe)"; return; }
  # Run under a solver, echo pass/hang/fail based on "All Finished".
  _run() {
    local solver="$1" tag="$2"
    rm -f /tmp/dv-solve.log
    timeout "$RUN_TIMEOUT" env VERILATOR_SOLVER="$solver" "$work/sim" \
        > "$work/run_$tag.log" 2>&1
    local rc=$?
    if grep -q "All Finished" "$work/run_$tag.log"; then echo pass
    elif [ $rc -eq 124 ]; then echo hang
    else echo fail; fi
  }
  local z=$(_run "$Z3" z3)
  local d=$(_run "$DV" dv)
  local res
  if   [ "$z" = pass ] && [ "$d" = pass ]; then res=BOTH_PASS
  elif [ "$z" = pass ] && [ "$d" = hang ]; then res=DV_HANG
  elif [ "$z" = pass ] && [ "$d" = fail ]; then res=DV_FAIL
  elif [ "$z" = fail ] && [ "$d" = fail ]; then res=BOTH_FAIL
  elif [ "$z" = hang ]; then res=Z3_HANG
  else res=Z3_FAIL; fi
  printf "%-12s %s\n" "$res" "$t"
}

DEFAULT_SET="t_constraint t_constraint_cond t_constraint_operators t_constraint_dist \
t_constraint_dist_range t_constraint_dist_weight t_constraint_foreach \
t_constraint_solve_before t_randomize_unique_elem t_randomize_soft \
t_randomize_soft_relaxation t_randc t_randc_constraint t_randc_enum_constraint \
t_std_randomize t_std_randomize_with t_constraint_inside_typedef_array \
t_constraint_implication_set t_constraint_mode t_constraint_inheritance \
t_constraint_array_index t_constraint_struct t_constraint_dyn_queue_basic \
t_constraint_func_call t_randomize_method t_randomize_prepost t_randomize_this \
t_constraint_redops t_constraint_countones t_randc_wide_constraint"

if [ -n "${TESTS_FILE:-}" ]; then
  mapfile -t TESTS < "$TESTS_FILE"
elif [ $# -gt 0 ]; then
  TESTS=("$@")
else
  read -ra TESTS <<< "$DEFAULT_SET"
fi

for t in "${TESTS[@]}"; do
  [ -z "$t" ] && continue
  run_one "$t"
done
