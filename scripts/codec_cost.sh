#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Codec cost harness for the Cortex-M33 in QEMU: instruction counts per frame
# for the three things that were not measured in this tree before
# (reports/codecs.md §2.7, §2.6 and §2.8):
#
#   codec2   Codec2 3200, encode and decode (M17's vocoder)
#   acelp    OpenACELP decoder, one 137-bit TETRA frame
#   resample 48 kHz <-> 8 kHz polyphase decimator and interpolator, 10 ms block
#            (the kernels of platform/drivers/audio/csk6_resample.h, i.e. the
#            code the driver runs, not a copy of it)
#   resample-tone  the alias and image attenuation of the same kernels
#
#   scripts/codec_cost.sh [O2|Os] [codec2|acelp|resample|resample-tone|all]
#
# Method, the same one docs/DMR_VOCODER.md and docs/IMBE.md use: the same
# harness runs on the host and on an emulated Cortex-M33 (QEMU mps2-an505,
# FPv5-SP, no cache model); the host and target outputs are compared through an
# FNV-1a hash of everything the codec produced, and only then are the
# instruction counts believed.  Two runs per harness, FRAMES=1 and FRAMES=21;
# the per-frame figure is the slope, so start-up and the encode prologue of the
# decode harness cancel out.
#
# Requirements: gcc, arm-none-eabi-gcc with newlib, qemu-system-arm with TCG
# plugin support, glib headers (for the counter plugin), python3.
#
# PROBE points at the instruction probe the other cost scripts use
# (scripts/kiss_cost.sh, scripts/tetra_rx_cost.sh); only its startup.o and
# mps2.ld are reused here, the plain counting plugin is built from
# lib/dmr_vocoder/tests/qemu/insn_count.c so the numbers do not depend on which
# probe happened to be built on this host.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROBE=${PROBE:-/home/n0p/src/c62/work/dsp-probe}
WHAT=${2:-all}
V=${1:-O2}
OUT=${OUT:-$ROOT/build-qemu-codec}
PLUGIN=$OUT/libinsn.so

case "$V" in
  O2) BASE="-O2" ;;
  Os) BASE="-Os" ;;
  *)  echo "unknown variant $V"; exit 1 ;;
esac
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard $BASE"
COMMON="-std=gnu11 -ffunction-sections -fdata-sections -fno-common"

for f in "$PROBE/common/mps2.ld" "$PROBE/common/startup.o"; do
    [ -f "$f" ] || { echo "instruction probe missing: $f (set PROBE=)"; exit 1; }
done

mkdir -p "$OUT"
cd "$OUT" || exit 1

if [ ! -f "$PLUGIN" ]; then
    gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) \
        "$ROOT/lib/dmr_vocoder/tests/qemu/insn_count.c" -o "$PLUGIN" ||
        { echo "cannot build the instruction counter plugin"; exit 1; }
fi

# run <elf> <log>: instruction count of one QEMU run, or nothing.
run_arm() {
    timeout 900 qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$1" -plugin "file=$PLUGIN" -d plugin >"$2" 2>&1
    grep -o 'INSN_COUNT [0-9]*' "$2" | awk '{print $2}'
}

