#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Design and check the 48 kHz <-> 8 kHz polyphase filter of csk6_resample.h.

    scripts/csk6_resample_design.py [--tables]

With no argument it prints the response of the design: passband, the
attenuation at the frequencies the report probes, and the alias/image levels a
tone produces through the integer kernel exactly as the C code computes it.
With --tables it prints the two C tables, ready to paste into the header.

The filter is a Kaiser-windowed sinc, 48 taps at 48 kHz, split into six phases
of eight taps.  48 taps is the cost envelope reported in reports/codecs.md
(section 2.8, 63 k instructions per 10 ms block for both directions), and it is
what fixes the transition band: at 48 kHz a 1 ms window cannot do better than
about 2.5 kHz between the passband edge and the 60 dB stopband.  The design
point below keeps the voice band flat to 2.5 kHz, -3.2 dB at 3.4 kHz (a 4 kHz
telephony band edge) and reaches -70 dB by 6.5 kHz.
"""
import math
import sys

FS = 48000.0
PHASES = 6
TAPS = 8
N = PHASES * TAPS
FC = 3800.0
BETA = 6.0
Q = 15  # Q15 coefficients


def bessel_i0(x):
    s, t = 1.0, 1.0
    for k in range(1, 60):
        t *= (x / 2.0 / k) ** 2
        s += t
    return s


def design():
    m = N - 1
    h = []
    for i in range(N):
        t = i - m / 2.0
        if abs(t) < 1e-12:
            h.append(2.0 * FC / FS)
        else:
            h.append(math.sin(2.0 * math.pi * FC * t / FS) / (math.pi * t))
    w = [bessel_i0(BETA * math.sqrt(max(0.0, 1.0 - (2.0 * i / m - 1.0) ** 2)))
         / bessel_i0(BETA) for i in range(N)]
    h = [hi * wi for hi, wi in zip(h, w)]
    total = sum(h)
    return [hi / total for hi in h]


def response(h, f):
    n = len(h)
    re = sum(h[i] * math.cos(-2 * math.pi * f / FS * i) for i in range(n))
    im = sum(h[i] * math.sin(-2 * math.pi * f / FS * i) for i in range(n))
    return 20.0 * math.log10(math.hypot(re, im) + 1e-12)


def quantise(h, gain):
    """Q15 taps, scaled by `gain` (1 for the decimator, 6 for the interpolator)."""
    q = [int(round(hi * gain * 2 ** Q)) for hi in h]
    return q


def tables(h):
    """The two C tables, in the layout the kernel indexes.

    Decimator: table[b][i] is applied to win[i] of branch b, win[7] being the
    newest sample of that branch.  Branch b holds the samples whose index is
    congruent to b modulo 6, and the output is taken when the sixth sample of
    the group has just arrived, so tap j lives in branch (5 - j) % 6 at a
    distance of j // 6 branch samples.
    Interpolator: table[p][i] is applied to win[i] of the input history, and
    tap j of phase p is h[6 * (7 - i) + p].
    """
    dec = [[0] * TAPS for _ in range(PHASES)]
    for b in range(PHASES):
        for i in range(TAPS):
            j = (5 - b) % PHASES + PHASES * (TAPS - 1 - i)
            dec[b][i] = quantise(h, 1)[j]
    itp = [[quantise(h, PHASES)[PHASES * (TAPS - 1 - i) + p]
            for i in range(TAPS)] for p in range(PHASES)]
    return dec, itp


def sat16(v):
    return 32767 if v > 32767 else (-32768 if v < -32768 else v)


class Decimator:
    def __init__(self, dec):
        self.h = [[0] * 16 for _ in range(PHASES)]
        self.head = 0
        self.phase = 0
        self.dec = dec

    def push(self, x):
        self.h[self.phase][self.head] = x
        self.h[self.phase][self.head + 8] = x
        if self.phase != PHASES - 1:
            self.phase += 1
            return None
        self.phase = 0
        acc = 0
        for b in range(PHASES):
            c = self.dec[b]
            w = self.h[b]
            base = self.head + 1
            for i in range(TAPS):
                acc += c[i] * w[base + i]
        self.head = (self.head + 1) % 8
        return sat16(acc >> Q)


class Interpolator:
    def __init__(self, itp):
        self.h = [0] * 16
        self.head = 0
        self.itp = itp

    def push(self, x):
        self.h[self.head] = x
        self.h[self.head + 8] = x
        out = []
        for p in range(PHASES):
            c = self.itp[p]
            acc = 0
            base = self.head + 1
            for i in range(TAPS):
                acc += c[i] * self.h[base + i]
            out.append(sat16(acc >> Q))
        self.head = (self.head + 1) % 8
        return out


def tone(f, n, fs=FS, amp=8000.0):
    return [int(round(amp * math.sin(2 * math.pi * f * i / fs))) for i in range(n)]


def level(samples, f, fs=FS):
    """Single-bin DFT magnitude of `f`, in dB relative to a full-scale sine."""
    n = len(samples)
    re = sum(s * math.cos(2 * math.pi * f * i / fs) for i, s in enumerate(samples))
    im = sum(s * math.sin(2 * math.pi * f * i / fs) for i, s in enumerate(samples))
    return 20.0 * math.log10(max(math.hypot(re, im) * 2.0 / n, 1e-9) / 32768.0)


def measure(h):
    dec, itp = tables(h)
    print("design: N=%d, fc=%.0f Hz, Kaiser beta=%.1f, Q15" % (N, FC, BETA))
    print("passband (dB):")
    for f in (100, 300, 500, 1000, 1500, 2000, 2500, 3000, 3200, 3400):
        print("  %5d Hz %7.3f" % (f, response(h, f)))
    print("stopband (dB):")
    for f in (3600, 3800, 4000, 4200, 4500, 5000, 5500, 6000, 6500, 7000,
              9000, 12000, 20000):
        print("  %5d Hz %7.1f" % (f, response(h, f)))

    # Alias: 6.5 kHz at 48 kHz must not come out as 1.5 kHz at 8 kHz.
    d = Decimator(dec)
    src = tone(6500.0, 4800)
    out = [y for x in src for y in [d.push(x)] if y is not None]
    out = out[20:]
    print("alias: 6.5 kHz in -> %.1f dB at 1.5 kHz (8 kHz out)"
          % (level(out, 1500.0, 8000.0) + 1e-9))

    # Images: 1 kHz at 8 kHz must not come out at 7 kHz or 9 kHz after the
    # interpolator (both are inside the 48 kHz band).
    i = Interpolator(itp)
    src = tone(1000.0, 600, 8000.0)
    out = []
    for x in src:
        out.extend(i.push(x))
    out = out[40:]
    print("image: 1 kHz in -> %.1f dB at 7 kHz, %.1f dB at 9 kHz"
          % (level(out, 7000.0), level(out, 9000.0)))
    print("       1 kHz itself: %.2f dB (the tone is 8 000 of 32 768)"
          % level(out, 1000.0))


def emit(h):
    dec, itp = tables(h)
    for name, tab in (("dec", dec), ("int", itp)):
        print("\nstatic const int16_t csk6_%s_coef[CSK6_RESAMPLE_PHASES]"
              "[CSK6_RESAMPLE_TAPS] = {" % name)
        for row in tab:
            print("    { %s }," % ", ".join("%6d" % v for v in row))
        print("};")
    print("\n/* table sums: dec %d, int %d (int is 6x: the zero-stuffing gain) */"
          % (sum(sum(r) for r in dec), sum(sum(r) for r in itp)))


if __name__ == "__main__":
    h = design()
    if "--tables" in sys.argv:
        emit(h)
    else:
        measure(h)
