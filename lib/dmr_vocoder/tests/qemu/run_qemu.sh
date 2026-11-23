#!/bin/bash
# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the vocoder for the host and for an emulated Cortex-M33, decode the same
# AMBE+2 frames on both, compare the PCM byte for byte, and measure the cost per
# frame with QEMU's instruction counter.
#
# usage: ./run_qemu.sh /path/to/frames.ambe [frames]
#
# The input must hold at least <frames>+1 9-byte frames: the extra one separates
# the one-off initialisation (DCT tables, FFT plan) from the per-frame cost.
#
# Requirements: gcc, arm-none-eabi-gcc with newlib, qemu-system-arm with TCG
# plugin support, and glib's headers for the counter plugin.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$(cd "$HERE/.." && pwd)
LIB=$(cd "$TESTS/.." && pwd)
DEC=$LIB/../opendmr/decoder
FRAMES=${1:?usage: run_qemu.sh frames.ambe [frames]}
NFRAMES=${2:-100}
WORK=${WORK:-$(mktemp -d)}

# The OpenDMR sources go through dmr_math.c instead of libm, exactly as in
# lib/dmr_vocoder/CMakeLists.txt.
OPENDMR_SRCS="$DEC/mbelib.c $DEC/mbe_adaptive.c $DEC/mbe_unvoiced_fft.c \
    $DEC/ambe3600x2450.c $DEC/ambe_common.c $DEC/ecc.c $DEC/ecc_const.c \
    $DEC/pffft.c"
OWN_SRCS="$LIB/dmr_vocoder.c $LIB/dmr_golay.c $LIB/dmr_golay_table.c $LIB/dmr_math.c"
OBJS="dmr_vocoder.o dmr_golay.o dmr_golay_table.o dmr_math.o mbelib.o \
    mbe_adaptive.o mbe_unvoiced_fft.o ambe3600x2450.o ambe_common.o ecc.o \
    ecc_const.o pffft.o"

DEFS="-DNDEBUG -DMBE_THREAD_LOCAL= -DPFFFT_MALLOC=dmr_vocoder_alloc \
    -DPFFFT_FREE=dmr_vocoder_free -DM_PI=3.14159265358979323846 \
    -DM_SQRT1_2=0.70710678118654752440 -DM_SQRT2=1.41421356237309504880"
MATH_DEFS="-include $LIB/dmr_math_redirect.h"
PREINC="-include $LIB/dmr_alloc.h"
# -fno-math-errno lets sqrtf become a VSQRT instruction: no libm on the target.
OPTS="-O2 -std=c99 -ffp-contract=off -fno-math-errno -ffunction-sections -fdata-sections -fno-common"
INCS="-I$LIB -I$DEC"

echo "== work dir: $WORK"
mkdir -p "$WORK"
cd "$WORK"

# The bare-metal build has no argv: it always reads dmr_frames.ambe from the
# directory QEMU runs in.
cp "$FRAMES" dmr_frames.ambe

echo "== host build"
for f in $OWN_SRCS $OPENDMR_SRCS; do
    gcc $OPTS $DEFS $MATH_DEFS $PREINC $INCS -c "$f" -o "$(basename "$f" .c).o"
done
gcc $OPTS $DEFS $PREINC $INCS -DITERS=$((NFRAMES + 1)) \
    -c "$TESTS/pcm_dump.c" -o pcm_dump_host.o
# -lm only for sqrtf, which is exactly rounded by IEEE-754
gcc -o pcm_host pcm_dump_host.o $OBJS -lm
./pcm_host dmr_frames.ambe host.pcm

echo "== Cortex-M33 build"
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
arm-none-eabi-gcc $ARM $OPTS -specs=rdimon.specs -c "$HERE/startup.c" -o startup.o
rm -f $OBJS
for f in $OWN_SRCS $OPENDMR_SRCS; do
    arm-none-eabi-gcc $ARM $OPTS $DEFS $MATH_DEFS $PREINC $INCS \
        -c "$f" -o "$(basename "$f" .c).o"
done
for n in 0 1 $((NFRAMES + 1)); do
    arm-none-eabi-gcc $ARM $OPTS $DEFS $PREINC $INCS -DITERS=$n \
        -c "$TESTS/pcm_dump.c" -o "pcm_dump_$n.o"
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$HERE/mps2.ld" \
        -specs=rdimon.specs -o "dmr_$n.elf" startup.o "pcm_dump_$n.o" $OBJS \
        -lrdimon -lc
    arm-none-eabi-size "dmr_$n.elf" | tail -1 | \
        awk -v n=$n '{printf "   ELF iters=%s: text=%s data=%s bss=%s\n", n, $1, $2, $3}'
done

echo "== instruction counter plugin"
gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) "$HERE/insn_count.c" -o libinsn.so

run() { # $1 = elf, $2 = log
    qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$1" -plugin "file=$PWD/libinsn.so" -d plugin >"$2" 2>&1 || true
    grep -o 'INSN_COUNT [0-9]*' "$2" | awk '{print $2}'
}

c0=$(run dmr_0.elf run0.log)
c1=$(run dmr_1.elf run1.log)
cN=$(run "dmr_$((NFRAMES + 1)).elf" runN.log)

mv dmr_out.pcm arm.pcm
echo
echo "== output of the bare-metal run"
grep -v INSN_COUNT runN.log | sed 's/^/   /'

python3 - "$c0" "$c1" "$cN" "$NFRAMES" <<'EOF'
import sys
c0, c1, cN, n = (int(x) for x in sys.argv[1:5])
per = (cN - c1) / n
print("== instruction counts (QEMU, Cortex-M33)")
print(f"   start-up (reset, no frame)                 : {c0:>10}")
print(f"   first frame (DCT tables + FFT plan)        : {c1 - c0:>10}")
print(f"   steady state, {n} frames                     : {cN - c1:>10}")
print(f"   per frame                                  : {per:>10.0f}")
for cpi in (1.2, 1.6):
    ms = per * cpi / 300e6 * 1e3
    print(f"   300 MHz, CPI {cpi}: {ms:.2f} ms per 20 ms frame = "
          f"{ms / 20 * 100:.1f} % of one core")
EOF

# PCM comparison last, so the measurements above survive a mismatch.
if cmp -s host.pcm arm.pcm; then
    echo "== PCM: host and Cortex-M33 are IDENTICAL ($(stat -c%s arm.pcm) bytes)"
else
    echo "== PCM: differs (see docs/DMR_VOCODER.md)"
    cmp host.pcm arm.pcm || true
    python3 - <<'PY'
import struct
a = open('host.pcm', 'rb').read()
b = open('arm.pcm', 'rb').read()
A = struct.unpack('<%dh' % (len(a) // 2), a)
B = struct.unpack('<%dh' % (len(b) // 2), b)
d = [x - y for x, y in zip(A, B)]
print(f"   samples={len(A)} differing={sum(1 for x in d if x)} "
      f"max_abs_diff={max(abs(x) for x in d)}")
PY
    exit 1
fi
