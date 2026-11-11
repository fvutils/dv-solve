#!/bin/sh
# Builds writer.cpp and runtime.cpp against an installed dv-solve using only
# the flags its pkg-config files give, then runs them.
#   tests/pkgconfig/run.sh <install prefix> [work dir]
set -e
here=$(cd "$(dirname "$0")" && pwd)
export PKG_CONFIG_PATH="$1/lib/pkgconfig"
work=${2:-$(mktemp -d)}
mkdir -p "$work"
CXX=${CXX:-c++}
$CXX -o "$work/writer" "$here/writer.cpp" $(pkg-config --cflags --libs dv-solve-writer)
$CXX -o "$work/runtime" "$here/runtime.cpp" $(pkg-config --static --cflags --libs dv-solve)
"$work/writer" "$work/problem.bin"
"$work/runtime" "$work/problem.bin"
