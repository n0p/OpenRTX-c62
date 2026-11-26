#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build tests/platform/c62_tx_4fsk_cost.c for a Cortex-M33 and run it on the
# QEMU mps2-an505, which is what the transmit cost figures of docs/TX_NXDN.md
# and docs/TX_DPMR.md are measured with.
#
#   tests/platform/run_tx_4fsk_cost.sh [nxdn|dpmr] [output-dir]
#
# Set TX4FSK_INSNFRAME to the QEMU TCG plugin (work/dsp-probe/common/
# libinsnframe.so) and the run also reports the instructions executed per
# transmitted frame, which is the number the documentation quotes.  Without it
# the harness still runs and its summary still has to match.

set -uo pipefail

MODE=${1:-nxdn}
OUT=${2:-}

case "$MODE" in
    nxdn) DEFS="" ;;
    dpmr) DEFS="-DTX4FSK_DPMR" ;;
    *) echo "usage: $0 [nxdn|dpmr] [output-dir]" >&2; exit 2 ;;
esac

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
INC=$ROOT/openrtx/include
QEMU_DIR=$ROOT/tests/platform/qemu_mps2

for tool in arm-none-eabi-gcc arm-none-eabi-g++ qemu-system-arm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "skip: $tool not found" >&2
        exit 77
    fi
done

[ -n "$OUT" ] || OUT=$(mktemp -d)
mkdir -p "$OUT"

ARCH="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -O2"
FLAGS="$ARCH -ffp-contract=off -ffunction-sections -fdata-sections"
OBJS=()

build() {   # <source> <object>
    arm-none-eabi-gcc -c $FLAGS $DEFS -DMARK_FRAMES -I"$INC" "$1" -o "$2" || exit 1
    OBJS+=("$2")
}

build "$QEMU_DIR/startup.c" "$OUT/startup.o"
build "$ROOT/tests/platform/c62_tx_4fsk_cost.c" "$OUT/harness.o"
build "$ROOT/openrtx/src/protocols/Fsk4Tx.c" "$OUT/Fsk4Tx.o"
build "$ROOT/openrtx/src/protocols/NXDN/NxdnTx.c" "$OUT/NxdnTx.o"
build "$ROOT/openrtx/src/protocols/NXDN/NxdnFec.c" "$OUT/NxdnFec.o"
build "$ROOT/openrtx/src/protocols/NXDN/NxdnDefs.c" "$OUT/NxdnDefs.o"
build "$ROOT/openrtx/src/protocols/DPMR/DpmrTx.c" "$OUT/DpmrTx.o"
build "$ROOT/openrtx/src/protocols/DPMR/DpmrDefs.c" "$OUT/DpmrDefs.o"

arm-none-eabi-g++ $ARCH -Wl,--gc-sections -T "$QEMU_DIR/mps2_an505.ld" \
    -specs=rdimon.specs -o "$OUT/tx_4fsk_cost.elf" "${OBJS[@]}" \
    -lrdimon -lm -lc 2>/dev/null || {
    echo "link failed" >&2
    exit 1
}

MARKER=$(arm-none-eabi-nm "$OUT/tx_4fsk_cost.elf" |
         awk '$3=="frame_marker"{print $1}')
PLUGIN=${TX4FSK_INSNFRAME:-}
QEMU_ARGS=(-M mps2-an505 -nographic -semihosting-config enable=on,target=native
           -kernel "$OUT/tx_4fsk_cost.elf")

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
grep "tx cost" "$OUT/run.log" || tail -5 "$OUT/run.log"

if [ -n "$PLUGIN" ] && [ -f "$OUT/insnhist" ]; then
    python3 - "$OUT/insnhist" <<'PY'
import statistics, sys

lines = open(sys.argv[1]).read().split("\n")
ninsn, nframes, total = map(int, lines[0].split())
totals = [int(l.split()[0]) for l in lines[1 + ninsn:] if l.strip()]
d = [totals[i] - totals[i - 1] for i in range(1, len(totals))]

if d:
    # the first interval is the preamble (and the header for dPMR)
    print("frames %d, per frame: median %.2f k, min %.2f k, max %.2f k"
          % (len(d), statistics.median(d[1:]) / 1e3, min(d[1:]) / 1e3,
             max(d[1:]) / 1e3))
    print("instructions per frame: %s" % ", ".join(str(x) for x in d[:4]))
PY
fi

exit $RC
