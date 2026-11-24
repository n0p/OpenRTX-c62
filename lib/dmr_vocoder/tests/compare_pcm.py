#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two s16le PCM streams sample by sample.

usage: compare_pcm.py a.pcm b.pcm [--frames N] [--min-snr DB]

Prints, per 160-sample frame: how many frames are bit-exact, the worst sample
difference and the SNR of b against a (a is the reference).  Exits 1 if the SNR
is below --min-snr (default 60 dB).
"""
import argparse
import math
import struct
import sys


def load(path):
    with open(path, "rb") as f:
        d = f.read()
    if len(d) % 2:
        sys.exit(f"{path}: odd size, not s16le")
    return list(struct.unpack("<%dh" % (len(d) // 2), d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--frames", type=int, default=160)
    ap.add_argument("--min-snr", type=float, default=60.0)
    args = ap.parse_args()

    A, B = load(args.a), load(args.b)
    if len(A) != len(B):
        sys.exit(f"size mismatch: {len(A)} vs {len(B)} samples")

    exact = 0
    worst = 0
    se = 0.0
    sr = 0.0
    for f in range(len(A) // args.frames):
        lo = f * args.frames
        diff = [A[lo + i] - B[lo + i] for i in range(args.frames)]
        worst = max(worst, max(abs(d) for d in diff))
        if not any(diff):
            exact += 1
        se += sum(d * d for d in diff)
        sr += sum(a * a for a in A[lo:lo + args.frames])

    nframes = len(A) // args.frames
    snr = 10.0 * math.log10(sr / se) if se else float("inf")
    print(f"   {args.a} vs {args.b}: {exact}/{nframes} frames bit-exact, "
          f"max |diff| {worst} LSB, SNR {snr:.2f} dB")
    return 0 if snr >= args.min_snr else 1


if __name__ == "__main__":
    sys.exit(main())
