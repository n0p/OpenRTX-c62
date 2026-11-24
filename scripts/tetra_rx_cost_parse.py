#!/usr/bin/env python3
"""Per-block instruction counts of the TETRA receive chain (QEMU profile).

Dump format of libinsnframe: "NINSN NFRAMES TOTAL", NINSN address lines, then
one line per snapshot: the frame's instruction total followed by the counters.

The harness in tests/qemu/tetra_rx_cost.c marks: init, then N times (demod,
push). Consecutive snapshot deltas are therefore demod, push, demod, push ...
"""
import sys

path = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 60
lines = [l for l in open(path).read().split("\n") if l.strip()]
ninsn = int(lines[0].split()[0])
frames = [int(l.split()[0]) for l in lines[1 + ninsn:]]
d = [frames[k] - frames[k - 1] for k in range(1, len(frames))]
if len(d) < 2:
    sys.exit("no snapshots")
init = d[0]
rest = d[1:]
demod = rest[0:2 * N:2]
push = rest[1:2 * N:2]


def pct(seq, p):
    s = sorted(seq)
    return s[min(len(s) - 1, int(p * len(s)))]


def med(seq):
    return pct(seq, 0.5)


def show(name, seq):
    if not seq:
        return 0
    print(f"  {name:22s} median {med(seq):9d}  p95 {pct(seq,0.95):9d}  "
          f"max {max(seq):9d}  min {min(seq):9d} instr/block")
    return med(seq)


print(f"{path}: {len(frames)} snapshots, init {init} instr")
md = show("demodulator", demod)
mp = show("libtetra push", push)
total = md + mp
BLOCK_MS = 30.0
CYCLES = 300e6  # Cortex-M33 in the C62, 1 IPC assumed for cycles
print(f"  {'block total':22s} {total:9d} instr = "
      f"{100.0 * total / (CYCLES * BLOCK_MS / 1000.0):5.2f}% of the M33 "
      f"per 30 ms block (1 IPC)")
print(f"  {'demod share':22s} {100.0 * md / total:5.1f}%")
for name, v in (("demod", md), ("push", mp), ("total", total)):
    print(f"  {name:8s} {v / CYCLES * 1e6:8.1f} us per block at 300 MHz, 1 IPC")
