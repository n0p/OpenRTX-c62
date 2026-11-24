#!/bin/bash
# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate tests/data/speech_ref.pcm with the *reference* decoder: OpenDMR's
# decoder/ (the mbelib-neo submodule in lib/opendmr) driven by its own decode
# path (ref/ref_dec.c is opendmr.cpp's decode, verbatim) and its CGolay24128.
#
# This is verification material only: nothing here is linked into the firmware.
#
# usage: tools/make_ref.sh [frames.ambe] [out.pcm]
#
# REFSRC points at another checkout of OpenDMR (decoder/, encoder/, the whole
# repository root); by default the submodule of this tree is used.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${REFSRC:-$(cd "$ROOT/../opendmr" && pwd)}
IN=${1:-$ROOT/tests/data/speech.ambe}
OUT=${2:-$ROOT/tests/data/speech_ref.pcm}
[ -f "$IN" ] || { echo "no such frame file: $IN" >&2; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

CF="-O3 -fno-math-errno"
for f in mbelib mbe_adaptive mbe_unvoiced_fft ambe3600x2450 ambe_common ecc ecc_const pffft; do
  cc $CF -w -I"$SRC/decoder" -c "$SRC/decoder/$f.c" -o "$T/$f.o"
done
c++ $CF -I"$SRC/encoder" -c "$SRC/encoder/cgolay24128.cpp" -o "$T/cgolay.o"
cc $CF -I"$SRC/decoder" -c "$ROOT/ref/ref_dec.c" -o "$T/ref_dec.o"
c++ $CF -I"$SRC/encoder" -c "$ROOT/ref/golay_c.cpp" -o "$T/golay_c.o"
c++ -o "$T/ref_dec" "$T"/*.o -lm
"$T/ref_dec" "$IN" "$OUT"
echo "wrote $OUT"
