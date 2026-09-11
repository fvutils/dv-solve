#!/usr/bin/env bash
# Fetch the SMT-COMP 2025 parallel-track archive and extract the 93 bit-vector
# benchmarks into smt2/. The archive itself (279 MB) and the extracted corpus
# (234 MB) are gitignored -- run this to materialize them locally.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARCHIVE="${1:-/tmp/parallel.tar.gz}"
URL="https://zenodo.org/records/16887742/files/parallel.tar.gz?download=1"

if [[ ! -f "$ARCHIVE" ]]; then
  echo "downloading parallel.tar.gz (279 MB) -> $ARCHIVE"
  curl -L -o "$ARCHIVE" "$URL"
fi
echo "extracting bit-vector family into smt2/ ..."
python3 "$HERE/extract_from_archive.py" "$ARCHIVE"
