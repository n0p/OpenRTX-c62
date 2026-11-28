#!/usr/bin/env python3
"""Per-frame instruction count of an M33 harness build, on QEMU mps2-an505.

usage: m33_cost.py <bench.elf>

Runs the ELF under qemu-system-arm with the TCG plugin in common/insnframe.so,
which snapshots its counters every time the guest calls frame_marker(), so each
snapshot delta is one 10 ms frame of rnnoise_process_frame().  Same harness, same
signal and same frame brackets as the ISS measurement on the DSP.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUG = os.path.join(ROOT, "common", "libinsnframe.so")


def main():
    elf = sys.argv[1]
    dump = os.path.join(ROOT, "build", "m33.gmon.txt")
    sym = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True)
    m = re.search(r"^([0-9a-f]+) T frame_marker$", sym.stdout, re.M)
    if not m:
        raise SystemExit("frame_marker not found in the ELF")
    marker = m.group(1)
    env = dict(os.environ, INSNHIST_OUT=dump, INSNHIST_MARKER=marker)
    cmd = ["qemu-system-arm", "-M", "mps2-an505", "-nographic",
           "-semihosting-config", "enable=on,target=native",
           "-kernel", elf, "-plugin", f"file={PLUG}", "-d", "plugin"]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=3600)
    out = p.stdout + p.stderr
    if "INSN_COUNT" not in out:
        raise SystemExit("plugin produced no count:\n" + out[-2000:])
    lines = open(dump).read().split("\n")
    ninsn, nframes, total = map(int, lines[0].split())
    totals = [int(lines[1 + ninsn + k].split()[0]) for k in range(nframes)]
    per = [totals[k + 1] - totals[k] for k in range(len(totals) - 1)]
    # The plugin snapshots on every frame_marker(); the last snapshot is taken
    # when the guest parks after the loop, so drop the frames that are far below
    # the median (an empty bracket is a few thousand instructions, a real frame
    # is over a million).
    if per:
        med = sorted(per)[len(per) // 2]
        per = [x for x in per if x > med // 10]
    if not per:
        raise SystemExit("no frame brackets seen")
    per_sorted = sorted(per)
    n = len(per)
    print(f"# M33 (QEMU mps2-an505, TCG plugin): {n} frames measured")
    print(f"# per frame instructions: min {min(per)}, median {per_sorted[n // 2]}, "
          f"mean {sum(per) / n:.0f}, max {max(per)}")
    for mhz in (300,):
        print(f"at {mhz} MHz: {per_sorted[n // 2] * 100 / (mhz * 1e6) * 100:.1f} % "
              f"of the core in instructions")


if __name__ == "__main__":
    main()
