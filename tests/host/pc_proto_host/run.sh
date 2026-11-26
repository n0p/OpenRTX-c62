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
# A directory of its own per run: two of these (meson runs tests in parallel,
# and a developer may have one going) used to share tests/host/pc_proto_host/
# build and rebuild each other's binaries under the pty test's feet.
out=$(mktemp -d "${TMPDIR:-/tmp}/pc-proto-host.XXXXXX")
trap 'rm -rf "$out"' EXIT

INC="-I$root/openrtx/include -I$root/platform -I$root/platform/targets/linux"
CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter"

# The protocol and the stores it touches: the real sources, not a mock.  The
# platform files come in for `state` (the settings handlers read it) and for
# the linker symbols the diagnostic hook does not need here.
SRC="$root/openrtx/src/core/pc_proto.c
     $root/openrtx/src/core/pc_proto_ops.c
     $root/openrtx/src/core/pc_proto_dbg.c
     $root/openrtx/src/core/pc_proto_bench.c
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
   $SRC "$STUB" -lpthread -lm -o "$out/pc_proto_device"

echo "building pc_proto_device_nodebug"
# shellcheck disable=SC2086
cc $CFLAGS $INC -DTOOLCHAIN_HOST \
   $SRC "$STUB" -lpthread -lm -o "$out/pc_proto_device_nodebug"

# The measurement build: debug plus the bench namespace.  Same sources, one
# more define; docs/BENCH.md is what it is for.
echo "building pc_proto_device_bench"
# shellcheck disable=SC2086
cc $CFLAGS $INC -DCONFIG_C62_DEBUG_PROTO -DCONFIG_C62_BENCH_PROTO \
   -DTOOLCHAIN_HOST $SRC "$STUB" -lpthread -lm -o "$out/pc_proto_device_bench"

# Both suites run even when one of them fails: the status at the end is the
# worst of the two, and a broken CPS expectation must not hide a broken bench.
status=0

echo "running the pty test"
PYTHONPATH="$root/tools/cps" python3 "$root/tools/cps/tests/test_pcproto.py" \
     --device "$out/pc_proto_device" \
     --device-nodebug "$out/pc_proto_device_nodebug" || status=1

echo "running the bench pty test"
PYTHONPATH="$root/tools/cps:$root/tools/bench" python3 \
     "$root/tools/bench/tests/test_bench_proto.py" \
     --device "$out/pc_proto_device_bench" \
     --device-nodebug "$out/pc_proto_device_nodebug" || status=1

echo "running the correlator test"
python3 "$root/tools/bench/tests/test_correlate.py" || status=1

echo "running the bench end to end, simulated"
python3 "$root/tools/bench/tests/test_bench_e2e.py" || status=1

exit $status
