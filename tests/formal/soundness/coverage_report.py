"""Implementation coverage of the soundness-critical C files, from an lcov trace.

Build an instrumented copy, run the campaign and the propagator harness
against it, capture, report:

    cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Release \\
        -DCMAKE_C_FLAGS_RELEASE="-O0 -g --coverage" \\
        -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage
    cmake --build build-cov --target dv-solve-smt2 dv_solve test_prop_exhaustive
    (cd build-cov && ./test_prop_exhaustive)
    DVS_SOLVER_PATH=build-cov PYTHONPATH=src python -m tests.formal.soundness.campaign \\
        --seed 31 --n 3000 --exe build-cov/dv-solve-smt2
    lcov --capture --directory build-cov --output-file cov.info --rc branch_coverage=1
    python -m tests.formal.soundness.coverage_report cov.info

An uncovered explainer, propagator or analysis branch is a hole in the
coverage model of docs/soundness_coverage_plan.md §2.2: some stimulus bin
is missing.
"""
from __future__ import annotations

import os
import sys

FILES = ["dvs_lcg.c", "dvs_explain.c", "dvs_prop_templates.c", "dvs_compile.c",
         "dvs_search.c", "dvs_propagate.c", "smt2_frontend.c", "dvs_validate.c"]


def parse(path: str) -> dict:
    data, d = {}, None
    for line in open(path):
        line = line.strip()
        if line.startswith("SF:"):
            d = data.setdefault(os.path.basename(line[3:]),
                                {"LF": 0, "LH": 0, "FNF": 0, "FNH": 0, "BRF": 0, "BRH": 0,
                                 "fns": {}})
        elif d is not None and line.split(":")[0] in ("LF", "LH", "FNF", "FNH", "BRF", "BRH"):
            k, v = line.split(":")
            d[k] += int(v)
        elif d is not None and line.startswith("FNDA:"):
            cnt, name = line[5:].split(",", 1)
            d["fns"][name] = d["fns"].get(name, 0) + int(cnt)
    return data


def main(argv=None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    data = parse(argv[0])

    def pc(a, b):
        return f"{100 * a / b:5.1f}% ({a}/{b})" if b else "-"
    print(f"{'file':24s} {'lines':>18s} {'branches':>18s} {'functions':>16s}")
    for f in FILES:
        d = data.get(f)
        if d:
            print(f"{f:24s} {pc(d['LH'], d['LF']):>18s} {pc(d['BRH'], d['BRF']):>18s}"
                  f" {pc(d['FNH'], d['FNF']):>16s}")
    for f in ("dvs_explain.c", "dvs_prop_templates.c"):
        d = data.get(f, {"fns": {}})
        never = sorted(n for n, c in d["fns"].items()
                       if c == 0 and ("explain" in n or "_fire_" in n))
        print(f"\n{f}: propagators/explainers never run: {', '.join(never) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
