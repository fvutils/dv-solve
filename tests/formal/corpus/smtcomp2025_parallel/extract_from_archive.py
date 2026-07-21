#!/usr/bin/env python3
"""Extract the 93 bit-vector-family benchmarks from the SMT-COMP 2025 parallel
archive (parallel.tar.gz, Zenodo record 16887742) into smt2/<logic>/<family>/<name>.

The archive stores formulas under scrambled names (scrambledNNN.smt2) with a
sibling <id>_<LOGIC>_...yml that records the *original* path via its
``original_files`` comment. We de-scramble by copying each scrambled formula to
its original family/name so it lines up with meta/manifest.json (which keys on
the competition's original benchmark names).

Usage:
    python extract_from_archive.py /tmp/parallel.tar.gz
"""
from __future__ import annotations

import json
import re
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOGICS = ("QF_BV", "QF_ABV", "QF_UFBV", "QF_AUFBV")
_ORIG = re.compile(r"original_files:\s*'([^']+)'")
_INPUT = re.compile(r"input_files:\s*'([^']+)'")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    archive = Path(sys.argv[1])
    manifest = json.load(open(HERE / "meta" / "manifest.json"))
    want = {(e["logic"], e["name"]) for e in manifest}
    fam = {(e["logic"], e["name"]): e["family"] for e in manifest}

    out = HERE / "smt2"
    with tarfile.open(archive, "r:gz") as tf:
        members = tf.getnames()
        # index scrambled formula members by basename per logic dir
        formula = {}  # (logic, scrambled_basename) -> member
        ymls = []  # (logic, member)
        for m in members:
            parts = m.split("/")
            if len(parts) < 3 or parts[-2] not in LOGICS:
                continue
            logic, base = parts[-2], parts[-1]
            if base.endswith(".smt2"):
                formula[(logic, base)] = m
            elif base.endswith(".yml"):
                ymls.append((logic, m))

        extracted = 0
        missing = []
        for logic, ymember in ymls:
            txt = tf.extractfile(ymember).read().decode("utf-8", "replace")
            mo, mi = _ORIG.search(txt), _INPUT.search(txt)
            if not (mo and mi):
                continue
            orig = mo.group(1)          # non-incremental/QF_ABV/<family>/<name>
            scrambled = mi.group(1)     # scrambledNNN.smt2
            name = orig.split("/")[-1]
            if (logic, name) not in want:
                continue
            src = formula.get((logic, scrambled))
            if src is None:
                missing.append((logic, name, "no scrambled formula"))
                continue
            dest = out / logic / fam[(logic, name)] / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            data = tf.extractfile(src).read()
            dest.write_bytes(data)
            extracted += 1

    have = {(e["logic"], e["name"]) for e in manifest
            if (out / e["logic"] / e["family"] / e["name"]).is_file()}
    missing_all = want - have
    print(f"extracted {extracted} formulas; manifest wants {len(want)}; "
          f"present {len(have)}; missing {len(missing_all)}")
    for logic in LOGICS:
        n = sum(1 for (l, _) in have if l == logic)
        print(f"  {logic}: {n}")
    for m in sorted(missing_all):
        print("  MISSING:", m)
    return 0 if not missing_all else 1


if __name__ == "__main__":
    raise SystemExit(main())
