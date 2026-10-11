#!/usr/bin/env bash
# Capture the first constraint query Verilator sends for a corpus harness.
#
# Usage: capture.sh NAME
#   Compiles captures/NAME/harness.sv with Verilator (--binary), runs it with
#   z3 as the solver and the transcript logged, and keeps the first query
#   (produce-models .. check-sat) as captures/NAME/base.smt2. The raw
#   transcript and build go to perf-out/svcorpus/capture/NAME/.
#
# Verilator is only the capture tool: the corpus test is the builder module
# cases/NAME.py, checked against base.smt2 by equiv.py.
set -eu
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
NAME="${1:?usage: capture.sh NAME}"
VERILATOR="${VERILATOR:-$REPO/packages/verilator/bin/verilator}"
Z3="${Z3:-$REPO/packages/python/bin/z3}"
SRC="$REPO/tests/perf/svcorpus/captures/$NAME"
WORK="$REPO/perf-out/svcorpus/capture/$NAME"
rm -rf "$WORK"; mkdir -p "$WORK"
"$VERILATOR" --binary --timing -Wno-fatal -Wno-lint --Mdir "$WORK/obj" -o sim \
    "$SRC/harness.sv" > "$WORK/verilate.log" 2>&1 || {
    grep -E "%Error" "$WORK/verilate.log" | head -20 >&2; exit 2; }
VERILATOR_SOLVER="$Z3 --in" "$WORK/obj/sim" +verilator+solver+file+"$WORK/transcript.smt2" \
    > "$WORK/run.log" 2>&1 || { tail -20 "$WORK/run.log" >&2; exit 3; }
python3 - "$WORK/transcript.smt2" "$SRC/base.smt2" <<'EOF'
import sys
src = open(sys.argv[1]).read()
i = src.find("(set-option :produce-models")
if i < 0:
    sys.exit("no query in the transcript (does the harness call randomize()?)")
i = src.rfind("\n", 0, i) + 1
j = src.find("(check-sat)", i) + len("(check-sat)")
open(sys.argv[2], "w").write(
    "; First query of a Verilator randomize() capture of harness.sv\n" + src[i:j] + "\n")
EOF
echo "captured $SRC/base.smt2 ($(wc -l < "$SRC/base.smt2") lines); run log: $WORK/run.log"
