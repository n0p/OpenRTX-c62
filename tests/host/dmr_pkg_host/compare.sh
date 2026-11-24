#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A/B of the two DMR receive chains on the same captures:
#   package  = tests/host/dmr_pkg_host  (protocols/DMR/, this tree)
#   baseline = work/dmr-rx/build/dmr_rx (the old DmrPhy chain, same algorithms)
#
#   tests/host/dmr_pkg_host/compare.sh [capture dir] [out dir]
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BASELINE=/home/n0p/src/c62/work/dmr-rx/build/dmr_rx
CAPS=${1:-/home/n0p/src/c62/work/dmr-rx/out}
OUT=${2:-$ROOT/build_host/compare}
PKG=${PKG:-/tmp/dmrhost/dmr_pkg_host}

mkdir -p "$OUT"

for c in discr_dmr_t3_cc discr_dmr_voice discr_dmr_bs_weak discr_sigidwiki; do
    wav=$CAPS/$c.wav
    [ -f "$wav" ] || continue
    echo "=== $c"
    "$PKG" "$wav" -o "$OUT/$c.pkg.wav" | sed -n '1,20p' > "$OUT/$c.pkg.txt"
    if [ -x "$BASELINE" ]; then
        "$BASELINE" --csv "$OUT/$c.base.csv" --out-wav "$OUT/$c.base.wav" \
                   "$wav" > /dev/null 2>&1 || true
    fi
    cat "$OUT/$c.pkg.txt"
done

python3 - "$OUT" <<'EOF'
import csv, os, sys, wave, statistics, math

out = sys.argv[1]


def wav_rms(path):
    try:
        w = wave.open(path)
    except Exception:
        return None
    n = w.getnframes()
    if n == 0:
        return 0.0, 0.0
    data = w.readframes(n)
    s = memoryview(data).cast('h')
    rms = math.sqrt(sum(float(x) * x for x in s) / max(1, len(s)))
    return n / w.getframerate(), rms


print("\n%-22s %8s %8s %8s %8s" % ("capture", "pkg s", "pkg rms",
                                   "base s", "base rms"))
for f in sorted(os.listdir(out)):
    if not f.endswith('.pkg.txt'):
        continue
    c = f[:-8]
    p = wav_rms(os.path.join(out, c + '.pkg.wav'))
    b = wav_rms(os.path.join(out, c + '.base.wav'))
    print("%-22s %8.2f %8.0f %8s %8s" % (
        c, p[0] if p else -1, p[1] if p else -1,
        "%.2f" % b[0] if b else "-", "%.0f" % b[1] if b else "-"))
EOF
