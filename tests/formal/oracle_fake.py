#!/usr/bin/env python3
"""A fake reference solver for testing dv-solve-smt2's oracle check.

    oracle_fake.py sat|unsat|unknown|die

Answers (get-info :version), and every check-sat with the given verdict, so a
test can make the oracle disagree with dv-solve on purpose. `die` exits after
the version. Everything else is read and ignored.
"""
import sys

mode = sys.argv[1]
for line in sys.stdin:
    if "(get-info :version)" in line:
        print('(:version "fake")', flush=True)
        if mode == "die":
            sys.exit(0)
    for _ in range(line.count("(check-sat")):
        print(mode, flush=True)
    if "(get-model)" in line:
        print("()", flush=True)
