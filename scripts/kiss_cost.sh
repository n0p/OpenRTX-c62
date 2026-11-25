#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run the packet modem cost harness for the Cortex-M33 in QEMU, with
# the per-stage instruction counter of work/dsp-probe (the same probe the
# TETRA, DMR and APRS cost figures use).
#
#   scripts/kiss_cost.sh [O2|Os]
#
# Prints the instruction count between frame_marker() calls: per stage of each
# modem (arm, modulate, demodulate, KISS) and per 10 ms block of audio. The
# profile file has the raw events; the summary is what docs/KISS.md quotes.
#
# Three details that are not optional, learned by watching this print marker=0
# and no numbers at all:
#
#   * the link address is 0x10000000. mps2-an505 boots from the vector table at
#     0x10000000 and ignores the ELF entry point; linked anywhere else the
#     guest starts at PC=0 and faults before main. Hence: the probe's own
#     linker script, unmodified.
#   * -fno-ipa-icf. Without it GCC folds frame_marker() (an empty function)
#     into every other empty function and the plugin snapshots 13 times at
#     the wrong places.
#   * the harness ends with exit(0). The plugin dumps its profile from an
#     atexit hook, which only runs when the guest exits; a WFI loop leaves
#     QEMU spinning until the timeout kills it.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROBE=${PROBE:-/home/n0p/src/c62/work/dsp-probe}
OUT="$ROOT/build-qemu-kiss"
V=${1:-O2}

case "$V" in
  O2) FLAGS="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -O2" ;;
  Os) FLAGS="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -Os" ;;
  *)  echo "unknown variant $V"; exit 1 ;;
esac
FLAGS="$FLAGS -fno-ipa-icf"

if [ ! -f "$PROBE/common/libinsnframe.so" ]; then
    echo "instruction probe not built: $PROBE/common/libinsnframe.so"
    exit 1
fi

mkdir -p "$OUT"

GCC=arm-none-eabi-gcc
INC="-I$ROOT/openrtx/include"
COMMON="-ffunction-sections -fdata-sections -fno-common -g -std=gnu11"

OBJS=""
for f in \
    "$ROOT/openrtx/src/protocols/KISS/kiss.c" \
    "$ROOT/openrtx/src/protocols/KISS/afsk9600.c" \
    "$ROOT/openrtx/src/protocols/KISS/packet_modem.c" \
    "$ROOT/openrtx/src/protocols/APRS/afsk.c" \
    "$ROOT/openrtx/src/protocols/APRS/ax25.c" \
    "$ROOT/openrtx/src/protocols/APRS/hdlc.c" \
    "$ROOT/tests/qemu/kiss_cost.c"
do
    b=$(basename "$f" .c)
    $GCC $FLAGS $COMMON $INC -c "$f" -o "$OUT/$b-$V.o" || exit 1
    OBJS="$OBJS $OUT/$b-$V.o"
done

$GCC $FLAGS -Wl,--gc-sections -T "$PROBE/common/mps2.ld" -specs=rdimon.specs \
     -o "$OUT/kisscost-$V.elf" "$PROBE/common/startup.o" $OBJS -lrdimon -lm -lc \
     2>&1 | grep -E "error|undefined" | head -10

MK=$(arm-none-eabi-nm "$OUT/kisscost-$V.elf" | awk '$3=="frame_marker"{print $1}')
echo "variant $V, frame_marker at 0x$MK"
INSNHIST_MARKER=$MK INSNHIST_OUT="$OUT/prof-$V.txt" \
  timeout 120 qemu-system-arm -M mps2-an505 -nographic \
  -semihosting-config enable=on,target=native \
  -kernel "$OUT/kisscost-$V.elf" \
  -plugin file=$PROBE/common/libinsnframe.so -d plugin 2>&1 |
  grep -E "INSN_COUNT|kiss_cost|error" | tee "$OUT/run-$V.txt"

echo
python3 - "$OUT/prof-$V.txt" "$V" "$OUT/run-$V.txt" <<'PYEOF'
import sys

path, variant, runlog = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(path).read().split("\n")
ninsn, nframes, total = (int(x) for x in lines[0].split()[0:3])
totals = [int(lines[1 + ninsn + k].split()[0]) for k in range(nframes)]

# Blocks and frames per modem, as the harness reported them.
blocks, frames = {}, {}
for line in open(runlog):
    if not line.startswith("kiss_cost "):
        continue
    name = line.split(":", 1)[0].split()[1]
    for part in line.split(":", 1)[1].split(","):
        key, _, val = part.strip().partition(" ")
        if key == "blocks":
            blocks[name] = int(val)
        elif key == "rx_frames":
            frames[name] = int(val)

# Snapshot order in tests/qemu/kiss_cost.c: one marker before main's first
# modem, five per modem, one after both.
names = ["start-up (init + frame build)",
         "1200 arm (init + tx_begin)",
         "1200 gen (whole burst)",
         "1200 re-arm (init + rx_begin)",
         "1200 dem (whole audio)",
         "1200 kiss (decode + encode)",
         "9600 arm (init + tx_begin)",
         "9600 gen (whole burst)",
         "9600 re-arm (init + rx_begin)",
         "9600 dem (whole audio)",
         "9600 kiss (decode + encode)",
         "exit path (after last marker)"]

print("variant %s: %d snapshots, %d instructions total" % (variant, nframes, total))
if nframes != len(names):
    print("  WARNING: expected %d snapshots, got %d -- the labels below may be "
          "off. Is -fno-ipa-icf missing?" % (len(names), nframes))

prev = 0
for i, t in enumerate(totals):
    d = t - prev
    prev = t
    label = names[i] if i < len(names) else "extra %d" % i
    extra = ""
    if " gen " in label or " dem " in label:
        who = {"1200": "AFSK1200", "9600": "AFSK9600"}.get(label.split()[0], "")
        n = blocks.get(who, 0)
        if n:
            extra = "   %8.0f per 10 ms block (%d blocks, %.0f per sample)" % (
                d / n, n, d / (n * 480.0))
    print("  %-32s %12d%s" % (label, d, extra))

for who in ("AFSK1200", "AFSK9600"):
    if who in frames:
        print("  %s demodulator locked: %d frame(s)" % (who, frames[who]))
PYEOF
echo "profile: $OUT/prof-$V.txt"
