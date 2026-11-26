#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cost of the DMR transmitter's AMBE+2 encoder, instructions per 20 ms frame.
#
#   tests/host/dmr_enc_qemu/run.sh speech.wav [frames] [opendmr dir]
#
# Builds OpenDMR's encoder for Cortex-M33 with the flags the C62 firmware
# compiles it with (-Os, VFPv5 single precision, hard float), runs it bare
# metal on QEMU's mps2-an505 with a TCG plugin that counts every executed
# instruction, and prints the cost of one frame plus the share of a 300 MHz
# core it takes while a call is up.
#
# Three ELFs are built and run: 0 frames (start-up), 1 frame (encoder creation
# plus a cold frame) and N frames.  The steady-state cost is
# (cN - c1) / (N - 1): what the transmitter pays per frame of speech.
#
# The input is any 8 kHz mono 16-bit WAV of speech (python3's wave module
# converts it); encoder cost depends mildly on content, mostly on how much of
# it is voiced, so use real speech and not a tone.
#
# Requirements: arm-none-eabi-gcc with newlib, qemu-system-arm with TCG plugin
# support, gcc and glib headers for the plugin, python3.  OpenDMR defaults to
# the lib/opendmr submodule of this tree.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WAV=${1:?usage: run.sh speech.wav [frames] [opendmr dir]}
NFRAMES=${2:-300}
OD=${3:-$ROOT/lib/opendmr}
# The mbelib the firmware links, for the decoder side; without it that half is
# skipped (scripts/c62/fetch_mbelib.sh puts one in <west>/modules/lib/mbelib)
WORK=${WORK:-$(mktemp -d)}
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
COMMON="-Os -ffunction-sections -fdata-sections -fno-common -fno-strict-aliasing -w"
CXXCOMMON="$COMMON -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit"

[ -f "$OD/opendmr.h" ] || { echo "no OpenDMR in $OD" >&2; exit 1; }
mkdir -p "$WORK"
cd "$WORK"

echo "== work dir: $WORK"
python3 - "$WAV" pcm8k.raw <<'EOF'
import sys, wave
src, dst = sys.argv[1], sys.argv[2]
w = wave.open(src)
if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != 8000:
    sys.exit("%s: need 8 kHz mono 16-bit" % src)
open(dst, "wb").write(w.readframes(w.getnframes()))
EOF

# The same translation units c62_opendmr is built from: the encoder needs
# mbelib's parameter init from the decoder half, which is why both are there.
SRCS="opendmr.cpp
      decoder/mbelib.c decoder/mbe_adaptive.c decoder/mbe_unvoiced_fft.c
      decoder/ambe3600x2450.c decoder/ambe_common.c decoder/ecc.c
      decoder/ecc_const.c decoder/pffft.c decoder/fftpack.c
      encoder/cgolay24128.cpp encoder/mbeenc.cpp encoder/aux_sub.cc
      encoder/basicop2.cc encoder/ch_encode.cc encoder/dc_rmv.cc
      encoder/dsp_sub.cc encoder/encode.cc encoder/imbe_vocoder.cc
      encoder/imbe_vocoder_impl.cc encoder/math_sub.cc encoder/pe_lpf.cc
      encoder/pitch_est.cc encoder/pitch_ref.cc encoder/qnt_sub.cc
      encoder/rand_gen.cc encoder/sa_encode.cc encoder/tbls.cc
      encoder/v_uv_det.cc"

# No TLS on a bare-metal Cortex-M: the firmware builds with the same empty
# define (platform/targets/c62/CMakeLists.txt).
DEFS="-DMBE_THREAD_LOCAL= -DM_PI=3.14159265358979323846f -D_GNU_SOURCE"

echo "== Cortex-M33 objects ($(echo $SRCS | wc -w) files)"
objs=""
for s in $SRCS; do
    o="obj_$(echo "$s" | tr '/' '_' | sed 's/\.[^.]*$/.o/')"
    case $s in
        *.c) arm-none-eabi-gcc $ARM $COMMON -std=gnu11 $DEFS \
                 -I"$OD" -I"$OD/decoder" -I"$OD/encoder" -c "$OD/$s" -o "$o" ;;
        *)   arm-none-eabi-g++ $ARM $CXXCOMMON -std=gnu++11 $DEFS \
                 -I"$OD" -I"$OD/decoder" -I"$OD/encoder" -c "$OD/$s" -o "$o" ;;
    esac
    objs="$objs $o"
done

arm-none-eabi-gcc $ARM $COMMON -specs=rdimon.specs -c "$HERE/startup.c" -o startup.o
for n in 0 1 "$NFRAMES"; do
    arm-none-eabi-gcc $ARM $COMMON -I"$OD" -DITERS=$n \
        -c "$HERE/enc_bench.c" -o "bench_$n.o"
    arm-none-eabi-g++ $ARM -Wl,--gc-sections -T "$HERE/mps2.ld" \
        -specs=rdimon.specs -o "enc_$n.elf" startup.o "bench_$n.o" $objs \
        -lrdimon -lc
    arm-none-eabi-size "enc_$n.elf" | tail -1 |
        awk -v n=$n '{printf "   ELF iters=%s: text=%s data=%s bss=%s\n", n, $1, $2, $3}'
done

echo "== instruction counter plugin"
gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) "$HERE/insn_count.c" \
    -o libinsn.so

run() { # $1 = elf, $2 = log
    timeout 900 qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$1" -plugin "file=$PWD/libinsn.so" -d plugin >"$2" 2>&1 || true
    grep -o 'INSN_COUNT [0-9]*' "$2" | awk '{print $2}'
}

c0=$(run enc_0.elf run0.log)
c1=$(run enc_1.elf run1.log)
cN=$(run "enc_$NFRAMES.elf" "run$NFRAMES.log")

echo
echo "== output of the bare-metal run (N frames)"
grep -v INSN_COUNT "run$NFRAMES.log" | sed 's/^/   /'
echo "== output of the 1-frame run"
grep -v INSN_COUNT run1.log | sed 's/^/   /'

python3 - "$c0" "$c1" "$cN" "$NFRAMES" <<'EOF'
import sys
c0, c1, cN, n = (int(x) for x in sys.argv[1:5])
per = (cN - c1) / (n - 1)
print("== instruction counts (QEMU, Cortex-M33)")
print(f"   start-up (no frame)                        : {c0:>10}")
print(f"   first frame (encoder creation + encode)    : {c1 - c0:>10}")
print(f"   steady state, {n - 1} frames                    : {cN - c1:>10}")
print(f"   encoder, per frame                         : {per:>10.0f}")
for cpi in (1.2, 1.6):
    ms = per * cpi / 300e6 * 1e3
    print(f"   300 MHz, CPI {cpi}: {ms:.2f} ms per 20 ms frame = "
          f"{ms / 20 * 100:.1f} % of one core")
EOF
