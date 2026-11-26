#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Golay(23,12) syndrome -> error pattern table for the DMR vocoder.

The DMR AMBE+2 frame carries two Golay codewords: the A block is Golay(24,12)
(A = 12 data bits + 12 check bits) and the B block is Golay(23,12) after being
descrambled.  Both are decoded with the same primitive code: Golay(23,12) with
generator polynomial g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 (0xC75).

Golay(23,12) is a perfect code: every one of the 2^11 syndromes corresponds to
exactly one error pattern of weight <= 3, so a full syndrome table decodes up to
three bit errors.  The table below is derived here from the generator polynomial
alone (no table is copied from any third-party source).

Usage: gen_golay_table.py > dmr_golay_table.c
"""
import sys

GENPOL = 0x0C75        # g(x) for Golay(23,12)
X22 = 0x400000
X11 = 0x000800
MASK12 = 0xFFFFF800
NSYN = 1 << 11


def syndrome(pattern: int) -> int:
    aux = X22
    if pattern >= X11:
        while pattern & MASK12:
            while not (aux & pattern):
                aux >>= 1
            pattern ^= (aux // X11) * GENPOL
    return pattern


def main() -> None:
    table = [0] * NSYN
    seen = [False] * NSYN
    # Weight 0 (the zero pattern) first, then all 1, 2 and 3 bit patterns.
    cands = [0]
    for a in range(23):
        cands.append(1 << a)
        for b in range(a + 1, 23):
            cands.append((1 << a) | (1 << b))
            for c in range(b + 1, 23):
                cands.append((1 << a) | (1 << b) | (1 << c))
    for pat in cands:
        s = syndrome(pat)
        if not seen[s]:
            seen[s] = True
            table[s] = pat
    missing = [s for s in range(NSYN) if not seen[s]]
    if missing:
        sys.exit("error: %d syndromes have no weight<=3 pattern" % len(missing))

    out = sys.stdout
    out.write("/* SPDX-FileCopyrightText: 2026 OpenRTX Contributors\n")
    out.write(" * SPDX-License-Identifier: GPL-3.0-or-later\n")
    out.write(" *\n")
    out.write(" * GENERATED FILE - do not edit.  Regenerate with tools/gen_golay_table.py.\n")
    out.write(" * Syndrome -> error pattern map for Golay(23,12), g(x) = 0xC75.\n")
    out.write(" * Entry i is the 23-bit error pattern whose syndrome is i; every entry has\n")
    out.write(" * weight <= 3 and all 2048 syndromes are covered (Golay(23,12) is perfect).\n")
    out.write(" */\n\n")
    out.write('#include "dmr_golay.h"\n\n')
    out.write("const uint32_t dmr_golay23127_err[2048] = {\n")
    for i in range(0, NSYN, 8):
        row = ", ".join("0x%06X" % v for v in table[i:i + 8])
        out.write("    " + row + ",\n")
    out.write("};\n")


if __name__ == "__main__":
    main()
