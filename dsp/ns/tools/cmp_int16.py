#!/usr/bin/env python3
"""Compare the int16 output of ns_dsp_process() with the M33 float dump.

usage: cmp_int16.py out_m33.txt out_ns_dsp.txt

out_m33.txt holds the Cortex-M33 harness output as raw float bits;
out_ns_dsp.txt holds the int16 frames the DSP service body produced.  The float
frames are converted here with the firmware's rounding rule
(openrtx/src/core/noise_suppress.c: clamp, then round half away from zero), so a
match also proves the conversion the service does.
"""
import struct
import sys


def load_float(path):
    frames = []
    for line in open(path):
        t = line.split()
        if t and t[0].startswith("F"):
            frames.append([struct.unpack("<f", bytes.fromhex(x)[::-1])[0]
                           for x in t[1:]])
    return frames


def load_int16(path):
    frames = []
    for line in open(path):
        t = line.split()
        if t and t[0].startswith("I"):
            frames.append([int(x, 16) - (1 << 16) if int(x, 16) >= (1 << 15)
                           else int(x, 16) for x in t[1:]])
    return frames


def to_int16(v):
    if v > 32767.0:
        v = 32767.0
    if v < -32768.0:
        v = -32768.0
    return int(v + 0.5) if v >= 0.0 else int(v - 0.5)


def main():
    ref = load_float(sys.argv[1])
    got = load_int16(sys.argv[2])
    if len(ref) != len(got):
        print(f"frame count differs: {len(ref)} vs {len(got)}")
        return 1
    same = total = 0
    worst = 0
    for k, (a, b) in enumerate(zip(ref, got)):
        for x, y in zip(a, b):
            want = to_int16(x)
            same += (want == y)
            worst = max(worst, abs(want - y))
            total += 1
    print(f"frames {len(ref)}, samples {total}: {same} identical "
          f"({100.0 * same / total:.4f} %), max |diff| {worst} LSB")
    print("VERDICT:", "bit-exact" if same == total else "NOT bit-exact")
    return 0 if same == total else 1


if __name__ == "__main__":
    sys.exit(main())
