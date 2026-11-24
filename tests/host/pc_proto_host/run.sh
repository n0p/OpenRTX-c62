#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the host "radio" of the PC protocol and run the pty test against it.
#
#   tests/host/pc_proto_host/run.sh          (from the repository root)
#
# Two binaries come out: pc_proto_device (the protocol with the debug
# namespace, as a bench build has it) and pc_proto_device_nodebug (a production
# build: the namespace compiled out).  The Python test drives both through a
# pty, so the client under test is the one that ships in tools/cps.
set -eu

here=$(dirname "$0")
root=$(cd "$here/../../.." && pwd)
out="$here/build"
mkdir -p "$out"

INC="-I$root/openrtx/include -I$root/platform -I$root/platform/targets/linux"
CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter"

# The protocol and the stores it touches: the real sources, not a mock.  The
# platform files come in for `state` (the settings handlers read it) and for
# the linker symbols the diagnostic hook does not need here.
SRC="$root/openrtx/src/core/pc_proto.c
     $root/openrtx/src/core/pc_proto_ops.c
     $root/openrtx/src/core/pc_proto_dbg.c
     $root/openrtx/src/core/channel_store.c
     $root/openrtx/src/core/contact_store.c
     $root/openrtx/src/core/slot_store.c
     $root/openrtx/src/core/zones.c
     $root/openrtx/src/core/airband.c
     $root/openrtx/src/core/wfm.c
     $root/openrtx/src/core/settings.c
     $root/openrtx/src/core/crc.c
     $root/openrtx/src/core/messaging.c
     $root/openrtx/src/core/messaging_store_none.c
     $root/tests/host/pc_proto_host/device.c
     $root/tests/host/pc_proto_host/ram_store.c"

# state.c pulls the whole platform in (nvmem, radio); the two symbols the
# protocol uses are provided by a stub instead, so the test needs no SDL.
STUB="$root/tests/host/pc_proto_host/state_stub.c"

echo "building pc_proto_device"
# shellcheck disable=SC2086
cc $CFLAGS $INC -DCONFIG_C62_DEBUG_PROTO -DTOOLCHAIN_HOST \
   $SRC "$STUB" -lpthread -o "$out/pc_proto_device"

echo "building pc_proto_device_nodebug"
# shellcheck disable=SC2086
cc $CFLAGS $INC -DTOOLCHAIN_HOST \
   $SRC "$STUB" -lpthread -o "$out/pc_proto_device_nodebug"

echo "running the pty test"
PYTHONPATH="$root/tools/cps" exec python3 "$root/tools/cps/tests/test_pcproto.py" \
     --device "$out/pc_proto_device" \
     --device-nodebug "$out/pc_proto_device_nodebug"
