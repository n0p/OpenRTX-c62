#!/bin/bash
# Measure flash/RAM/PSRAM of the P25 build with and without the vocoder.
set -u
cd /home/n0p/src/c62/work/imbe || exit 1
WS=/home/n0p/src/c62/build/c62-ws
run() { # $1 tag, rest: cmake options
    tag=$1; shift
    rm -rf build_c62
    sudo docker run --rm --user "$(id -u):$(id -g)" -v "$WS":/work -v "$PWD":/work/OpenRTX \
        openrtx-c62 c62-build "$@" > "/tmp/build_$tag.log" 2>&1
    rc=$?
    printf '%s rc=%s ' "$tag" "$rc"
    if [ $rc -ne 0 ]; then echo FAILED; return; fi
    cp -a build_c62 "build_$tag"
    python3 - "$tag" <<'EOF'
import re, sys
tag = sys.argv[1]
mapf = f"build_{tag}/zephyr/zephyr.map"
txt = open(mapf).read()
m = re.search(r"__rom_region_start = \.(\s*)(0x[0-9a-f]+)", txt)
def sym(name):
    mm = re.search(r"\b" + name + r" = \.(\s*)(0x[0-9a-f]+)", txt)
    return int(mm.group(2), 16) if mm else 0
flash = sym("__rom_region_end") - sym("__rom_region_start")
ram = sym("_image_ram_end")
psram = sym("__psram_section_end") - sym("__psram_section_start")
print(f"flash={flash} ram_end=0x{ram:x} psram={psram}")
EOF
}
run p25 -DCONFIG_C62_P25=y
run p25_novoc -DCONFIG_C62_P25=y -DCONFIG_C62_P25_VOCODER=n
run mega -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y -DCONFIG_C62_P25=y \
    -DC62_TETRA=ON -DC62_SCAN=ON -DC62_FUSION=ON -DC62_WFM=ON
run mega_novoc -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y -DCONFIG_C62_P25=y \
    -DC62_TETRA=ON -DC62_SCAN=ON -DC62_FUSION=ON -DC62_WFM=ON \
    -DCONFIG_C62_P25_VOCODER=n
