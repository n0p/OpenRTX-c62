#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build tests/platform/c62_nxdn_phy_cost.c for a Cortex-M33 and run it on the
# QEMU mps2-an505, which is what the cost figures of the NXDN receiver in
# docs/RX_NXDN.md are measured with.
#
#   tests/platform/run_nxdn_phy_cost.sh <capture.wav> [output-dir]
#
# The capture is 48 kHz mono s16 discriminator audio.  Meson runs this on
# tests/unit/assets/nxdn_air_frame.wav; on a machine without arm-none-eabi-gcc
# or qemu-system-arm the script exits 77 and the test is reported as skipped.
#
# Set NXDN_INSNFRAME to the QEMU TCG plugin (work/dsp-probe/common/
# libinsnframe.so) and the run also reports the instructions executed per
# second of audio, which is the number the documentation quotes.  Without it
# the harness still decodes, and its summary still has to match.

set -uo pipefail

CAP=${1:-}
OUT=${2:-}

if [ -z "$CAP" ] || [ ! -f "$CAP" ]; then
    echo "usage: $0 <capture.wav> [output-dir]" >&2
    exit 2
fi

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
INC=$ROOT/openrtx/include
SRC=$ROOT/openrtx/src/protocols/NXDN
QEMU_DIR=$ROOT/tests/platform/qemu_mps2

for tool in arm-none-eabi-gcc arm-none-eabi-g++ qemu-system-arm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "skip: $tool not found" >&2
        exit 77
    fi
done

CAP=$(cd "$(dirname "$CAP")" && pwd)/$(basename "$CAP")
[ -n "$OUT" ] || OUT=$(mktemp -d)
mkdir -p "$OUT"

ARCH="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard ${NXDN_OPT:--O2}"
# -ffp-contract=off is what the firmware uses: the slicer decides on margins
FLAGS="$ARCH -ffp-contract=off -ffunction-sections -fdata-sections"
OBJS=()

build() {   # <source> <object>
    arm-none-eabi-gcc -c $FLAGS -DMARK_FRAMES -DWAV_PATH="\"$CAP\"" \
        -I"$INC" "$1" -o "$2" || exit 1
    OBJS+=("$2")
}

build "$QEMU_DIR/startup.c" "$OUT/startup.o"
build "$ROOT/tests/platform/c62_nxdn_phy_cost.c" "$OUT/harness.o"
for s in NxdnPhy NxdnDefs NxdnFec; do
    build "$SRC/$s.c" "$OUT/$s.o"
done

arm-none-eabi-g++ $ARCH -Wl,--gc-sections -T "$QEMU_DIR/mps2_an505.ld" \
    -specs=rdimon.specs -o "$OUT/nxdn_phy_cost.elf" "${OBJS[@]}" \
    -lrdimon -lm -lc 2>/dev/null || {
    echo "link failed" >&2
    exit 1
}

MARKER=$(arm-none-eabi-nm "$OUT/nxdn_phy_cost.elf" |
         awk '$3=="frame_marker"{print $1}')
PLUGIN=${NXDN_INSNFRAME:-}
QEMU_ARGS=(-M mps2-an505 -nographic -semihosting-config enable=on,target=native
           -kernel "$OUT/nxdn_phy_cost.elf")

if [ -n "$PLUGIN" ]; then
    if [ ! -f "$PLUGIN" ]; then
        echo "skip: $PLUGIN not found" >&2
        exit 77
    fi
    INSNHIST_MARKER=$MARKER INSNHIST_OUT=$OUT/insnhist \
        qemu-system-arm "${QEMU_ARGS[@]}" -plugin "file=$PLUGIN" -d plugin \
        > "$OUT/run.log" 2>&1
else
    qemu-system-arm "${QEMU_ARGS[@]}" > "$OUT/run.log" 2>&1
fi

RC=$?
grep "phy cost" "$OUT/run.log" || tail -5 "$OUT/run.log"

if [ -n "$PLUGIN" ] && [ -f "$OUT/insnhist" ]; then
    python3 - "$OUT/insnhist" <<'PY'
import statistics, sys

lines = open(sys.argv[1]).read().split("\n")
ninsn, nframes, total = map(int, lines[0].split())
totals = [int(l.split()[0]) for l in lines[1 + ninsn:] if l.strip()]
d = [totals[i] - totals[i - 1] for i in range(1, len(totals))]

if d:
    print("instructions: %d over %d s, median %.1f M/s, min %.1f, max %.1f"
          % (total, len(d), statistics.median(d) / 1e6, min(d) / 1e6,
             max(d) / 1e6))
PY
fi

exit $RC
