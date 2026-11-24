#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the host harness of the DMR protocol package.
#
#   tests/host/dmr_pkg_host/build.sh [own | mbelib dir]
#
#   (nothing)    metadata only: no voice, no PCM
#   own          voice from lib/dmr_vocoder (what the firmware builds)
#   mbelib dir   voice from mbelib (the package's choice); the figures in
#                docs/DMR_PKG.md were taken with lwvmobile/mbelib @34adf9f
#
# scripts/c62/fetch_mbelib.sh puts mbelib in <west>/modules/lib/mbelib.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC=$ROOT/openrtx/src/protocols/DMR
INC="-I$ROOT/openrtx/include -I$ROOT/openrtx/include/protocols/DMR -I$ROOT/openrtx/include/protocols/DMR/trunk -I$SRC -I$SRC/trunk"
OUT=${OUT:-$ROOT/build_host}
VOICE=${1:-}
LIBS=""

mkdir -p "$OUT"

# The package's own switches, as the firmware sets them: Tier III feeds off
# dmr_proto, and without CONFIG_DMR_TRUNK the #ifdef there compiles the feed out
DEFS="-DCONFIG_DMR_TRUNK"

case "$VOICE" in
    "")
    echo "voice: off (pass 'own' or a mbelib directory to enable it)"
    ;;
    own)
    # Same sources and per-file flags as lib/dmr_vocoder/CMakeLists.txt
    LIB=$ROOT/lib/dmr_vocoder
    INC="$INC -I$LIB"
    DEFS="$DEFS -DDMR_VOICE_OWN"
    rm -f "$OUT/libdmr_vocoder_host.a"
    for f in dmr_vocoder dmr_decoder dmr_fft; do
        case $f in
            dmr_fft) o="-ffp-contract=fast" ;;
            *)       o="-ffp-contract=off" ;;
        esac
        cc -c -O2 -std=c99 -fno-math-errno $o -I"$LIB" \
           "$LIB/$f.c" -o "$OUT/dv_$f.o"
    done
    ar rcs "$OUT/libdmr_vocoder_host.a" "$OUT"/dv_*.o
    LIBS="$OUT/libdmr_vocoder_host.a"
    echo "voice: lib/dmr_vocoder (ours)"
    ;;
    *)
    MBELIB=$VOICE
    DEFS="$DEFS -DDMR_VOICE_ENABLED -I$MBELIB"
    LIBS="$OUT/mbelib.a"
    rm -f "$LIBS"
    for f in "$MBELIB"/*.c; do
        o=$OUT/mb_$(basename "$f" .c).o
        cc -c -O2 -w -I"$MBELIB" "$f" -o "$o"
    done
    ar rcs "$LIBS" "$OUT"/mb_*.o
    echo "voice: mbelib from $MBELIB"
    ;;
esac

# The Tier III probe: what the engine does with a PDU the standard describes,
# next to what it does with the vendor CSBKs the control-channel capture holds
cc -O2 -g -std=gnu11 -Wall -Wextra $INC $DEFS \
   "$HERE/t3_probe.c" "$SRC/dmr_glue.c" \
   "$SRC/trunk/dmr_trunk.c" "$SRC/trunk/t3_ms.c" "$SRC/trunk/t3_pdu.c" \
   "$SRC/trunk/t3_plan.c" "$SRC/trunk/t3_sys.c" \
   -lm -o "$OUT/dmr_t3_probe"

cc -O2 -g -std=gnu11 -Wall -Wextra $INC $DEFS \
   "$HERE/main.c" \
   "$SRC/DmrDefs.c" "$SRC/DmrStatus.c" "$SRC/dmr_glue.c" \
   "$SRC/dmr_demod.c" "$SRC/dmr_fec.c" "$SRC/dmr_trellis.c" \
   "$SRC/dmr_proto.c" "$SRC/dmr_data.c" "$SRC/dmr_lrrp.c" \
   "$SRC/dmr_track.c" "$SRC/dmr_voice.c" "$SRC/dmr_sms.c" \
   "$SRC/dmr_ep.c" "$SRC/dmr_aes.c" "$SRC/dmr_hytera.c" \
   "$SRC/aes/tinyaes.c" "$SRC/aes/tinyaes128.c" "$SRC/aes/des.c" \
   "$SRC/trunk/dmr_trunk.c" "$SRC/trunk/t3_ms.c" "$SRC/trunk/t3_pdu.c" \
   "$SRC/trunk/t3_plan.c" "$SRC/trunk/t3_sys.c" \
   $LIBS -lm -lpthread -o "$OUT/dmr_pkg_host"

echo "built $OUT/dmr_pkg_host and $OUT/dmr_t3_probe"
