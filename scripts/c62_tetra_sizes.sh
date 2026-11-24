#!/bin/bash
# FLASH/RAM of the C62 firmware with and without the TETRA receive mode.
# Builds into one west workspace, one build dir per configuration.
set -u
WS=/home/n0p/src/c62/build/c62-ws-next3
LOG=/home/n0p/src/c62/build/tetra_sizes
mkdir -p "$LOG"
cd "$WS" || exit 1
SRC=${1:-/home/n0p/src/c62/work/OpenRTX-integ}
git -C OpenRTX fetch -q "$SRC" "${2:-master}" && git -C OpenRTX reset -q --hard FETCH_HEAD
git -C OpenRTX log -1 --format='build source: %h %s'

build() {
    name=$1; shift
    rm -rf "OpenRTX/build_$name"
    sudo docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work openrtx-c62 \
        c62-build "$@" > "$LOG/$name.log" 2>&1
    echo "rc_$name=$?"
    mv OpenRTX/build_c62 "OpenRTX/build_$name" 2>/dev/null
    grep -A10 'Memory region' "$LOG/$name.log" | tail -12
    ls -l "OpenRTX/build_$name/zephyr/zephyr.bin" 2>/dev/null
}

build tetra_on -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y -DC62_TETRA=ON
build tetra_off -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y -DC62_TETRA=OFF
echo DONE
