#!/usr/bin/env bash
# Build dv-solve and run its test suites, as CI does (.forgejo/workflows/test.yml).
#
# Runs in a bare catthehacker/ubuntu:act-22.04 container (the Forgejo runner's
# ubuntu-latest), so it fetches what `ivpm update` would otherwise provide:
# CaDiCaL's sources, and z3 as the reference solver several tests compare
# against. Tests whose tools are absent here (Verilator, bitwuzla, yosys) skip.
#
#   tests/ci/run_tests.sh            # build, ctest, unit + formal, step checker
#   JOBS=16 tests/ci/run_tests.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
JOBS=${JOBS:-8}

pip_install() {
  # --break-system-packages is a fallback: some rebuilds of the image mark the
  # interpreter externally-managed (PEP 668); the container is thrown away.
  python3 -m pip install -q "$@" 2>/dev/null || python3 -m pip install -q --break-system-packages "$@"
}

echo "::group::dependencies"
pip_install --upgrade pip
pip_install cmake ninja pytest scipy z3-solver
[ -d packages/cadical ] || git clone -q --depth 1 -b rel-3.0.0 \
    https://github.com/arminbiere/cadical.git packages/cadical
echo "::endgroup::"

echo "::group::build"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$JOBS"
echo "::endgroup::"

echo "::group::ctest"
(cd build && ctest -j"$JOBS" --output-on-failure)
echo "::endgroup::"

echo "::group::unit and formal suites"
# test_e2e / test_partitioner need the zuspec packages, which CI does not fetch.
python3 -m pytest -q -p no:cacheprovider tests/unit tests/formal \
    --ignore=tests/unit/test_e2e.py --ignore=tests/unit/test_partitioner.py
echo "::endgroup::"

echo "::group::step checker"
# Every learnt clause and explanation on the clause-learning stress problems
# is checked (docs/soundness_coverage_plan.md, P1).
cmake -S . -B build-check -G Ninja -DCMAKE_BUILD_TYPE=Release -DDVS_STEP_CHECK=ON
cmake --build build-check -j"$JOBS" --target dv-solve-smt2
python3 -m pytest -q -p no:cacheprovider tests/formal/soundness/test_lcg_stress.py
echo "::endgroup::"
