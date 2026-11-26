#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cost of the transmitter with our vocoders, instructions per 30 ms voice
# burst and per 20 ms frame: lib/dmr_encoder + dmr_tx_voice() + the 4FSK
# modulator, bare metal on QEMU's mps2-an505 (Cortex-M33), every executed
# instruction counted by a TCG plugin.
#
#   tests/host/dmr_enc_qemu/run_own.sh speech8k.pcm [bursts]
#
# Three ELFs are run: 0 bursts (start-up), 1 (init + first burst, cold) and N;
# the steady-state cost is (cN - c1) / (N - 1).
#
# Requirements: arm-none-eabi-gcc with newlib, qemu-system-arm with TCG plugin
# support, gcc and glib headers for the plugin.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
PCM=${1:?usage: run_own.sh speech8k.pcm [bursts]}
NB=${2:-60}
WORK=${WORK:-$(mktemp -d)}
ARM="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
COMMON="-Os -ffunction-sections -fdata-sections -fno-common -fno-strict-aliasing -w"
SRC=$ROOT/openrtx/src/protocols/DMR
LIB_ENC=$ROOT/lib/dmr_encoder
INC="-I$ROOT/openrtx/include -I$ROOT/openrtx/include/protocols/DMR -I$SRC -I$LIB_ENC"

mkdir -p "$WORK"
cd "$WORK"
cp "$PCM" pcm8k.raw

echo "== Cortex-M33 objects"
objs=""
for s in "$SRC/dmr_enc.c" "$SRC/dmr_tx.c" "$SRC/dmr_fec.c" \
         "$SRC/dmr_trellis.c" \
         "$LIB_ENC/dmr_encoder.c" "$LIB_ENC/dmr_enc_core.c" \
         "$LIB_ENC/dmr_enc_frame.c" "$LIB_ENC/dmr_enc_ambe_tables.c" \
         "$LIB_ENC/dmr_enc_trig.c"; do
    [ -f "$s" ] || { echo "missing $s" >&2; exit 1; }
    o="obj_$(basename "${s%.c}").o"
    # The library's own per-file flags (lib/dmr_encoder/CMakeLists.txt)
    case $s in
        */dmr_enc_core.c|*/dmr_enc_frame.c|*/dmr_enc_ambe_tables.c|*/dmr_enc_trig.c|*/dmr_encoder.c)
            extra="-O2 -ffp-contract=off -fno-math-errno -Wno-array-bounds" ;;
        *) extra="" ;;
    esac
    arm-none-eabi-gcc $ARM $COMMON $extra $INC -c "$s" -o "$o"
    objs="$objs $o"
done
arm-none-eabi-gcc $ARM $COMMON -specs=rdimon.specs -c "$HERE/startup.c" -o startup.o

for n in 0 1 "$NB"; do
    arm-none-eabi-gcc $ARM $COMMON $INC -DITERS=$n -c "$HERE/bench_own.c" \
        -o "bench_$n.o"
    arm-none-eabi-gcc $ARM -Wl,--gc-sections -T "$HERE/mps2.ld" \
        -specs=rdimon.specs -o "own_$n.elf" startup.o "bench_$n.o" $objs \
        -lrdimon -lm -lc
    arm-none-eabi-size "own_$n.elf" | tail -1 |
        awk -v n=$n '{printf "   ELF bursts=%s: text=%s data=%s bss=%s\n", n, $1, $2, $3}'
done

echo "== instruction counter plugin"
gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) "$HERE/insn_count.c" \
    -o libinsn.so

run() { # $1 = elf, $2 = log
    timeout 1800 qemu-system-arm -M mps2-an505 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$1" -plugin "file=$PWD/libinsn.so" -d plugin >"$2" 2>&1 || true
    grep -o 'INSN_COUNT [0-9]*' "$2" | awk '{print $2}'
}

c0=$(run own_0.elf run0.log)
c1=$(run own_1.elf run1.log)
cN=$(run "own_$NB.elf" "run$NB.log")

grep -v INSN_COUNT "run$NB.log" | sed 's/^/   /'

python3 - "$c0" "$c1" "$cN" "$NB" <<'EOF'
import sys
c0, c1, cN, n = (int(x) for x in sys.argv[1:5])
per = (cN - c1) / (n - 1)
print("== instruction counts (QEMU, Cortex-M33)")
print(f"   start-up (no burst)                        : {c0:>10}")
print(f"   first burst (init + encode + modulate)     : {c1 - c0:>10}")
print(f"   voice burst, steady state                  : {per:>10.0f}")
print(f"   per 20 ms frame (a third of a burst)       : {per / 3:>10.0f}")
for cpi in (1.2, 1.6):
    ms = per / 3 * cpi / 300e6 * 1e3
    print(f"   300 MHz, CPI {cpi}: {ms:.2f} ms per 20 ms frame = "
          f"{ms / 20 * 100:.1f} % of one core")
EOF
