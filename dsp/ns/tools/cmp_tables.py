#!/usr/bin/env python3
"""Compare the init-time tables of two rnnoise builds (see bench/tables_main.c).

usage: cmp_tables.py reference.txt candidate.txt
"""
import sys
from collections import Counter


def load(path):
    out = []
    with open(path) as f:
        for line in f:
            t = line.split()
            if len(t) == 2 and t[0] in ("hw", "dct", "twr", "twi", "brv", "fac"):
                out.append((t[0], t[1]))
            elif t and t[0] in ("nfft", "init"):
                out.append((t[0], " ".join(t[1:])))
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    print(f"reference entries {len(a)}, candidate entries {len(b)}")
    if len(a) != len(b):
        print("entry count differs")
    kinds = Counter(k for k, _ in a)
    diff = Counter()
    first = None
    for i, ((ka, va), (kb, vb)) in enumerate(zip(a, b)):
        if ka != kb or va != vb:
            diff[ka] += 1
            if first is None:
                first = (i, ka, va, vb)
    for k in ("init", "nfft", "hw", "dct", "fac", "brv", "twr", "twi"):
        if kinds[k]:
            print(f"{k:5} {kinds[k]:5} entries, {diff[k]:5} differ")
    if first:
        i, k, va, vb = first
        print(f"first difference at {k}[{i}]: {va} vs {vb}")
        print("VERDICT: tables differ")
    else:
        print("VERDICT: tables identical")


if __name__ == "__main__":
    main()
