#!/usr/bin/env python3
"""Per-frame cost on the Xtensa ISS, and where it goes.

usage: cost.py cost0.txt cost8.txt N gprof0.txt gprof8.txt

The harness runs two warm-up frames plus N measured ones; both builds run the
same warm-up, so the difference of the two totals is exactly the N measured
frames.  That avoids instrumenting anything: the ISS counts instructions and
cycles, and the run is deterministic.  The same subtraction is applied to the
per-function profile, so init-time work (the FFT twiddles, the model copy) drops
out and what is left is one frame.
"""
import re
import sys


def parse_summary(path):
    text = open(path, errors="ignore").read()
    insn = re.search(r"Committed instructions\s+(\d+)", text)
    cyc = re.search(r"^Cycles: total = (\d+)", text, re.M)
    if not insn or not cyc:
        raise SystemExit(f"no counts in {path}")
    return int(insn.group(1)), int(cyc.group(1))


def parse_gprof(path):
    """Flat profile: self cycles per function (the second numeric column)."""
    out = {}
    in_flat = False
    for line in open(path, errors="ignore"):
        if line.startswith("Flat profile"):
            in_flat = True
            continue
        if not in_flat:
            continue
        m = re.match(r"\s*[\d.]+\s+([\d.]+)\s+([\d.]+)\s+(\S+)?\s*(\S+)?\s*(\S+)?\s*(\S+)?\s*(\S+)?\s*(\S+)?\s*(\S+)?\s*$", line)
        if not m:
            continue
        fields = line.split()
        # a data row has at least: %, cumulative, self, and a name somewhere
        if len(fields) >= 4 and re.match(r"^[\d.]+$", fields[0]):
            name = fields[-1]
            try:
                if len(fields) >= 7 and re.match(r"^\d+$", fields[3]):
                    self_cycles = float(fields[2])
                else:
                    self_cycles = float(fields[2])
            except ValueError:
                continue
            out[name] = out.get(name, 0.0) + self_cycles
    return out


def main():
    c0, c8, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    i0, y0 = parse_summary(c0)
    i1, y1 = parse_summary(c8)
    di, dy = (i1 - i0) / n, (y1 - y0) / n
    print(f"# ISS: {i0} -> {i1} instructions, {y0} -> {y1} cycles ({n} measured frames)")
    print(f"instructions per 10 ms frame: {di:.0f}")
    print(f"cycles per 10 ms frame:       {dy:.0f}  (CPI {dy / di:.3f})")
    for mhz in (300,):
        print(f"at {mhz} MHz: {dy * 100 / (mhz * 1e6) * 100:.1f} % of the core, "
              f"{dy / (mhz * 1e6) * 1e3:.2f} ms of CPU per 10 ms of audio")

    if len(sys.argv) > 5:
        g0 = parse_gprof(sys.argv[4])
        g8 = parse_gprof(sys.argv[5])
        rows = []
        for name, v in g8.items():
            per = v - g0.get(name, 0.0)
            if per > 0:
                rows.append((per, name))
        rows.sort(reverse=True)
        tot = sum(r[0] for r in rows) or 1.0
        print(f"\n{'cycles/frame':>13} {'%':>5}  function (init subtracted)")
        for per, name in rows[:16]:
            print(f"{per:13.0f} {100 * per / tot:5.1f}  {name}")


if __name__ == "__main__":
    main()
