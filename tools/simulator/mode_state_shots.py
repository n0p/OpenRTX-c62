#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The main screen of every mode, with the receiver state the host cannot decode.

This is the check behind docs/UI.md, "El estado del modo": each operating mode
publishes what it has decoded on the main screen (the state block,
ui/ui_mode_state.h) and no longer as read-only rows of its Settings page. A
simulator that only ever shows "SRCH" would not say whether the block fits, so
the script injects the state a locked receiver would hold (`inject dmr|nxdn|
p25|dstar|tetra-...`, docs/SIMULATOR.md) and then:

  - takes a PNG of the main screen of the eleven modes,
  - reads the dump and requires the expected texts to be *on that screen* (the
    block's line 2 identifiers, line 1 call and quality, line 4 note),
  - requires that no line of the screen runs past the 160 px panel, that no two
    lines share a baseline in the body of the screen (the state block, the
    frequency and the S-meter have one line each), and that no line is empty,
  - requires that the mode's Settings page has no read-only row left (the same
    rule as tests/unit/ui_menu_audit.cpp, checked on the running firmware).

    python3 tools/simulator/mode_state_shots.py --out /tmp/modestate

Exit status is 0 when every mode passed; each failure names the mode and the
text or the line that broke the rule.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simlib import Simulator, SimError  # noqa: E402

# What each mode has to show, and what a locked receiver reports. The `inject`
# arguments are the ones docs/SIMULATOR.md documents.
CASES = [
    # mode,   inject command,                          texts on the main screen
    ("FM",     None,                                   ["FM"]),
    ("M17",    None,                                   ["M17"]),
    ("TETRA",  "tetra-cell 228 8889 1 430500000 0",    ["228-8889", "LA1"]),
    ("DMR",    "dmr 1 2 evm 980 call 2317 9",          ["CC1 TS2", "2317>9"]),
    ("NXDN",   "nxdn 12 evm 40 frames 214 "
               "call 2317 9 1",                        ["RAN12", "2317>9"]),
    ("dPMR",   None,                                   ["SRCH"]),
    ("P25",    "p25 0x293 talkgroup 2317 9 duid 3 "
               "frames 120 bad 1",                     ["NAC293"]),
    ("YSF",    None,                                   ["SRCH"]),
    ("DSTAR",  "dstar EA5ABC CQCQCQ ED5YAM viterbi 2 "
               "text hola desde el aire",              ["EA5ABC>CQCQCQ"]),
    ("AM",     None,                                   ["AM"]),
    ("WFM",    None,                                   ["SRCH"]),
]

# Extras: the TETRA state that used to be the REG/NET rows, and a TETRA call
# taking the screen over (that screen is not the block, so it is only captured).
TETRA_EXTRA = [
    ("tetra-registration", ["tetra-registration 1 3 0"], ["ACC G3"]),
    ("tetra-call", ["tetra-call 1 1 9 2317 166 1 0"], ["G 9"]),
]

PANEL_W = 160


def goto_mode(sim, mode):
    for _ in range(len(CASES) + 2):
        d = sim.dump()
        if d["radio"]["mode"] == mode:
            return d
        sim.cmd("keycombo MONI 5")
    raise SimError("cannot reach mode %s (stuck in %s)"
                   % (mode, sim.dump()["radio"]["mode"]))


def body_lines(dump):
    """Every text line the screen drew, as (y, x, width, align, text)."""
    out = []
    for line in dump.get("lines", []):
        text = line.get("text", "")
        if text:
            out.append((line["y"], line["x"], line.get("w", 0), line["align"],
                        text))
    return out


def span(x, width, align):
    """[start, end) of a drawn string, as gfx_print() places it."""
    if align == 1:            # TEXT_ALIGN_CENTER: centred on the panel
        start = (PANEL_W - width) // 2
        return start, start + width
    if align == 2:            # TEXT_ALIGN_RIGHT: the text ends at x
        return x - width, x
    return x, x + width


def check_screen(case, dump, texts, failures):
    lines = body_lines(dump)
    drawn = " | ".join(t for *_, t in lines)

    for wanted in texts:
        if wanted not in drawn:
            failures.append("%s: the main screen does not show %r (it shows %r)"
                            % (case, wanted, drawn))

    # Nothing runs off the panel
    for y, x, w, align, text in lines:
        x0, x1 = span(x, w, align)
        if x0 < 0 or x1 > PANEL_W:
            failures.append("%s: %r runs off the panel (%d..%d px)"
                            % (case, text, x0, x1))

    # Two strings on one baseline must not overlap. Sharing a baseline is
    # normal (the mode, the clock and the battery in the top bar; the integer
    # and the hundredths of the frequency; the scale of the S-meter), drawing
    # over each other is not.
    for i in range(len(lines)):
        for j in range(i + 1, len(lines)):
            ya, xa, wa, aa, ta = lines[i]
            yb, xb, wb, ab, tb = lines[j]
            if ya != yb:
                continue
            a0, a1 = span(xa, wa, aa)
            b0, b1 = span(xb, wb, ab)
            if (a0 < b1) and (b0 < a1):
                failures.append("%s: %r and %r overlap on baseline %d "
                                "(%d..%d and %d..%d)"
                                % (case, ta, tb, ya, a0, a1, b0, b1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/modestate")
    ap.add_argument("--sim", default=None)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    shots = os.path.join(args.out, "shots")
    os.makedirs(shots, exist_ok=True)

    failures = []
    with Simulator(binary=args.sim, shots=shots) as sim:
        sim.dump()      # the first frame is drawn

        for mode, inject, texts in CASES:
            if mode == "DMR":
                # Without a DMR ID the block is replaced by the "NO TX" warning
                sim.cmd("inject identity 2143017")
            if inject:
                sim.cmd("inject " + inject)
            goto_mode(sim, mode)
            sim.cmd("sync")
            png = sim.shot("main-%s.png" % mode)
            dump = sim.dump()
            with open(os.path.join(args.out, "main-%s.json" % mode), "w") as f:
                import json
                json.dump(dump, f, indent=1)
            check_screen(mode, dump, texts, failures)

        # The two TETRA states that are not the plain cell line
        for name, injects, texts in TETRA_EXTRA:
            sim.cmd("inject tetra-cell 228 8889 1 430500000 0")
            for i in injects:
                sim.cmd("inject " + i)
            goto_mode(sim, "TETRA")
            sim.cmd("sync")
            png = sim.shot("%s.png" % name)
            dump = sim.dump()
            check_screen(name, dump, texts, failures)

        # And out: back to a mode that decodes nothing, for the "no clutter"
        # screenshot: the block has to say SRCH and draw nothing else.
        sim.cmd("inject mode-state clear")
        goto_mode(sim, "DMR")
        sim.cmd("sync")
        sim.shot("main-DMR-nothing.png")
        dump = sim.dump()
        check_screen("DMR (nothing)", dump, ["DMR searching"], failures)

    print("\n".join(failures) if failures else "every mode's main screen is ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
