#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Check that the per-mode statistics layer costs nothing when it is off.
#
# tests/unit/mode_stats_off.c calls every hook of the layer with
# CONFIG_C62_MODE_STATS undefined. Compiled, that probe must be:
#   - free of any reference to modeStats_* (nothing to link, nothing to call)
#   - nothing but a return instruction (no arithmetic, no memory access)
#
# Usage: mode_stats_off.sh <source root> <toolchain prefix>
#
# Exits non-zero, with what it found, when either is false.

set -eu

root=${1:?source root}
cross=${2:-}

cc="${cross}gcc"
objdump="${cross}objdump"
nm="${cross}nm"

if ! command -v "$cc" >/dev/null 2>&1; then
    cc=cc
    objdump=objdump
    nm=nm
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The probe: -O2, the optimisation the firmware is built with
"$cc" -std=c99 -O2 -Wall -Wextra -I"$root/openrtx/include" \
    -c "$root/tests/unit/mode_stats_off.c" -o "$tmp/probe.o"

fail=0

# 1) No symbol of the layer may be referenced
refs=$("$nm" -u "$tmp/probe.o" | grep -c 'modeStats_' || true)
if [ "$refs" -ne 0 ]; then
    echo "FAIL: the disabled hooks reference $refs modeStats_* symbols:"
    "$nm" -u "$tmp/probe.o" | grep 'modeStats_' || true
    fail=1
fi

# 2) The function must be a bare return: endbr64 (x86-64 with CET) and ret,
#    or a single branch on the ARM targets.
body=$("$objdump" -d --no-show-raw-insn "$tmp/probe.o" |
    sed -n '/<probe>:/,/^$/p' |
    grep -E '^[[:space:]]+[0-9a-f]+:' |
    sed -E 's/^[[:space:]]+[0-9a-f]+:[[:space:]]*//' |
    awk '{print $1}')

extra=$(printf '%s\n' "$body" |
    grep -v -E '^(retq?|endbr64|bx|nop|\.\.\.)$' | grep -c . || true)

if [ "$extra" -ne 0 ]; then
    echo "FAIL: the disabled hooks are not free, the probe compiles to:"
    "$objdump" -d --no-show-raw-insn "$tmp/probe.o" | sed -n '/<probe>:/,/^$/p'
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "OK: disabled hooks cost nothing (instructions: $(printf '%s' "$body" | tr '\n' ' '))"
fi

exit "$fail"
