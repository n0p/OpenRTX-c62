#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cost of the C4FM transmitters (P25 Phase 1 and System Fusion) on QEMU's
# mps2-an505, a Cortex-M33 like the CSK6011B: instructions per frame, and the
# share of a 300 MHz core that means while the carrier is up.  The counter is
# the TCG plugin of tests/host/dmr_enc_qemu/insn_count.c.
#
#   tests/platform/c62_c4fm_tx_cost.sh p25 [frames]
#   tests/platform/c62_c4fm_tx_cost.sh ysf [frames]
#   VOICE=1 tests/platform/c62_c4fm_tx_cost.sh p25 [frames]
#
# With VOICE=1 the harness installs a voice source, so the frame that is timed
# carries a full payload (nine IMBE words in an LDU, five AMBE+2 slots in a
# V/D mode 1 frame) instead of null voice.
#
# Requires arm-none-eabi-gcc with newlib, qemu-system-arm with plugin support,
# gcc and glib headers for the plugin.
set -euo pipefail

WHICH=${1:?usage: c62_c4fm_tx_cost.sh p25|ysf [frames]}
N=${2:-40}
VOICE=${VOICE:-0}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU_DIR=$ROOT/tests/host/dmr_enc_qemu
WORK=${WORK:-$(mktemp -d)}
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
# -O2: the firmware builds this with -O2 (platform/targets/c62/CMakeLists.txt)
COMMON="-O2 -ffunction-sections -fdata-sections -fno-common -fno-strict-aliasing -w"
INC="-I$ROOT/openrtx/include -I$ROOT/openrtx/include/protocols/P25 -I$ROOT/openrtx/include/protocols/YSF"

case "$WHICH" in
    p25)
        DEF=-DP25
        SRCS="$ROOT/openrtx/src/protocols/C4FM/C4fmMod.c \
              $ROOT/openrtx/src/protocols/P25/P25Tx.c \
              $ROOT/openrtx/src/protocols/P25/P25Fec.c \
              $ROOT/openrtx/src/protocols/P25/P25Defs.c \
              $ROOT/openrtx/src/protocols/P25/P25Tables.c"
        FRAME_MS=180
        VOICE_MS=20
        ;;
    ysf)
        DEF=-DYSF
        SRCS="$ROOT/openrtx/src/protocols/C4FM/C4fmMod.c \
              $ROOT/openrtx/src/protocols/YSF/YsfTx.c"
        FRAME_MS=100
        VOICE_MS=20
        ;;
    *)
        echo "unknown chain $WHICH" >&2
        exit 1
        ;;
esac

# -DVOICE takes no value: it is a switch in the harness
if [ "$VOICE" = 1 ]; then
    DEF="$DEF -DVOICE"
fi

mkdir -p "$WORK"
cd "$WORK"

echo "== Cortex-M33 objects ($WHICH)"
objs=""
for s in $SRCS; do
    o="obj_$(basename "${s%.c}").o"
    arm-none-eabi-gcc $ARM $COMMON $DEF $INC -c "$s" -o "$o"
    objs="$objs $o"
done
arm-none-eabi-gcc $ARM $COMMON -specs=rdimon.specs -c "$QEMU_DIR/startup.c" -o startup.o

for n in 0 1 "$N"; do
    arm-none-eabi-gcc $ARM $COMMON $DEF $INC -DITERS=$n \
        -c "$ROOT/tests/platform/c62_c4fm_tx_cost.c" -o "bench_$n.o"
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$QEMU_DIR/mps2.ld" \
        -specs=rdimon.specs -o "${WHICH}_$n.elf" startup.o "bench_$n.o" $objs \
        -lrdimon -lm -lc
    arm-none-eabi-size "${WHICH}_$n.elf" | tail -1 |
        awk -v n=$n '{printf "   ELF frames=%s: text=%s data=%s bss=%s\n", n, $1, $2, $3}'
done

echo "== instruction counter plugin"
gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) "$QEMU_DIR/insn_count.c" \
    -o libinsn.so

run() { # $1 = elf, $2 = log
    timeout 1800 qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$1" -plugin "file=$PWD/libinsn.so" -d plugin >"$2" 2>&1 || true
    grep -o 'INSN_COUNT [0-9]*' "$2" | awk '{print $2}'
}

c0=$(run "${WHICH}_0.elf" run0.log)
c1=$(run "${WHICH}_1.elf" run1.log)
cN=$(run "${WHICH}_$N.elf" "run$N.log")

grep -v INSN_COUNT "run$N.log" | sed 's/^/   /'

python3 - "$c0" "$c1" "$cN" "$N" "$FRAME_MS" "$VOICE_MS" "$WHICH" <<'EOF'
import sys
c0, c1, cN, n, frame_ms, voice_ms = (int(x) for x in sys.argv[1:7])
which = sys.argv[7]
per = (cN - c1) / (n - 1)
print("== instruction counts (QEMU, Cortex-M33)")
print(f"   start-up (no frame)                       : {c0:>10}")
print(f"   first frame (init + build + shaper)       : {c1 - c0:>10}")
print(f"   frame, steady state ({frame_ms} ms)       : {per:>10.0f}")
print(f"   per {voice_ms} ms of audio                : {per * voice_ms / frame_ms:>10.0f}")
print(f"   per second of audio                       : {per * 1000 / frame_ms:>10.0f}")
for cpi in (1.2, 1.6):
    ms = per * cpi / 300e6 * 1e3
    print(f"   300 MHz, CPI {cpi}: {ms:.2f} ms per {frame_ms} ms frame = "
          f"{ms / frame_ms * 100:.1f} % of one core")
EOF
