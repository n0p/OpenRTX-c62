#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the IMBE decoder on the host, decode the two frame corpora with it and
# with the mbelib-neo reference, and compare the PCM.
#
#   ./run_host.sh              # everything
#   MBELIB=/path/to/mbelib-neo ./run_host.sh
#
# Corpora (11 packed parameter bytes per frame, i.e. the air format):
#   tests/data/p25_vc_frames.imbe    261 real frames from an off-air P25 Phase 1
#                                    voice channel (samples/P25-C4FM-VC_IF.wav of
#                                    work/p25-rx, dumped with its --imbe-out and
#                                    converted by tests/frames2bin.py); every
#                                    frame arrived with 0 corrected FEC bits
#   tests/data/speech_frames.imbe    1000 frames from mbelib-neo's own IMBE
#                                    encoder over the codec2 test speech
#                                    (wav/ve9qrp.wav, 8 kHz): a corpus with the
#                                    full dynamic range and every L=9..56
#
# The reference is built twice: the IMBE path with the TIA-102.BABA 7.5 spec
# rendering (the pipeline this decoder implements, so it is the fidelity check)
# and mbe_processImbe4400Data (the DVSI 10 ms subframe rendering, what a P25
# listener of that library hears, so the renderer difference is measured rather
# than hidden).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
LIB=$(cd "$HERE/.." && pwd)
DATA=$HERE/data
MBELIB=${MBELIB:-/home/n0p/src/c62/ref/mbelib-neo}
WORK=${WORK:-$(mktemp -d)}

echo "== work dir: $WORK"
mkdir -p "$WORK"
cd "$WORK"

INC="-I$LIB -I$LIB/../dmr_vocoder"
COMMON="-O2 -std=c99 -fno-math-errno -ffunction-sections -fdata-sections -fno-common"

echo "== host build"
gcc $COMMON -ffp-contract=off $INC -c "$LIB/imbe_decoder.c" -o imbe_decoder.o
gcc $COMMON -ffp-contract=off $INC -c "$LIB/imbe_vocoder.c" -o imbe_vocoder.o
gcc $COMMON -ffp-contract=fast $INC -c "$LIB/imbe_fft.c" -o imbe_fft.o

echo "== reference build ($MBELIB)"
gcc $COMMON -I"$MBELIB/include" -I"$MBELIB/src/internal" -o ref_pcm "$HERE/ref_pcm.c" \
    "$MBELIB/build/libmbe-neo.a" -lm

rc=0
for corpus in "p25_vc_frames:$DATA/p25_vc_frames.imbe" "speech:$DATA/speech_frames.imbe"; do
    name=${corpus%%:*}
    frames=${corpus#*:}
    nframes=$(( $(stat -c %s "$frames") / 11 ))
    echo
    echo "== corpus $name ($nframes frames)"
    gcc $COMMON -DIMBE_TEST_HOST -DITERS="$nframes" $INC -c "$HERE/pcm_dump.c" -o pcm_dump.o
    gcc -o decoder pcm_dump.o imbe_vocoder.o imbe_decoder.o imbe_fft.o -lm
    ./decoder "$frames" "$name.out.pcm" | sed 's/^/   /'
    ./ref_pcm spec "$frames" "$name.ref_spec.pcm" | sed 's/^/   /'
    ./ref_pcm dvsi "$frames" "$name.ref_dvsi.pcm" | sed 's/^/   /'

    echo "   -- fidelity of the port against the reference rendered the same way"
    python3 "$HERE/compare_pcm.py" "$name.ref_spec.pcm" "$name.out.pcm" --min-snr 45 || rc=1
    echo "   -- the reference's two renderings against each other (the yardstick)"
    python3 "$HERE/compare_pcm.py" "$name.ref_spec.pcm" "$name.ref_dvsi.pcm" --min-snr -100 || true
    echo "   -- the port against the DVSI rendering a P25 listener hears"
    python3 "$HERE/compare_pcm.py" "$name.ref_dvsi.pcm" "$name.out.pcm" --min-snr -100 || true
done

echo
echo "== robustness (ASan/UBSan: bit errors and random parameter vectors)"
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
gcc $SAN -std=c99 $INC -o fuzz "$HERE/fuzz.c" \
    "$LIB/imbe_vocoder.c" "$LIB/imbe_decoder.c" "$LIB/imbe_fft.c" -lm
./fuzz "$DATA/p25_vc_frames.imbe" | sed 's/^/   /' || rc=1
./fuzz "$DATA/speech_frames.imbe" | sed 's/^/   /' || rc=1

exit $rc

