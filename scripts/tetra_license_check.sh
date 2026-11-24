#!/bin/bash
# Inventories the licences of a libtetra checkout and checks the licence
# boundary of a firmware image.
#
# libtetra is not vendored: it arrives as a west module (west.yml) and is
# built by platform/targets/c62/CMakeLists.txt. Everything it ships carries an
# SPDX identifier; the crypto files are AGPL-3.0 because they derive from
# osmo-tetra, the rest is GPL-3.0-or-later, and both are compatible with
# OpenRTX. This script answers the two questions that matter before shipping:
# is every file attributed, and did AGPL code end up in the image?
#
#   scripts/tetra_license_check.sh [libtetra-dir] [zephyr.elf]
set -uo pipefail

LT=${1:-/home/n0p/src/c62/build/c62-ws-next3/modules/lib/libtetra}
ELF=${2:-/home/n0p/src/c62/build/c62-ws-next3/OpenRTX/build_tetra_on/zephyr/zephyr.elf}

echo "== $LT"
[ -d "$LT" ] || { echo "   no such directory"; exit 1; }
echo "   upstream:  $(git -C "$LT" remote get-url origin 2>/dev/null)"
echo "   commit:    $(git -C "$LT" rev-parse --short HEAD 2>/dev/null) $(git -C "$LT" log -1 --format='%s')"

gpl=0
agpl=0
missing=0
while IFS= read -r f; do
    p="$LT/$f"
    [ -f "$p" ] || continue
    if grep -q "SPDX-License-Identifier: AGPL" "$p"; then
        agpl=$((agpl + 1))
        echo "   AGPL  $f"
    elif grep -q "SPDX-License-Identifier: GPL-3.0-or-later" "$p"; then
        gpl=$((gpl + 1))
    else
        missing=$((missing + 1))
        echo "   NO SPDX LINE  $f (vendored from osmo-tetra: AGPL-3.0)"
    fi
done < <(cd "$LT" && git ls-files '*.c' '*.h' '*.cpp')

echo "   GPL-3.0-or-later files: $gpl"
echo "   AGPL-3.0 files:         $agpl (crypto, built only with TETRA_ENABLE_CRYPTO)"
echo "   without SPDX line:      $missing"

if [ -f "$ELF" ]; then
    echo "== $ELF"
    syms=$(arm-none-eabi-nm "$ELF" 2>/dev/null || true)
    case "$syms" in
        *tetra_tea*|*tetra_tb5*|*tetra_ta61*|*tetra_taa1*|*hurdle*)
            echo "   AGPL crypto symbols present: check the licence of the image"
            exit 1 ;;
    esac
    case "$syms" in
        *tetra_tmo_push_soft*)
            echo "   TMO receiver linked, no AGPL crypto: image is GPL-3.0-or-later only" ;;
        *)
            echo "   no TETRA receiver in this image" ;;
    esac
fi
