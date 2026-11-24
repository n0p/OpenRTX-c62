#!/bin/bash
# Build and run the TETRA receive cost harness for the Cortex-M33 in QEMU,
# with the per-frame instruction counter of work/dsp-probe.
#
#   scripts/tetra_rx_cost.sh [variant]
#     variant: dspO2 (default), dspOs, nodspO2, dspO2crypto
#
# Prints the instruction count between frame_marker() calls, i.e. per 30 ms
# block, for the demodulator and for libtetra separately.
set -uo pipefail

ROOT=/home/n0p/src/c62/work/tetra-elk
PROBE=/home/n0p/src/c62/work/dsp-probe
LT=${LT:-/home/n0p/src/c62/build/c62-ws-next3/modules/lib/libtetra}
# Generated tetra_config.h: build-lt-pin has crypto off, build-lt-crypto on
GEN=${GEN:-$ROOT/build-lt-pin/generated}
OUT=$ROOT/build-qemu
V=${1:-dspO2}
CRYPTO=0

case "$V" in
  dspO2)      FLAGS="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -O2" ;;
  dspOs)      FLAGS="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -Os" ;;
  nodspO2)    FLAGS="-mcpu=cortex-m33+nodsp -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -O2" ;;
  dspO2crypto) FLAGS="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -O2"; CRYPTO=1 ;;
  *) echo "unknown variant $V"; exit 1 ;;
esac

mkdir -p "$OUT/lt-$V"
GCC=arm-none-eabi-gcc
INC="-I$GEN -I$LT/include -I$LT/src -I$ROOT/openrtx/include -I$ROOT/tests/qemu"
COMMON="-ffunction-sections -fdata-sections -fno-common -g -std=gnu11"
DEFS=""

SRCS="common.c tmo_rx.c l1/burst.c l1/l1.c l2/coding.c l2/conv.c l2/lmac.c l2/speech.c l2/tmo_mac.c l2/tmo_ul_mac.c l2/dmo_mac.c l3/sds.c l3/lip.c l3/tmo_l3.c l3/tmo_mle.c l3/dmo_cc.c dmo_rx.c util/bits.c"
[ "$CRYPTO" = 1 ] && SRCS="$SRCS crypto/tea.c crypto/tea1.c crypto/tea2.c crypto/tea3.c crypto/tb5.c crypto/ta61.c crypto/hurdle.c crypto/taa1.c tmo_crypto.c"

OBJS=""
for s in $SRCS; do
  o="$OUT/lt-$V/$(echo "$s" | tr '/' '_').o"
  [ -f "$o" ] || $GCC $FLAGS $COMMON $DEFS $INC -c "$LT/src/$s" -o "$o" || exit 1
  OBJS="$OBJS $o"
done

for f in "$ROOT/tests/qemu/tetra_rx_cost.c" "$ROOT/openrtx/src/protocols/TETRA/TetraDemod.c"; do
  b=$(basename "$f" .c)
  $GCC $FLAGS $COMMON $INC -c "$f" -o "$OUT/$b-$V.o" || exit 1
  OBJS="$OBJS $OUT/$b-$V.o"
done
$GCC $FLAGS $COMMON -c "$PROBE/common/startup.c" -o "$OUT/startup.o" || exit 1

$GCC $FLAGS -Wl,--gc-sections -T "$PROBE/common/mps2.ld" -specs=rdimon.specs \
     -o "$OUT/rxcost-$V.elf" "$OUT/startup.o" $OBJS -lrdimon -lm -lc 2>&1 |
    grep -E "error|undefined" | head -10

MK=$(arm-none-eabi-nm "$OUT/rxcost-$V.elf" | awk '$3=="frame_marker"{print "0x"$1}')
echo "variant $V, frame_marker at $MK"
INSNHIST_MARKER=$MK INSNHIST_OUT="$OUT/prof-$V.txt" \
  qemu-system-arm -M mps2-an505 -nographic \
  -semihosting-config enable=on,target=native \
  -kernel "$OUT/rxcost-$V.elf" \
  -plugin file=$PROBE/common/libinsnframe.so -d plugin 2>&1 | tail -2
echo "profile: $OUT/prof-$V.txt"
