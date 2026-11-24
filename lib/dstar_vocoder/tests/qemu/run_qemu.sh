#!/bin/bash
# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the D-STAR vocoder for the host and for an emulated Cortex-M33, decode
# the same frames on both, compare the PCM with each other and with the
# reference decoder, and measure the cost per frame with QEMU's instruction
# counter.
#
# usage: ./run_qemu.sh [bank.air] [frames] [ref.pcm]
#
# Defaults: tests/data/bank.air, 400 frames, tests/data/bank_ref.pcm (the same
# frames decoded by mbelib-neo v2.6.2, see tools/make_ref.sh).
#
# FIRMWARE_BUILD=<build dir> links the objects the Zephyr firmware build
# produced instead of rebuilding them, so the count is that of the shipped code
# with the shipped flags.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$(cd "$HERE/.." && pwd)
LIB=$(cd "$TESTS/.." && pwd)
ROOT=$(cd "$LIB/../.." && pwd)
FRAMES=${1:-$TESTS/data/bank.air}
NFRAMES=${2:-400}
REF=${3:-$TESTS/data/bank_ref.pcm}
WORK=${WORK:-$(mktemp -d)}

COMMON="-O2 -std=c99 -fno-math-errno -ffunction-sections -fdata-sections -fno-common"
NF=$NFRAMES

echo "== work dir: $WORK"
mkdir -p "$WORK"
cd "$WORK"
head -c $((NF * 9)) "$FRAMES" > dstar_frames.air

srcs="dstar_vocoder.c dstar_decoder.c ../dmr_vocoder/dmr_fft.c ../dmr_vocoder/dmr_golay.c ../dmr_vocoder/dmr_golay_table.c ../../openrtx/src/protocols/DSTAR/DStarVocoder.c"

echo "== host build"
objs_host=""
for f in dstar_vocoder.c dstar_decoder.c; do
    gcc $COMMON -ffp-contract=off -I"$LIB" -I"$ROOT/lib/dmr_vocoder" \
        -I"$ROOT/openrtx/include" -c "$LIB/$f" -o "${f%.c}_host.o"
    objs_host="$objs_host ${f%.c}_host.o"
done
for f in dmr_fft.c dmr_golay.c dmr_golay_table.c; do
    gcc $COMMON -ffp-contract=fast -I"$ROOT/lib/dmr_vocoder" \
        -c "$ROOT/lib/dmr_vocoder/$f" -o "${f%.c}_host.o"
    objs_host="$objs_host ${f%.c}_host.o"
done
gcc $COMMON -ffp-contract=off -I"$ROOT/lib/dmr_vocoder" -I"$ROOT/openrtx/include" \
    -c "$ROOT/openrtx/src/protocols/DSTAR/DStarVocoder.c" -o DStarVocoder_host.o
objs_host="$objs_host DStarVocoder_host.o"
gcc $COMMON -ffp-contract=off -DSTAR_TEST_HOST -DITERS=$NF -I"$LIB" \
    -I"$ROOT/lib/dmr_vocoder" -I"$ROOT/openrtx/include" \
    -c "$TESTS/pcm_dump.c" -o pcm_dump_host.o
gcc -o pcm_host pcm_dump_host.o $objs_host -lm
./pcm_host dstar_frames.air host.pcm

echo "== Cortex-M33 objects"
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
objs=""
if [ -n "${FIRMWARE_BUILD:-}" ]; then
    echo "   using the firmware objects from $FIRMWARE_BUILD"
    # The Zephyr build compiles every source of the app target into
    # CMakeFiles/app.dir/<path as seen by cmake>.c.obj, so walk it by path
    # instead of guessing a flat layout.
    for p in \
        lib/dstar_vocoder/dstar_vocoder.c.obj \
        lib/dstar_vocoder/dstar_decoder.c.obj \
        openrtx/src/protocols/DSTAR/DStarVocoder.c.obj; do
        o="$FIRMWARE_BUILD/CMakeFiles/app.dir/$p"
        [ -f "$o" ] || o=$(find "$FIRMWARE_BUILD" -name "$(basename "$p")" \
                                  -path '*app.dir*' | head -1)
        [ -n "$o" ] && [ -f "$o" ] || { echo "missing $p" >&2; exit 1; }
        objs="$objs $o"
    done
    for o in dmr_fft dmr_golay dmr_golay_table; do
        p=$(find "$FIRMWARE_BUILD" -name "$o.c.obj" -path '*app.dir*' | head -1)
        [ -n "$p" ] || p=$(find "$FIRMWARE_BUILD" -name "$o.c.obj" | head -1)
        [ -n "$p" ] || { echo "missing $o" >&2; exit 1; }
        objs="$objs $p"
    done
