#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Receive cost of the DMR package with our vocoder: instructions per second of
# 48 kHz discriminator audio, bare metal on QEMU's mps2-an505 (Cortex-M33).
#
#   tests/host/dmr_pkg_qemu/run_rx.sh capture.wav [seconds]
#
# Two ELFs: SECS=0 (start-up + mode init) and SECS=N (same + N seconds of the
# capture through demod, proto and the vocoder); the difference divided by N is
# the receive cost.  The capture must be 48 kHz mono 16-bit.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ENCQ=$(cd "$HERE/../dmr_enc_qemu" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
WAV=${1:?usage: run_rx.sh capture.wav [seconds]}
SECS=${2:-12}
WORK=${WORK:-$(mktemp -d)}
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
COMMON="-Os -ffunction-sections -fdata-sections -fno-common -fno-strict-aliasing -w"
SRC=$ROOT/openrtx/src/protocols/DMR
VOC=$ROOT/lib/dmr_vocoder
INC="-I$ROOT/openrtx/include -I$ROOT/openrtx/include/protocols/DMR -I$ROOT/openrtx/include/protocols/DMR/trunk -I$SRC -I$SRC/trunk -I$VOC"
DEFS="-DCONFIG_DMR_TRUNK -DDMR_VOICE_OWN -DDMR_PORT_NO_CLOCK"

mkdir -p "$WORK"
cd "$WORK"

python3 - "$WAV" discr.raw <<'EOF'
import sys, wave
w = wave.open(sys.argv[1])
if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != 48000:
    sys.exit("%s: need 48 kHz mono 16-bit discriminator audio" % sys.argv[1])
open(sys.argv[2], "wb").write(w.readframes(w.getnframes()))
EOF

echo "== Cortex-M33 objects"
objs=""
srcs="$SRC/DmrDefs.c $SRC/DmrStatus.c $SRC/dmr_glue.c $SRC/dmr_demod.c
      $SRC/dmr_fec.c $SRC/dmr_trellis.c $SRC/dmr_proto.c $SRC/dmr_data.c
      $SRC/dmr_lrrp.c $SRC/dmr_track.c $SRC/dmr_voice.c
      $SRC/dmr_ep.c $SRC/dmr_aes.c $SRC/dmr_hytera.c $SRC/aes/tinyaes.c
      $SRC/aes/tinyaes128.c $SRC/aes/des.c $SRC/trunk/dmr_trunk.c
      $SRC/trunk/t3_ms.c $SRC/trunk/t3_pdu.c $SRC/trunk/t3_plan.c
      $SRC/trunk/t3_sys.c"
# The vocoder keeps the firmware's per-file flags (lib/dmr_vocoder/CMakeLists)
voc="$VOC/dmr_vocoder.c $VOC/dmr_decoder.c $VOC/dmr_fft.c"

for s in $srcs; do
    o="obj_$(basename "${s%.c}").o"
    arm-none-eabi-gcc $ARM $COMMON $INC $DEFS -c "$s" -o "$o"
    objs="$objs $o"
done
for s in $voc; do
    o="obj_$(basename "${s%.c}").o"
    case $s in
        */dmr_fft.c) extra="-O2 -ffp-contract=fast -fno-math-errno" ;;
        *)           extra="-O2 -ffp-contract=off -fno-math-errno" ;;
    esac
    arm-none-eabi-gcc $ARM $COMMON $extra $INC $DEFS -c "$s" -o "$o"
    objs="$objs $o"
done
arm-none-eabi-gcc $ARM $COMMON -specs=rdimon.specs -c "$ENCQ/startup.c" -o startup.o

for n in 0 "$SECS"; do
    arm-none-eabi-gcc $ARM $COMMON $INC $DEFS -DSECS=$n -c "$HERE/bench_rx.c" \
        -o "bench_$n.o"
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$ENCQ/mps2.ld" \
        -specs=rdimon.specs -o "rx_$n.elf" startup.o "bench_$n.o" $objs \
        -lrdimon -lm -lc
    arm-none-eabi-size "rx_$n.elf" | tail -1 |
        awk -v n=$n '{printf "   ELF secs=%s: text=%s data=%s bss=%s\n", n, $1, $2, $3}'
done

echo "== instruction counter plugin"
gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) "$ENCQ/insn_count.c" \
    -o libinsn.so

run() { # $1 = elf, $2 = log
    timeout 3600 qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$1" -plugin "file=$PWD/libinsn.so" -d plugin >"$2" 2>&1 || true
    grep -o 'INSN_COUNT [0-9]*' "$2" | awk '{print $2}'
}

c0=$(run rx_0.elf run0.log)
cN=$(run "rx_$SECS.elf" "run$SECS.log")

grep -v INSN_COUNT "run$SECS.log" | sed 's/^/   /'

python3 - "$c0" "$cN" "$SECS" <<'EOF'
import sys
c0, cN, n = (int(x) for x in sys.argv[1:4])
per_s = (cN - c0) / n
print("== instruction counts (QEMU, Cortex-M33)")
print(f"   start-up + mode init                : {c0:>12}")
print(f"   {n} s of audio, whole receiver       : {cN - c0:>12}")
print(f"   receiver, per second of audio       : {per_s:>12.0f}")
for cpi in (1.2, 1.6):
    pct = per_s * cpi / 300e6 * 100
    print(f"   300 MHz, CPI {cpi}: {pct:.1f} % of one core")
EOF
