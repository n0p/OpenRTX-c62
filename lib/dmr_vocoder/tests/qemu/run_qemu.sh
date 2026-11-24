#!/bin/bash
# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the vocoder for the host and for an emulated Cortex-M33, decode the same
# AMBE+2 frames on both, compare the PCM with each other and with the reference
# decoder, and measure the cost per frame with QEMU's instruction counter.
#
# usage: ./run_qemu.sh [frames.ambe] [frames] [ref.pcm]
#
# Defaults: tests/data/speech.ambe, 1000 frames, tests/data/speech_ref.pcm (the
# same 1000 frames decoded by OpenDMR's own decoder, see tools/make_ref.sh).
#
# FIRMWARE_BUILD=<build dir> links the objects the Zephyr firmware build
# produced (build_c62/dmr_vocoder/CMakeFiles/dmr_vocoder.dir/*.o) instead of
# rebuilding them, so the instruction count is that of the shipped code with the
# shipped flags.  Without it, the sources are rebuilt here with the same
# per-file options as lib/dmr_vocoder/CMakeLists.txt.
#
# Requirements: gcc, arm-none-eabi-gcc with newlib, qemu-system-arm with TCG
# plugin support, and glib headers for the counter plugin.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$(cd "$HERE/.." && pwd)
LIB=$(cd "$TESTS/.." && pwd)
FRAMES=${1:-$TESTS/data/speech.ambe}
NFRAMES=${2:-1000}
REF=${3:-$TESTS/data/speech_ref.pcm}
WORK=${WORK:-$(mktemp -d)}

# Per-file options, as in lib/dmr_vocoder/CMakeLists.txt: the model path may not
# contract (fidelity), the FFT may.
COMMON="-O2 -std=c99 -fno-math-errno -ffunction-sections -fdata-sections -fno-common"
NF=$NFRAMES

echo "== work dir: $WORK"
mkdir -p "$WORK"
cd "$WORK"

# The bare-metal build has no argv: it always reads dmr_frames.ambe from the
# directory QEMU runs in.
head -c $((NF * 9)) "$FRAMES" > dmr_frames.ambe

srcs="dmr_vocoder.c dmr_decoder.c dmr_fft.c"

echo "== host build"
for f in $srcs; do
    case $f in
        dmr_fft.c) o="-ffp-contract=fast" ;;
        *)         o="-ffp-contract=off" ;;
    esac
    gcc $COMMON $o -I"$LIB" -c "$LIB/$f" -o "${f%.c}_host.o"
done
gcc $COMMON -I"$LIB" -DITERS=$NF -c "$TESTS/pcm_dump.c" -o pcm_dump_host.o
# -lm only for the host's fmaf (on the M33 __builtin_fmaf is a VFMA instruction)
gcc -o pcm_host pcm_dump_host.o dmr_vocoder_host.o dmr_decoder_host.o dmr_fft_host.o -lm
./pcm_host dmr_frames.ambe host.pcm

echo "== Cortex-M33 objects"
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
if [ -n "${FIRMWARE_BUILD:-}" ]; then
    D="$FIRMWARE_BUILD/dmr_vocoder/CMakeFiles/dmr_vocoder.dir"
    echo "   using the firmware build objects from $D"
    objs="$D/dmr_vocoder.c.obj $D/dmr_decoder.c.obj $D/dmr_fft.c.obj"
    for o in $objs; do [ -f "$o" ] || { echo "missing $o" >&2; exit 1; }; done
else
    objs=""
    for f in $srcs; do
        case $f in
            dmr_fft.c) o="-ffp-contract=fast" ;;
            *)         o="-ffp-contract=off" ;;
        esac
        arm-none-eabi-gcc $ARM $COMMON $o -I"$LIB" -c "$LIB/$f" -o "${f%.c}.o"
        objs="$objs ${f%.c}.o"
    done
fi
arm-none-eabi-gcc $ARM $COMMON -specs=rdimon.specs -c "$HERE/startup.c" -o startup.o
for n in 0 1 $NF; do
    arm-none-eabi-gcc $ARM $COMMON -I"$LIB" -DITERS=$n \
        -c "$TESTS/pcm_dump.c" -o "pcm_dump_$n.o"
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$HERE/mps2.ld" \
        -specs=rdimon.specs -o "dmr_$n.elf" startup.o "pcm_dump_$n.o" $objs \
        -lrdimon -lc
    arm-none-eabi-size "dmr_$n.elf" | tail -1 |
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
cN=$(run "dmr_$NF.elf" runN.log)

mv dmr_out.pcm arm.pcm
echo
echo "== output of the bare-metal run"
grep -v INSN_COUNT runN.log | sed 's/^/   /'

python3 - "$c0" "$c1" "$cN" "$NFRAMES" <<'EOF'
import sys
c0, c1, cN, n = (int(x) for x in sys.argv[1:5])
# ELFs: 0 frames, 1 frame, n frames.  The first frame carries the one-off init
# and the fade-in; the remaining n-1 are the steady state.
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
echo "== PCM comparison"
rc=0
python3 "$TESTS/compare_pcm.py" host.pcm arm.pcm --min-snr 300 || rc=1
python3 "$TESTS/compare_pcm.py" "$REF" host.pcm --min-snr 60 || rc=1
python3 "$TESTS/compare_pcm.py" "$REF" arm.pcm --min-snr 60 || rc=1
exit $rc
