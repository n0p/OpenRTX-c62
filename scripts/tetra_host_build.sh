#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The TETRA receive chain on the host, with the TMO uplink build the firmware
# uses: libtetra configured with TETRA_UPLINK_DETECT and TETRA_UPLINK_DECODE
# pinned ON, and then the firmware's own translation units (TetraStatus.c,
# TetraUplink.c) exercised without a radio.
#
#   scripts/tetra_host_build.sh <libtetra-src-dir> <build-dir> [capture]
#
# The generated tetra/tetra_config.h is what both the library and every
# consumer compile against, so the script fails if the two options did not
# make it into it: a host harness built against a different layout than the
# firmware is worse than no harness.
#
# docs/TETRA_LIB.md, "Escuchar el uplink de la celda", has what each check
# proves and what is left for a radio and a capture.
set -euo pipefail

SRC=${1:?libtetra source directory}
BLD=${2:?build directory}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}

echo "libtetra: $SRC"
echo "build:    $BLD"

cmake -G Ninja -S "$SRC" -B "$BLD" \
      -DTETRA_BUILD_TESTS=OFF -DTETRA_BUILD_TOOLS=OFF \
      -DTETRA_UPLINK_DETECT=ON -DTETRA_UPLINK_DECODE=ON > "$BLD.configure.log"
ninja -C "$BLD" > "$BLD.build.log"

CFG="$BLD/generated/tetra/tetra_config.h"
grep -q '^#define TETRA_UPLINK_DETECT 1$' "$CFG"
grep -q '^#define TETRA_UPLINK_DECODE 1$' "$CFG"
echo "tetra_config.h: TETRA_UPLINK_DETECT 1, TETRA_UPLINK_DECODE 1"

INC="-I $ROOT/openrtx/include -I $SRC/include -I $BLD/generated"
CFLAGS="-O2 -std=gnu17 $INC"

# The firmware's own units, compiled as C (they are C), then linked by the
# C++ harnesses that include their headers inside extern "C".
mkdir -p "$BLD/obj"
for f in TetraStatus TetraUplink TetraDemod SdsMsg; do
    $CC $CFLAGS -c "$ROOT/openrtx/src/protocols/TETRA/$f.c" \
        -o "$BLD/obj/$f.o"
done

# The uplink harness: the duplex table against libtetra's, the cell event to
# the status, and a real MS control uplink burst (U-SETUP) through the
# receiver.
$CXX -std=c++14 -O2 $INC \
     "$ROOT/tests/platform/c62_tetra_uplink_host.cpp" \
     "$BLD/obj/TetraStatus.o" "$BLD/obj/TetraUplink.o" \
     "$BLD/obj/TetraDemod.o" "$BLD/obj/SdsMsg.o" \
     "$BLD/libtetra.a" -lpthread -lm -o "$BLD/c62_tetra_uplink_host"
"$BLD/c62_tetra_uplink_host"

# The status harness of docs/TETRA_LIB.md section 5.1, so a capture can be run
# against the same tree.
$CC -O2 $INC \
    "$ROOT/tests/platform/c62_tetra_status_host.c" \
    "$BLD/obj/TetraStatus.o" "$BLD/obj/TetraDemod.o" "$BLD/obj/SdsMsg.o" \
    "$BLD/libtetra.a" -lpthread -lm -o "$BLD/c62_tetra_status_host"
echo "harnesses: $BLD/c62_tetra_uplink_host, $BLD/c62_tetra_status_host"

if [ "${3:-}" != "" ]; then
    echo "capture: $3"
    "$BLD/c62_tetra_status_host" "$3"
fi
