#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run the firmware DMR PHY over a clean capture and over the distorted variants
# distort.py makes from it, and print one summary line per variant.
#
#   ./run.sh /tmp/dmrhw/dmr_t3_cc.wav
#
# What to look for: the row that matches what the radio reports is the
# distortion its audio path is adding.

set -e
HERE=$(dirname "$0")
[ -x "$HERE/build/dmr_phy_host" ] || make -C "$HERE" >/dev/null

SRC=${1:-/tmp/dmrhw/dmr_t3_cc.wav}
OUT=${2:-/tmp/dmrhw/dist}

python3 "$HERE/distort.py" "$SRC" "$OUT" >/dev/null

printf '%-10s %6s %6s %6s %6s %8s %s\n' variant bursts cach slot crc "meanEVM" "colour codes"
for f in "$SRC" "$OUT"/*.wav; do
    name=$(basename "$f" .wav)
    out=$("$HERE/build/dmr_phy_host" "$f")
    bursts=$(printf '%s\n' "$out" | sed -n 's/^\([0-9]*\) bursts:.*/\1/p')
    cach=$(printf '%s\n' "$out" | sed -n 's/.*| CACH \([0-9]*\) |.*/\1/p')
    slot=$(printf '%s\n' "$out" | sed -n 's/.*| slot-type \([0-9]*\) |.*/\1/p')
    crc=$(printf '%s\n' "$out" | sed -n 's/.*| CRC16 \([0-9]*\/[0-9]*\).*/\1/p')
    evm=$(printf '%s\n' "$out" | sed -n 's/.*evm=\([0-9.]*\).*/\1/p' \
          | awk '{s+=$1; n++} END {if (n) printf "%.3f", s/n; else print "-"}')
    cc=$(printf '%s\n' "$out" | sed -n 's/.*cc= *\([0-9]*\) .*/\1/p' \
         | sort -n | uniq -c | tr '\n' ' ' | sed 's/  */ /g')
    printf '%-10s %6s %6s %6s %6s %8s %s\n' \
        "$name" "${bursts:--}" "${cach:--}" "${slot:--}" "${crc:--}" "$evm" "$cc"
done
