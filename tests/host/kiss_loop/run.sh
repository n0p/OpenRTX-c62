#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run the packet modem bench: the KISS codec, the AFSK 1200 and
# G3RUH 9600 engines in loopback, the bit error rate against noise, and, when
# direwolf is available, the cross-check on both rates.
#
#   tests/host/kiss_loop/run.sh                 (from the repository root)
#
# The direwolf part needs its tools; point DIREWOLF at the build's src
# directory (atest, gen_packets) or leave it unset and only the in-tree checks
# run:
#
#   DIREWOLF=~/src/direwolf/build/src tests/host/kiss_loop/run.sh
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out="$here/build"
mkdir -p "$out"
cd "$out"

CC=${CC:-cc}
CFLAGS="-std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -I $root/openrtx/include"

echo "== building"
$CC $CFLAGS -o kiss_test \
    "$here/main.c" "$here/wav.c" \
    "$root/openrtx/src/protocols/KISS/kiss.c" \
    "$root/openrtx/src/protocols/KISS/afsk9600.c" \
    "$root/openrtx/src/protocols/KISS/packet_modem.c" \
    "$root/openrtx/src/protocols/APRS/ax25.c" \
    "$root/openrtx/src/protocols/APRS/hdlc.c" \
    "$root/openrtx/src/protocols/APRS/afsk.c" \
    -lm

echo
echo "== selftest: KISS codec and both modems in loopback"
./kiss_test selftest

echo
echo "== loopback: full frames of 20, 64, 200 and 256 octets"
./kiss_test loop --modem 1200
./kiss_test loop --modem 9600

echo
echo "== bit error rate against noise"
echo "-- AFSK 1200"
./kiss_test ber --modem 1200 --from -12 --to 2 --step 2 --bits 20000
echo "-- G3RUH 9600"
./kiss_test ber --modem 9600 --from 0 --to 16 --step 2 --bits 20000

echo
echo "== WAV files for direwolf"
./kiss_test wav "$out/kiss1200.wav" 1200 \
    "The quick brown fox jumps over the lazy dog 1200" \
    "Packet modem loopback, second frame, 1200 baud"
./kiss_test wav "$out/kiss9600.wav" 9600 \
    "The quick brown fox jumps over the lazy dog 9600" \
    "Packet modem loopback, second frame, 9600 baud"

dw=${DIREWOLF:-}
if [ -z "$dw" ]; then
    for cand in "$root/../aprs/thirdparty/direwolf-master/build/src" \
                "$HOME/src/direwolf/build/src" \
                /usr/local/bin /usr/bin; do
        if [ -x "$cand/atest" ]; then
            dw="$cand"
            break
        fi
    done
fi

if [ -z "$dw" ]; then
    echo
    echo "== direwolf not found: skipping the cross-check"
    echo "   set DIREWOLF=<build>/src to run it (see docs/KISS.md)"
    exit 0
fi

echo
echo "== direwolf decodes our 1200 audio"
set +e
"$dw/atest" -B 1200 "$out/kiss1200.wav" | tail -6
echo
echo "== direwolf decodes our 9600 audio"
"$dw/atest" -B 9600 "$out/kiss9600.wav" | tail -6

echo
echo "== we decode direwolf's 1200 audio"
"$dw/gen_packets" -r 48000 -o "$out/dw1200.wav" >/dev/null
./kiss_test decode "$out/dw1200.wav" 1200 | tail -3

echo
echo "== we decode direwolf's 9600 audio"
"$dw/gen_packets" -r 48000 -B 9600 -o "$out/dw9600.wav" >/dev/null
./kiss_test decode "$out/dw9600.wav" 9600 | tail -3
set -e
