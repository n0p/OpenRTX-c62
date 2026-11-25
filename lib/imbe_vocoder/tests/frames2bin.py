#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert a p25-rx --imbe-out dump into 88-byte-per-frame parameter records.

The host prototype (work/p25-rx) prints one line per IMBE frame of a real P25
capture: 22 hex digits (the 11 parameter bytes, MSB first) and `errs=N`, the
corrected-bit count its FEC layer reported.

    ./frames2bin.py vc_imbe.txt frames.imbe [errs.txt]

Each record of frames.imbe is the 11 packed parameter bytes the LDU carries
(88 bits, b0 first), which is what lib/imbe_vocoder consumes and what the
reference unpacker turns into mbelib-neo's one-bit-per-byte vector; errs.txt
gets one decimal per frame, for the repeat and mute decisions.
"""
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    errs_path = sys.argv[3] if len(sys.argv) > 3 else None

    frames = 0
    errs = []
    with open(src) as f, open(dst, "wb") as out:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            hexstr = parts[0]
            if len(hexstr) != 22:
                sys.exit(f"line {frames + 1}: {len(hexstr)} hex digits, expected 22")
            out.write(bytes.fromhex(hexstr))
            errs.append(int(parts[1].split("=")[1]) if len(parts) > 1 else 0)
            frames += 1

    if errs_path:
        with open(errs_path, "w") as f:
            f.write("\n".join(str(e) for e in errs) + "\n")
    print(f"frames2bin: {frames} frames -> {dst}")


main()
