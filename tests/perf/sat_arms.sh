#!/bin/bash
# Run one DIMACS file through the SAT arms of docs/fw_hdl_xor_hypothesis.md
# (built under perf-builds/sat). usage: tests/perf/sat_arms.sh file.cnf
# Prints one line per arm: name result seconds. T sets the budget (60 s).
S=perf-builds/sat; T=${T:-60}; f=$1
run() { local n=$1; shift; local t0=$(date +%s.%N); out=$(timeout $T "$@" "$f" 2>/dev/null | grep -m1 '^s '); local t1=$(date +%s.%N); r=${out#s }; [ -z "$r" ] && r=TIMEOUT; printf "%s %s %.2f\n" $n "$r" $(echo "$t1-$t0"|bc); }
run lgl        $S/lingeling/lingeling
run lgl-nogauss $S/lingeling/lingeling --gauss=0
run cms        $S/cryptominisat/build/cryptominisat5 --verb 0
run cms-nogauss $S/cryptominisat/build/cryptominisat5 --verb 0 --maxnummatrices 0
run kissat     $S/kissat/build/kissat -q
run cadical    $S/cadical/build/cadical -q