# split <name>: one harness, host and target, two frame counts, hash checked,
# instruction slope and the two CPI conversions printed.
measure() {
    name=$1; shift
    c1=$(run_arm "$name-1.elf" "$name-1.log")
    c21=$(run_arm "$name-21.elf" "$name-21.log")
    h1=$(grep -o "hash=[0-9a-f]*" "$name-21.log" | head -1)
    hh=$(grep -o "hash=[0-9a-f]*" "$name-host.log" 2>/dev/null | head -1)
    s1=$(grep -o "sum=-*[0-9]*" "$name-21.log" | head -1 | cut -d= -f2)
    sh=$(grep -o "sum=-*[0-9]*" "$name-host.log" | head -1 | cut -d= -f2)
    if [ -z "$c1" ] || [ -z "$c21" ]; then
        echo "  $name: no instruction count (see $name-21.log)"; return 1
    fi
    per=$(( (c21 - c1) / 20 ))
    python3 - "$name" "$per" "$h1" "$hh" "${s1:-0}" "${sh:-0}" <<'EOF'
import sys
name, per, h1, hh = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
s1, sh = int(sys.argv[5]), int(sys.argv[6])
# Codec2 is floating point: the M33 build contracts into VFMA, the x86-64
# baseline has no FMA, so the samples may differ by 1 LSB.  The hash must match
# exactly for integer code; for Codec2 the sample sum is compared instead.
if h1 == hh:
    verdict = "host==target"
elif abs(s1 - sh) <= max(1000, abs(sh) // 100):
    verdict = "host~target (float, sum %d vs %d)" % (s1, sh)
else:
    verdict = "HOST/TARGET MISMATCH (sum %d vs %d)" % (s1, sh)
print("  %-12s %10d per frame   %s %s" % (name, per, h1, verdict))
for cpi in (1.2, 1.6):
    print("              300 MHz CPI %.1f: %6.3f ms of 20 ms = %5.1f %% of the core"
          % (cpi, per * cpi / 300e6 * 1e3, per * cpi / 300e6 * 100.0 / 0.020))
EOF
}

# ---------------------------------------------------------------- codec2 ----
codec2() {
    C2=$ROOT/subprojects/codec2
    if [ ! -f "$C2/src/codec2.c" ]; then
        echo "codec2: subprojects/codec2 not present (west update / meson subprojects download)"
        return
    fi
    if [ ! -f data_codec2.c ]; then
        python3 - "$C2/raw/hts1a.raw" <<'EOF'
import struct, sys
raw = open(sys.argv[1], 'rb').read()
n = min(len(raw) // 2 // 160, 150)
s = struct.unpack('<%dh' % (n * 160), raw[:n * 160 * 2])
with open('data_codec2.c', 'w') as f:
    f.write('/* codec2 raw/hts1a.raw: %d frames of 160 samples, 8 kHz, s16 */\n' % n)
    f.write('#include <stdint.h>\n')
    f.write('const unsigned NFRAMES_C2 = %d;\n' % n)
    f.write('const int16_t speech_c2[%d] = {\n' % (n * 160))
    for i in range(0, n * 160, 16):
        f.write(','.join(str(x) for x in s[i:i + 16]) + ',\n')
    f.write('};\n')
EOF
    fi
    # Everything in src/ except the modem applications and the two files that
    # need dependencies this tree does not carry (samplerate.h, GIT_HASH).
    SKIP="c2enc c2dec resample insert_errors ldpc_dec_test ldpc_enc_test modem_probe freedv_api c2sim cohpsk_ch cohpsk_demod cohpsk_get_test_bits cohpsk_mod cohpsk_put_test_bits deframer fdmdv_channel fdmdv_demod fdmdv_get_test_bits fdmdv_mod fdmdv_put_test_bits fm_demod fmfsk_demod fmfsk_mod framer freedv_data_raw_rx freedv_data_raw_tx freedv_data_rx freedv_data_tx freedv_mixed_rx freedv_mixed_tx freedv_rx freedv_tx fsk_demod fsk_get_test_bits fsk_mod_ext_vco fsk_mod fsk_put_test_bits generate_codebook ldpc_dec ldpc_enc ldpc_noise ofdm_demod ofdm_get_test_bits ofdm_mod ofdm_put_test_bits tollr vhf_deframe_c2 vhf_frame_c2"
    arm-none-eabi-gcc $ARM $COMMON -c data_codec2.c -o c2-data_codec2.o || return 1
    gcc $BASE $COMMON -c data_codec2.c -o c2h-data_codec2.o || return 1
    OBJS=""
    for f in "$C2"/src/*.c; do
        b=$(basename "$f" .c)
        case " $SKIP " in *" $b "*) continue ;; esac
        arm-none-eabi-gcc $ARM $COMMON -I"$C2" -I"$C2/src" -c "$f" -o "c2-$b.o" || return 1
        gcc $BASE $COMMON -I"$C2" -I"$C2/src" -c "$f" -o "c2h-$b.o" || return 1
        OBJS="$OBJS c2-$b.o"
    done
    for m in enc dec; do
        arm-none-eabi-gcc $ARM $COMMON -I"$C2" -I"$C2/src" -DMODE_${m^^} \
            -DFRAMES=21 -c "$ROOT/tests/qemu/codec2_cost.c" -o "c2m-$m-21.o" || return 1
        arm-none-eabi-gcc $ARM $COMMON -I"$C2" -I"$C2/src" -DMODE_${m^^} \
            -DFRAMES=1 -c "$ROOT/tests/qemu/codec2_cost.c" -o "c2m-$m-1.o" || return 1
        gcc $BASE $COMMON -I"$C2" -I"$C2/src" -DMODE_${m^^} -DFRAMES=21 \
            -c "$ROOT/tests/qemu/codec2_cost.c" -o "c2hm-$m.o" || return 1
        for n in 1 21; do
            arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$PROBE/common/mps2.ld" \
                -specs=rdimon.specs -o "codec2-$m-$n.elf" "$PROBE/common/startup.o" \
                "c2m-$m-$n.o" "c2-data_codec2.o" $OBJS -lrdimon -lm -lc \
                2>&1 | grep -E "error|undefined" | head -5
        done
        gcc $BASE -Wl,--gc-sections -o "codec2-$m-host" "c2hm-$m.o" c2h-data_codec2.o \
            $(echo "$OBJS" | sed 's/c2-/c2h-/g') -lm && \
            ./codec2-$m-host >"codec2-$m-host.log"
        measure "codec2-$m"
    done
}

# ----------------------------------------------------------------- acelp ----
acelp() {
    OA=$ROOT/forks/OpenACELP
    [ -f "$OA/src/oa_dec.c" ] || OA=/home/n0p/src/c62/forks/OpenACELP
    [ -f "$OA/src/oa_dec.c" ] || { echo "acelp: OpenACELP sources not found"; return; }
    OBJS=""
    for f in oa_dec oa_dsp oa_bits oa_tables_tetra; do
        arm-none-eabi-gcc $ARM $COMMON -I"$OA/include" -I"$OA/src" \
            -c "$OA/src/$f.c" -o "ac-$f.o" || return 1
        gcc $BASE $COMMON -I"$OA/include" -I"$OA/src" -c "$OA/src/$f.c" -o "ach-$f.o" || return 1
        OBJS="$OBJS ac-$f.o"
    done
    for n in 1 21; do
        arm-none-eabi-gcc $ARM $COMMON -I"$OA/include" -DFRAMES=$n \
            -c "$ROOT/tests/qemu/acelp_cost.c" -o "oa-m-$n.o" || return 1
        arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$PROBE/common/mps2.ld" \
            -specs=rdimon.specs -o "acelp-$n.elf" "$PROBE/common/startup.o" \
            "oa-m-$n.o" $OBJS -lrdimon -lc 2>&1 | grep -E "error|undefined" | head -5
    done
    gcc $BASE $COMMON -I"$OA/include" -DFRAMES=21 -c "$ROOT/tests/qemu/acelp_cost.c" \
        -o ach-m.o && gcc $BASE -o acelp-host ach-m.o $(echo "$OBJS" | sed 's/ac-/ach-/g') \
        && ./acelp-host >acelp-host.log
    c1=$(run_arm acelp-1.elf acelp-1.log)
    c21=$(run_arm acelp-21.elf acelp-21.log)
    per=$(( (c21 - c1) / 20 ))
    h1=$(grep -o "hash=[0-9a-f]*" acelp-21.log | head -1)
    hh=$(grep -o "hash=[0-9a-f]*" acelp-host.log | head -1)
    python3 - "$per" "$h1" "$hh" <<'EOF'
import sys
per, h1, hh = int(sys.argv[1]), sys.argv[2], sys.argv[3]
print("  %-12s %10d per 30 ms frame   %s %s" %
      ("acelp", per, h1, "ok" if h1 == hh else "HOST/TARGET MISMATCH"))
for cpi in (1.2, 1.6):
    print("              300 MHz CPI %.1f: %6.3f ms of 30 ms = %5.1f %% of the core"
          % (cpi, per * cpi / 300e6 * 1e3, per * cpi / 300e6 * 100.0 / 0.030))
EOF
}

# -------------------------------------------------------------- resample ----
# The harness compiles the shipping kernels of csk6_resample.h, so these are
# the instructions the driver pays, and the TONE build measures the alias and
# image attenuation of the same code (reports/codecs.md section 3.1).
RESAMPLE_INC="-I$ROOT/platform/drivers/audio"

resample() {
    for v in dec int both; do
        case $v in
            dec)  D="-DONLY_DEC" ;;
            int)  D="-DONLY_INT" ;;
            both) D="" ;;
        esac
        for n in 1 21; do
            arm-none-eabi-gcc $ARM $COMMON $RESAMPLE_INC $D -DFRAMES=$n \
                -c "$ROOT/tests/qemu/resample_cost.c" -o "rs-$v-$n.o" || return 1
            arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$PROBE/common/mps2.ld" \
                -specs=rdimon.specs -o "rs-$v-$n.elf" "$PROBE/common/startup.o" \
                "rs-$v-$n.o" -lrdimon -lc -lm 2>&1 | grep -E "error|undefined" | head -5
        done
        gcc $BASE $COMMON $RESAMPLE_INC $D -DFRAMES=21 \
            -c "$ROOT/tests/qemu/resample_cost.c" -o "rsh-$v.o" && \
            gcc $BASE -o "rs-$v-host" "rsh-$v.o" -lm && \
            ./rs-$v-host >"rs-$v-host.log"
        c1=$(run_arm "rs-$v-1.elf" "rs-$v-1.log")
        c21=$(run_arm "rs-$v-21.elf" "rs-$v-21.log")
        per=$(( (c21 - c1) / 20 ))
        h21=$(grep -o "hash=[0-9a-f]*" "rs-$v-21.log" | head -1)
        hh=$(grep -o "hash=[0-9a-f]*" "rs-$v-host.log" | head -1)
        python3 - "$v" "$per" "$h21" "$hh" <<'EOF'
import sys
v, per = sys.argv[1], int(sys.argv[2])
same = "host==target" if (sys.argv[3] and sys.argv[3] == sys.argv[4]) else \
       "HOST/TARGET MISMATCH (%s vs %s)" % (sys.argv[3], sys.argv[4])
print("  %-12s %10d per 10 ms block (%5.2f ms/s at 300 MHz, CPI 1.2)  %s"
      % ("resample " + v, per, per * 100 * 1.2 / 300e6 * 1e3, same))
EOF
    done
}

# resample-tone: alias and image attenuation, measured through the same kernels.
resample_tone() {
    arm-none-eabi-gcc $ARM $COMMON $RESAMPLE_INC -DTONE \
        -c "$ROOT/tests/qemu/resample_cost.c" -o rs-tone.o || return 1
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$PROBE/common/mps2.ld" \
        -specs=rdimon.specs -o rs-tone.elf "$PROBE/common/startup.o" \
        rs-tone.o -lrdimon -lc -lm 2>&1 | grep -E "error|undefined" | head -5
    gcc $BASE $COMMON $RESAMPLE_INC -DTONE \
        -c "$ROOT/tests/qemu/resample_cost.c" -o rs-tone-host.o && \
        gcc $BASE -o rs-tone-host rs-tone-host.o -lm && ./rs-tone-host
    timeout 900 qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel rs-tone.elf 2>&1 | grep RESAMPLE-TONE
}

case "$WHAT" in
    codec2)   echo "== codec2 ($V)";   codec2 ;;
    acelp)    echo "== acelp ($V)";    acelp ;;
    resample) echo "== resample ($V)"; resample ;;
    resample-tone) echo "== resample tone ($V)"; resample_tone ;;
    all)      echo "== codec2 ($V)";   codec2
              echo "== acelp ($V)";    acelp
              echo "== resample ($V)"; resample
              echo "== resample tone ($V)"; resample_tone ;;
    *) echo "usage: $0 [O2|Os] [codec2|acelp|resample|resample-tone|all]"; exit 1 ;;
esac
echo "logs and ELFs: $OUT"
