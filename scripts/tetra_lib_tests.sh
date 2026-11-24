#!/bin/bash
# Build libtetra's unit tests against the amalgamated Catch2, without CMake's
# Catch2 package requirement, and run them.
#   tests.sh <libtetra-src-dir> <build-dir>
set -euo pipefail
SRC=${1:?src}
BLD=${2:?build}
C2=/home/n0p/src/c62/work/tetra-dmo/third_party
mkdir -p "$BLD/tests"
g++ -std=c++14 -O1 -c "$C2/catch_amalgamated.cpp" -I "$C2" -o "$BLD/tests/catch.o"
for t in coding dmo sds speech tmo tmo_l3 tmo_ul; do
  f="$SRC/tests/unit/test_$t.cpp"
  [ -f "$f" ] || continue
  g++ -std=c++14 -O1 -I "$SRC/src" -I "$SRC/include" -I "$BLD/generated" -I "$C2" \
      -DTETRA_TEST_VECTOR_DIR="\"$SRC/tests/vectors\"" \
      -c "$f" -o "$BLD/tests/test_$t.o"
  g++ -o "$BLD/tests/test_$t" "$BLD/tests/test_$t.o" "$BLD/tests/catch.o" \
      "$BLD/libdmo_scenario.a" "$BLD/libtetra.a" -lm
  printf '%-10s ' "$t"
  "$BLD/tests/test_$t" | tail -2 | tr '\n' ' '
  echo
done
