#!/usr/bin/env python3
"""Frame-by-frame comparison of two rnnoise harness dumps.

usage: cmp_frames.py reference.txt candidate.txt

Both files hold one line per 10 ms frame: "Fnn" followed by 480 samples as raw
IEEE-754 single-precision bit patterns in hex, which is what the harnesses
print.  The comparison is on the bits first (the criterion) and on the values
second (the tolerance, when the bits do not match).
"""
import struct
import sys


def load(path):
    frames = []
    with open(path) as f:
        for line in f:
            t = line.split()
            if not t or not t[0].startswith("F"):
                continue
            frames.append([struct.unpack("<f", bytes.fromhex(x)[::-1])[0]
                           for x in t[1:]])
    return frames


def snr_db(ref, cand):
    sig = sum(x * x for fr in ref for x in fr)
    err = sum((x - y) ** 2 for fr, fc in zip(ref, cand) for x, y in zip(fr, fc))
    if err == 0:
        return float("inf")
    if sig == 0:
        return float("-inf")
    return 10.0 * (sig / err).__class__(0) if False else 10.0 * __import__("math").log10(sig / err)


def main():
    ref, cand = load(sys.argv[1]), load(sys.argv[2])
    if len(ref) != len(cand):
        print(f"frame count differs: {len(ref)} vs {len(cand)}")
        return 1
    total = same = 0
    maxerr = 0.0
    first_bad = None
    exact_frames = 0
    print(f"{'frame':>5} {'identical':>10} {'max |err|':>10} {'SNR dB':>8}")
    for k, (a, b) in enumerate(zip(ref, cand)):
        if len(a) != len(b):
            print(f"frame {k}: sample count differs")
            return 1
        n_same = sum(1 for x, y in zip(a, b) if x == y)
        err = max((abs(x - y) for x, y in zip(a, b)), default=0.0)
        rms_a = (sum(x * x for x in a) / max(len(a), 1)) ** 0.5
        rms_e = (sum((x - y) ** 2 for x, y in zip(a, b)) / max(len(a), 1)) ** 0.5
        snr = 20 * __import__("math").log10(rms_a / rms_e) if rms_e > 0 and rms_a > 0 else float("inf")
        print(f"{k:5d} {n_same:10d} {err:10.6f} {snr:8.2f}")
        total += len(a)
        same += n_same
        maxerr = max(maxerr, err)
        exact_frames += (n_same == len(a))
        if n_same != len(a) and first_bad is None:
            first_bad = (k, next(i for i, (x, y) in enumerate(zip(a, b)) if x != y))
    print(f"\nframes bit-exact: {exact_frames}/{len(ref)}")
    print(f"samples bit-exact: {same}/{total} ({100.0 * same / total:.4f} %)")
    print(f"max |error|: {maxerr:.6f} LSB (full scale 32768)")
    if same == total:
        print("VERDICT: bit-exact")
    else:
        print(f"VERDICT: not bit-exact; first difference frame {first_bad[0]} "
              f"sample {first_bad[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