else
    for f in dstar_vocoder.c dstar_decoder.c; do
        arm-none-eabi-gcc $ARM $COMMON -ffp-contract=off -I"$LIB" \
            -I"$ROOT/lib/dmr_vocoder" -I"$ROOT/openrtx/include" \
            -c "$LIB/$f" -o "${f%.c}.o"
        objs="$objs ${f%.c}.o"
    done
    for f in dmr_fft.c dmr_golay.c dmr_golay_table.c; do
        arm-none-eabi-gcc $ARM $COMMON -ffp-contract=fast \
            -I"$ROOT/lib/dmr_vocoder" -c "$ROOT/lib/dmr_vocoder/$f" -o "${f%.c}.o"
        objs="$objs ${f%.c}.o"
    done
    arm-none-eabi-gcc $ARM $COMMON -ffp-contract=off \
        -I"$ROOT/lib/dmr_vocoder" -I"$ROOT/openrtx/include" -c \
        "$ROOT/openrtx/src/protocols/DSTAR/DStarVocoder.c" -o DStarVocoder.o
    objs="$objs DStarVocoder.o"
fi
arm-none-eabi-gcc $ARM $COMMON -specs=rdimon.specs -c "$HERE/startup.c" -o startup.o
for n in 0 1 $NF; do
    arm-none-eabi-gcc $ARM $COMMON -ffp-contract=off -DITERS=$n -I"$LIB" \
        -I"$ROOT/lib/dmr_vocoder" -I"$ROOT/openrtx/include" \
        -c "$TESTS/pcm_dump.c" -o "pcm_dump_$n.o"
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$HERE/mps2.ld" \
        -specs=rdimon.specs -o "dstar_$n.elf" startup.o "pcm_dump_$n.o" $objs \
        -lrdimon -lc -lm
    arm-none-eabi-size "dstar_$n.elf" | tail -1 |
        awk -v n=$n '{printf "   ELF iters=%s: text=%s data=%s bss=%s\n", n, $1, $2, $3}'
done

echo "== instruction counter plugin"
gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) "$HERE/insn_count.c" -o libinsn.so

run() {
    qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$1" -plugin "file=$PWD/libinsn.so" -d plugin >"$2" 2>&1 || true
    grep -o 'INSN_COUNT [0-9]*' "$2" | awk '{print $2}'
}

c0=$(run dstar_0.elf run0.log)
c1=$(run dstar_1.elf run1.log)
cN=$(run "dstar_$NF.elf" runN.log)

mv dstar_out.pcm arm.pcm
echo
echo "== output of the bare-metal run"
grep -v INSN_COUNT runN.log | sed 's/^/   /'

python3 - "$c0" "$c1" "$cN" "$NFRAMES" <<'EOF'
import sys
c0, c1, cN, n = (int(x) for x in sys.argv[1:5])
per = (cN - c1) / (n - 1)
print("== instruction counts (QEMU, Cortex-M33)")
print(f"   start-up (reset, no frame)                 : {c0:>10}")
print(f"   first frame (init + decode)                : {c1 - c0:>10}")
print(f"   steady state, {n - 1} frames                   : {cN - c1:>10}")
print(f"   per frame                                  : {per:>10.0f}")
for cpi in (1.2, 1.6):
    ms = per * cpi / 300e6 * 1e3
    print(f"   300 MHz, CPI {cpi}: {ms:.2f} ms per 20 ms frame = "
          f"{ms / 20 * 100:.1f} % of one core")
EOF

echo
echo "== PCM comparison (host vs target, both vs the reference)"
rc=0
python3 - arm.pcm host.pcm <<'EOF' || rc=1
import struct, sys
def load(p):
    d = open(p, 'rb').read()
    return struct.unpack('<%dh' % (len(d) // 2), d)
a, b = load(sys.argv[1]), load(sys.argv[2])
if len(a) != len(b):
    print(f"   length mismatch {len(a)} vs {len(b)}")
    sys.exit(1)
bad = sum(1 for x, y in zip(a, b) if x != y)
print(f"   host vs target: {len(a)} samples, {bad} differ")
sys.exit(0 if bad == 0 else 1)
EOF
exit $rc
