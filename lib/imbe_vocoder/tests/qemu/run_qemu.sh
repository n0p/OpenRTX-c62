#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decode the IMBE corpus on an emulated Cortex-M33 (QEMU mps2-an505, FPv5-SP,
# no cache model), compare the samples with the host build, and count executed
# instructions per frame.
#
#   ./run_qemu.sh [frames.imbe] [frames] [host_reference.pcm]
#
# Defaults: tests/data/speech_frames.imbe, 1000 frames.  A third argument
# compares the target's PCM against that file instead of the host build.
#
# FIRMWARE_BUILD=<build dir> links the objects the Zephyr firmware build
# produced instead of rebuilding them, so the count is that of the shipped code
# with the shipped flags.
#
# Requirements: gcc, arm-none-eabi-gcc with newlib, qemu-system-arm with TCG
# plugin support, glib headers for the counter plugin.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$(cd "$HERE/.." && pwd)
LIB=$(cd "$TESTS/.." && pwd)
FRAMES=${1:-$TESTS/data/speech_frames.imbe}
NFRAMES=${2:-1000}
REF=${3:-}
WORK=${WORK:-$(mktemp -d)}

# The model path may not contract (fidelity); the FFT may (VFMA, no phase).
COMMON="-O2 -std=c99 -fno-math-errno -ffunction-sections -fdata-sections -fno-common"
INC="-I$LIB -I$LIB/../dmr_vocoder"
NF=$NFRAMES

echo "== work dir: $WORK"
mkdir -p "$WORK"
cd "$WORK"

# The bare-metal build has no argv: it reads imbe_frames.imbe from the directory
# QEMU runs in.
head -c $((NF * 11)) "$FRAMES" > imbe_frames.imbe

srcs="imbe_vocoder.c imbe_decoder.c imbe_fft.c"

echo "== host build"
for f in $srcs; do
    case $f in
        imbe_fft.c) o="-ffp-contract=fast" ;;
        *)          o="-ffp-contract=off" ;;
    esac
    gcc $COMMON $o $INC -c "$LIB/$f" -o "${f%.c}_host.o"
done
gcc $COMMON $INC -DIMBE_TEST_HOST -DITERS=$NF -c "$TESTS/pcm_dump.c" -o pcm_dump_host.o
gcc -o pcm_host pcm_dump_host.o imbe_vocoder_host.o imbe_decoder_host.o imbe_fft_host.o -lm
./pcm_host imbe_frames.imbe host.pcm

echo "== Cortex-M33 objects"
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
if [ -n "${FIRMWARE_BUILD:-}" ]; then
    D="$FIRMWARE_BUILD/imbe_vocoder/CMakeFiles/imbe_vocoder.dir"
    echo "   using the firmware build objects from $D"
    objs="$D/imbe_vocoder.c.obj $D/imbe_decoder.c.obj $D/imbe_fft.c.obj"
    for o in $objs; do [ -f "$o" ] || { echo "missing $o" >&2; exit 1; }; done
else
    objs=""
    for f in $srcs; do
        case $f in
            imbe_fft.c) o="-ffp-contract=fast" ;;
            *)          o="-ffp-contract=off" ;;
        esac
        arm-none-eabi-gcc $ARM $COMMON $o $INC -c "$LIB/$f" -o "${f%.c}.o"
        objs="$objs ${f%.c}.o"
    done
fi
arm-none-eabi-gcc $ARM $COMMON -specs=rdimon.specs -c "$HERE/startup.c" -o startup.o
for n in 0 1 $NF; do
    arm-none-eabi-gcc $ARM $COMMON $INC -DITERS=$n -c "$TESTS/pcm_dump.c" -o "pcm_dump_$n.o"
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$HERE/mps2.ld" \
        -specs=rdimon.specs -o "imbe_$n.elf" startup.o "pcm_dump_$n.o" $objs -lrdimon -lc
    arm-none-eabi-size "imbe_$n.elf" | tail -1 |
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

c0=$(run imbe_0.elf run0.log)
c1=$(run imbe_1.elf run1.log)
cN=$(run "imbe_$NF.elf" runN.log)

mv imbe_out.pcm arm.pcm
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
    print(f"   300 MHz, CPI {cpi}: {ms:.2f} ms per 20 ms frame = {ms / 20 * 100:.1f} % of one core")
EOF

echo
echo "== PCM comparison"
rc=0
python3 "$TESTS/compare_pcm.py" host.pcm arm.pcm --min-snr 300 || rc=1
if [ -n "$REF" ]; then
    python3 "$TESTS/compare_pcm.py" "$REF" arm.pcm --min-snr 40 || rc=1
fi
exit $rc
