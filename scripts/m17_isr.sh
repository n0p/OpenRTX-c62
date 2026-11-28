#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The interrupt-context rule, reproduced on the kernel the radio runs.
#
#   scripts/m17_isr.sh [variant ...]
#
# Variants (default: all of them):
#
#   mutation   injects the fault: a function reachable from an interrupt takes
#              a mutex.  Must abort with the radio's own message, and must say
#              which function took it.  This is what makes the harness
#              believable: it can see the bug.
#   m17_path   calls the firmware's own audioPath_request(SOURCE_MIC, SINK_MCU,
#              PRIO_TX) -- the first thing OpMode_M17::txState() does when the
#              PTT is pressed -- from an interrupt instead of from the RTX
#              thread.  If the transmit path were running in interrupt context,
#              this is the abort it would produce.
#   thread     the control: the same call from a thread.  Must not abort.
#   printk     a log line written from an interrupt.  Must not abort with
#              CONFIG_LOG_MODE_DEFERRED and the log thread.
#   panic      k_panic() from an interrupt: the fatal path logs, and what the
#              console then shows is the shape of the radio's own fault dump.
#
# Each run is a Zephyr image for the Cortex-M33 the radio uses (mps2_an521 in
# QEMU, the listenai Zephyr 3.4 fork, the same kernel source), and the
# interrupt is a real one: an NVIC line pended with NVIC_SetPendingIRQ(), so the
# entry goes through the vector table exactly as the radio's SPI and DSP
# mailbox interrupts do.  VARIANTS with --timer use a k_timer expiry instead,
# which is the other interrupt context the firmware uses.
#
# Method: the kernel's k_mutex_lock() is interposed (-Wl,--wrap) so a lock
# asked for in interrupt context prints the return address of the caller; the
# script resolves that address with arm-none-eabi-addr2line against the very
# ELF QEMU ran, so "which function" is answered by the binary.
#
# Requirements: docker image openrtx-c62 (scripts/c62/Dockerfile),
# qemu-system-arm, arm-none-eabi-addr2line, a west workspace with
# zephyr-listenai next to this checkout (WEST by default is the one the last
# firmware build used).
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WEST=${WEST:-/home/n0p/src/c62/build/c62-ws-codec}
ZEPHYR=${ZEPHYR:-$WEST/zephyr-listenai}
MODULES=${MODULES:-$WEST/modules/hal/cmsis}
OUT=${OUT:-$ROOT/build-qemu-isr}
BOARD=${BOARD:-mps2_an521}
QEMU_TIMEOUT=${QEMU_TIMEOUT:-40}

VARIANTS=${*:-"mutation thread m17_path printk panic"}

[ -d "$ZEPHYR" ] || { echo "no Zephyr tree at $ZEPHYR (set WEST=)"; exit 1; }
[ -f "$MODULES/CMSIS/Core/Include/core_cm33.h" ] || \
    { echo "no CMSIS module at $MODULES (set MODULES=)"; exit 1; }

mkdir -p "$OUT"

build() {
    local name=$1 trigger=$2
    local dir="$OUT/$name-${trigger}"
    local extra=""
    [ "$trigger" = timer ] && extra="-DTEST_TRIGGER=timer"

    docker run --rm --user "$(id -u):$(id -g)" \
        -v "$ROOT":/src:ro \
        -v "$WEST":/zephyr-tree \
        -v "$OUT":/out \
        openrtx-c62 bash -lc "
            export ZEPHYR_BASE=/zephyr-tree/zephyr-listenai
            export ZEPHYR_MODULES=/zephyr-tree/modules/hal/cmsis
            rm -rf /out/$name-$trigger
            cmake -GNinja -S /src/tests/qemu/m17_isr_app -B /out/$name-$trigger \
                  -DBOARD=$BOARD -DTEST_NAME=$name $extra >/out/$name-$trigger.cfg 2>&1 &&
            ninja -C /out/$name-$trigger >/out/$name-$trigger.build 2>&1" \
        >/dev/null 2>&1

    if [ ! -f "$dir/zephyr/zephyr.elf" ]; then
        echo "  $name: BUILD FAILED, see $dir.cfg and $dir.build"
        grep -E "error|Error" "$dir.cfg" "$OUT"/*.build 2>/dev/null | head -5
        return 1
    fi
    return 0
}

# Resolve an address to "function (file:line)", outermost frame first: the
# innermost frames are the inlined syscall wrappers inside the caller.
resolve() {
    local elf=$1 addr=$2
    arm-none-eabi-addr2line -f -C -i -e "$elf" "$addr" 2>/dev/null |
        paste - - | awk -F'\t' '{f[NR]=$1; l[NR]=$2} END {
            if (NR == 0) { print "?"; exit }
            printf "%s (%s)", f[NR], l[NR] }'
}

run_variant() {
    local name=$1 trigger=${2:-irq}
    local dir="$OUT/$name-$trigger"
    local log="$dir/run.log"

    build "$name" "$trigger" || return 1

    local t="$QEMU_TIMEOUT"
    # The panic variant halts the kernel on purpose: nothing will exit it.
    [ "$name" = panic ] && t=10

    timeout "$t" qemu-system-arm -M mps2-an521 -nographic \
        -semihosting-config enable=on,target=native \
        -kernel "$dir/zephyr/zephyr.elf" >"$log" 2>&1
    local rc=$?

    local assert_line mutex_line caller isr verdict
    assert_line=$(grep -m1 "^HARNESS assert at" "$log" | sed 's/^HARNESS assert at //')
    mutex_line=$(grep -m1 "^HARNESS mutex-in-interrupt" "$log" | wc -l)
    caller=$(grep -m1 "^HARNESS caller" "$log" | awk '{print $3}')
    isr=$(sed -n '/^HARNESS mutex-in-interrupt$/{n;p;}' "$log" | sed 's/^HARNESS //')
    verdict=$(grep -m1 "^HARNESS VERDICT" "$log" | sed 's/^HARNESS //')

    echo "== $name ($trigger), exit $rc"
    tr -d '\r' <"$log" | awk '!seen[$0]++' | sed 's/^HARNESS /   /' |
        grep -v "^   caller\|^   mutex 0x" | head -24

    if [ -n "$caller" ]; then
        echo "   caller $caller = $(resolve "$dir/zephyr/zephyr.elf" "$caller")"
        [ -n "$isr" ] && echo "   interrupt entry: $isr"
    fi

    case "$name" in
        mutation|m17_path)
            if [ "$assert_line" = "WEST_TOPDIR/zephyr-listenai/kernel/mutex.c:101" ] ||
               echo "$assert_line" | grep -q "kernel/mutex.c:101"; then
                echo "   RESULT reproduced: mutex taken in interrupt context"
            else
                echo "   RESULT NOT reproduced (assert='$assert_line')"
            fi ;;
        thread|printk)
            if [ -z "$assert_line" ]; then
                echo "   RESULT ok: no assertion"
            else
                echo "   RESULT unexpected assertion at $assert_line"
            fi ;;
        panic)
            echo "   RESULT panic from an interrupt: see the lines above"
            ;;
    esac
    echo
}

for v in $VARIANTS; do
    run_variant "$v" irq
done

if [ "${WITH_TIMER:-0}" = 1 ]; then
    for v in $VARIANTS; do
        run_variant "$v" timer
    done
fi

echo "builds and logs: $OUT"
