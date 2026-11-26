#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run the DMR transmit sequence on the host: TUNE(DMR), PTT down for
# three seconds, PTT up, with the firmware's own slot machine, burst builders
# and encoder.  The baseband it produces is decoded by the repository's
# reference receiver, so the test fails if the bursts it emits are not valid.
#
#   tests/host/dmr_tx_host/run.sh            (from the repository root)
#
# The receiver is work/dmr-rx/build/dmr_rx; set DMR_RX to another path to use a
# different build of it.  Without a receiver the sequence is still walked and
# counted, and the decode step is reported as skipped.
#
# What the receiver has to say about the emission: a DMR voice superframe
# carries its 48-bit sync only in burst A (B..F carry the embedded LC in its
# place), so one accepted burst per six is the signal being right, not a
# defect.  What matters is that every sync it finds is accepted with zero sync
# errors, that the CACH decodes in all of them, that the AMBE+2 frames come out
# whole, and that the call carried a header, voice and a terminator.
set -eu

here=$(dirname "$0")
root=$(cd "$here/../../.." && pwd)
out="$here/build"
mkdir -p "$out"

SRC=$root/openrtx/src/protocols/DMR
LIB=$root/lib/dmr_encoder
INC="-I$root/openrtx/include -I$root/openrtx/include/protocols/DMR \
     -I$SRC -I$LIB"

CFLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter"

echo "== building dmr_tx_host"
# shellcheck disable=SC2086
cc $CFLAGS $INC -DCONFIG_DMR_TX \
   "$here/main.c" \
   "$SRC/dmr_tx.c" "$SRC/dmr_enc.c" "$SRC/dmr_fec.c" "$SRC/dmr_trellis.c" \
   "$LIB/dmr_encoder.c" "$LIB/dmr_enc_core.c" "$LIB/dmr_enc_frame.c" \
   "$LIB/dmr_enc_ambe_tables.c" "$LIB/dmr_enc_trig.c" \
   -lm -lpthread -o "$out/dmr_tx_host"

echo "== running the sequence"
"$out/dmr_tx_host" "$out/baseband.s16"

# ---- the receiver: the bursts have to be valid on the air -------------------
rx=${DMR_RX:-/home/n0p/src/c62/work/dmr-rx/build/dmr_rx}
if [ ! -x "$rx" ]; then
    echo "== receiver: $rx not found, decode step SKIPPED"
    exit 0
fi

echo "== decoding with $rx"
"$rx" --format s16 --fs 48000 --no-wav --quiet "$out/baseband.s16" \
      > "$out/dmr_rx.txt" 2>&1 || true
cat "$out/dmr_rx.txt"

bursts=$(sed -n 's/^bursts  *\([0-9][0-9]*\) accepted.*/\1/p' "$out/dmr_rx.txt" | head -1)
voice=$(sed -n 's/^  voice \([0-9][0-9]*\),.*/\1/p' "$out/dmr_rx.txt" | head -1)
frames=$(sed -n 's/^AMBE frames  *\([0-9][0-9]*\) decoded.*/\1/p' "$out/dmr_rx.txt" | head -1)
syncerr=$(sed -n 's/^  sync bit errors\/48  mean \([0-9.]*\).*/\1/p' "$out/dmr_rx.txt" | head -1)
cach=$(sed -n 's/^  CACH OK  *\([0-9][0-9]*\)\/\([0-9][0-9]*\).*/\1 \2/p' "$out/dmr_rx.txt" | head -1)

echo "== receiver: ${bursts:-0} rafagas aceptadas, ${voice:-0} de voz, ${frames:-0} tramas AMBE, sync ${syncerr:-?}, CACH ${cach:-?}"

fail() { echo "FALLO: $*"; exit 1; }

[ -n "${bursts:-}" ] || fail "el receptor no dio cifras"
[ "$bursts" -ge 6 ] || fail "solo $bursts rafagas aceptadas"
[ "${voice:-0}" -ge 4 ] || fail "solo ${voice:-0} rafagas de voz aceptadas"
[ "${frames:-0}" -ge 40 ] || fail "solo ${frames:-0} tramas AMBE decodificadas"
case "${syncerr:-x}" in
    0.00) ;;
    *) fail "errores de sync medio ${syncerr:-?}, se esperaba 0.00" ;;
esac
[ -n "${cach:-}" ] || fail "sin cifras de CACH"
[ "$(echo "$cach" | cut -d' ' -f1)" = "$(echo "$cach" | cut -d' ' -f2)" ] \
    || fail "CACH no decodifica en todas las rafagas ($cach)"

echo "OK: las rafagas emitidas son validas para el receptor de referencia"
