#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run the transmit hang harness: the real audio stream code and the
# real transmit guard against a stub sink that never consumes.
#
#   tests/host/tx_lifecycle_host/run.sh        (from the repository root)
#
set -eu

here=$(dirname "$0")
root=$(cd "$here/../../.." && pwd)
out="$here/build"
mkdir -p "$out"

cc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
   -I "$root/openrtx/include" \
   "$here/main.c" \
   "$root/openrtx/src/core/audio_stream.c" \
   "$root/openrtx/src/rtx/TxGuard.c" \
   -lpthread -o "$out/tx_lifecycle_host"

exec "$out/tx_lifecycle_host"
