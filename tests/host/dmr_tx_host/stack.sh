#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The measurement behind RTX_THREAD_STKSIZE (openrtx/include/core/threads.h) and
# tests/unit/rtx_stack_budget.cpp: how deep the DMR transmitter goes inside the
# radio thread, in frames, on the core it runs on.
#
#   tests/host/dmr_tx_host/stack.sh            (from the repository root)
#
# The chain, from the mode's update() to the deepest function:
#
#   update() -> txSlot() -> fillMic() -> dmrEnc_encode() -> dmr_encoder_encode()
#     -> dmr_enc_frame() -> enc_ambe() -> e_p() -> pitch_est()
#
# Everything below dmrEnc_encode() is in lib/dmr_encoder and builds without
# Zephyr, so -fstack-usage works here even though the firmware image does not
# build outside the vendor SDK.  The frames around the chain -- dmr_tx_voice()
# 520 B and the 816 B a modulator slot keeps live -- are the two numbers the
# comment in threads.h adds to this sum; they are printed here so the total can
# be compared with what ships.
#
# Exit status is 0 when the measured chain plus those buffers still fits the
# stack the radio ships, 1 when it does not: this is the same rule the static
# assert enforces, measured instead of asserted.
set -eu

here=$(dirname "$0")
root=$(cd "$here/../../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/dmr-stack.XXXXXX")
trap 'rm -rf "$out"' EXIT

CC=${CC:-arm-none-eabi-gcc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "== no $CC: the ARM measurement is SKIPPED"
    exit 0
fi

# The core the radio runs: Cortex-M33 with the single-precision FPU, at -Os,
# which is what the C62 build uses.
FLAGS="-std=gnu11 -Os -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
       -mfloat-abi=hard -fstack-usage -w \
       -I$root/openrtx/include -I$root/openrtx/include/protocols/DMR \
       -I$root/openrtx/src/protocols/DMR -I$root/lib/dmr_encoder"

echo "== compiling the encoder chain for Cortex-M33 (-Os)"
# shellcheck disable=SC2086
for f in lib/dmr_encoder/dmr_encoder.c \
         lib/dmr_encoder/dmr_enc_frame.c \
         lib/dmr_encoder/dmr_enc_core.c \
         lib/dmr_encoder/dmr_enc_ambe_tables.c \
         lib/dmr_encoder/dmr_enc_trig.c \
         openrtx/src/protocols/DMR/dmr_enc.c; do
    # shellcheck disable=SC2086
    $CC $FLAGS -c "$root/$f" -o "$out/$(basename "$f" .c).o"
done

# Frame of one function, in bytes, from the .su files.  0 if absent.
frame_of() {
    name=$1
    bytes=$(awk -F'\t' -v f="$name" '$1 ~ ":" f "$" { print $2 }' "$out"/*.su \
            | sort -nr | head -1)
    echo "${bytes:-0}"
}

sum=0
echo "== the chain, deepest last"
for f in dmrEnc_encode dmr_encoder_encode dmr_enc_frame enc_ambe e_p \
         'pitch_est.constprop'; do
    b=$(frame_of "$f")
    [ "$b" = "0" ] && b=$(frame_of "${f%%.constprop}")
    printf '   %-24s %5s B\n' "$f" "$b"
    sum=$((sum + b))
done

echo "   -----------------------------------------"
printf '   %-24s %5s B\n' "chain" "$sum"
printf '   %-24s %5s B\n' "dmr_tx_voice + slot" "$((520 + 816))"
total=$((sum + 520 + 816))
printf '   %-24s %5s B\n' "total" "$total"

# What ships, read from the header the build uses: the same number the static
# assert is written against.
shipped=$(sed -n 's/^#define RTX_THREAD_STKSIZE *\([0-9][0-9]*\)/\1/p' \
          "$root/openrtx/include/core/threads.h" | sort -nr | head -1)
echo
echo "   measured $total B, the C62 ships ${shipped:-0} B"

if [ -z "$shipped" ]; then
    echo "== could not read RTX_THREAD_STKSIZE: FAILED"
    exit 1
fi

if [ "$total" -gt "$shipped" ]; then
    echo "== the chain does not fit the rtx stack: FAILED"
    exit 1
fi

# A stack that only just fits is the failure this measurement exists for: the
# 4 KiB branch that the bench image took left no room at all for the frames
# below the ones counted here.  A kilobyte spare is one more deep function
# before the guard page; less than that is a warning, not a failure.
if [ $((shipped - total)) -lt 1024 ]; then
    echo "== under a kilobyte spare: one more frame and this overflows"
fi

echo "== ok: the encoder chain fits with $((shipped - total)) B to spare"
