#!/usr/bin/env bash
# Nightly soundness run (.forgejo/workflows/nightly.yml); budget: 2 hours.
#
#   1. Everything the per-push job does (tests/ci/run_tests.sh).
#   2. The soundness campaign with fresh seeds for the rest of the budget, in
#      parallel: half on the normal build (20% wide problems), a quarter on the
#      step-checker build (every learnt clause and explanation checked), a
#      quarter wide-only (z3 as oracle). On Sundays the mutation score runs
#      instead (docs/soundness_coverage_plan.md §5.3).
#   3. Each campaign process writes its stimulus coverage as a UCIS database;
#      covsight merges them into one database, a report and a gap list.
#
# Output (uploaded as the job's artifact): $OUT/coverage.cdb, coverage.txt,
# gaps.json, campaign logs, and regressions/ -- every failure, shrunk, as a
# JSON problem that tests/formal/soundness/test_regressions.py can replay once
# copied into tests/formal/soundness/regressions/. Any failure fails the job.
#
#   BUDGET_MIN=120 JOBS=12 OUT=nightly-out tests/ci/nightly.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
BUDGET_MIN=${BUDGET_MIN:-120}
JOBS=${JOBS:-12}
OUT=${OUT:-nightly-out}
REPORT_MIN=10                       # kept back for merging and reporting
start=$(date +%s)
mkdir -p "$OUT/regressions"

tests/ci/run_tests.sh

pip_install() {
  python3 -m pip install -q "$@" 2>/dev/null || python3 -m pip install -q --break-system-packages "$@"
}
pip_install "git+https://github.com/covsight/covsight-core.git" \
            "git+https://github.com/covsight/covsight.git"

elapsed_min=$(( ($(date +%s) - start) / 60 ))
minutes=$(( BUDGET_MIN - elapsed_min - REPORT_MIN ))
if [ "$minutes" -lt 5 ]; then
  echo "::error::the build and suites used the whole budget (${elapsed_min} min)"
  exit 1
fi
export PYTHONPATH=src
seed_base=$(date +%Y%m%d)

if [ "$(date +%u)" = 7 ]; then
  echo "::group::mutation score (Sunday)"
  # The work tree must be outside the repository (it copies the repository).
  python3 -m tests.formal.soundness.mutation --work "${RUNNER_TEMP:-/tmp}/dvs-mutation" | tee "$OUT/mutation.txt"
  echo "::endgroup::"
else
  echo "::group::campaign: $JOBS processes x ${minutes} min"
  : > "$OUT/stepcheck.log"
  pids=()
  for i in $(seq 0 $((JOBS - 1))); do
    seed=$((seed_base * 100 + i))
    common=(--seed "$seed" --minutes "$minutes" --out "$OUT/regressions"
            --ucis "$OUT/cov_$i.cdb")
    case $((i % 4)) in
      0|1) python3 -m tests.formal.soundness.campaign "${common[@]}" --wide 0.2 \
             > "$OUT/campaign_$i.log" 2>&1 & ;;
      2)   DVS_SOLVER_PATH=build-check DV_STEP_CHECK_CONTINUE=1 \
           DV_STEP_CHECK_LOG="$PWD/$OUT/stepcheck.log" \
           python3 -m tests.formal.soundness.campaign "${common[@]}" \
             --exe build-check/dv-solve-smt2 --builder-limit-ms 120000 \
             > "$OUT/campaign_$i.log" 2>&1 & ;;
      3)   python3 -m tests.formal.soundness.campaign "${common[@]}" --wide 1.0 \
             > "$OUT/campaign_$i.log" 2>&1 & ;;
    esac
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || true; done
  grep -h "^STATS" "$OUT"/campaign_*.log || true
  echo "::endgroup::"

  echo "::group::coverage"
  covsight merge -o "$OUT/coverage.cdb" "$OUT"/cov_*.cdb
  covsight report "$OUT/coverage.cdb" | tee "$OUT/coverage.txt"
  covsight show gaps "$OUT/coverage.cdb" > "$OUT/gaps.json"
  echo "::endgroup::"
fi

fails=$(find "$OUT/regressions" -name '*.json' | wc -l)
if [ "$fails" -gt 0 ] || [ -s "$OUT/stepcheck.log" ]; then
  grep -h "^FAIL" "$OUT"/campaign_*.log || true
  head -20 "$OUT/stepcheck.log" 2>/dev/null || true
  echo "::error::$fails shrunk failure(s) in $OUT/regressions; step-checker log: $OUT/stepcheck.log"
  exit 1
fi
echo "nightly: no failures in $(( ($(date +%s) - start) / 60 )) min"
