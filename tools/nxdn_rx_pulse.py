#!/usr/bin/env python3
"""Generate the NXDN receive pulse: the matched filter for the pulse NXDN sends.

NXDN TS 1-A 3.4/3.5 defines the transmit filter P(f) = H(f) * sinc(fT): a root
raised cosine of roll-off 0.2 times the sinc of a symbol held for its period
(the transmitter holds each symbol for T).  The receiver is H(f) * D(f) with
D(f) = 1 / sinc(fT) = pi f T / sin(pi f T) over |f| <= (1 + a) / 2T, so the two
together give the Nyquist raised cosine H(f)^2.

This script evaluates that receive pulse by quadrature at 48 kHz, 20 samples per
symbol, 4 symbols either side (161 taps), and prints it as a C table with a DC
gain of one.  It also reports the noise bandwidth of the old flat-RRC table and
of the new one, which is the sensitivity the matched filter is expected to buy.

    python3 tools/nxdn_rx_pulse.py            # print the C table
    python3 tools/nxdn_rx_pulse.py --check    # only the bandwidth numbers
"""

import math
import sys

FS = 48000.0
RS = 2400.0
SPS = FS / RS          # 20 samples per symbol at 48 kHz
ALPHA = 0.2
SPAN = 4               # symbols either side
TAPS = int(2 * SPAN * SPS) + 1


def rrc(f):
    """Root raised cosine of roll-off ALPHA, unit gain in the flat part."""
    t = f / RS
    if abs(t) <= (1.0 - ALPHA) / 2.0:
        return 1.0
    if abs(t) >= (1.0 + ALPHA) / 2.0:
        return 0.0
    return math.sqrt(0.5 * (1.0 + math.cos(math.pi / ALPHA
                                           * (abs(t) - (1.0 - ALPHA) / 2.0))))


def inv_sinc(f):
    """1 / sinc(f T): undoes the transmit hold.  Removable at f = 0."""
    x = math.pi * f / RS
    if abs(x) < 1e-12:
        return 1.0
    return x / math.sin(x)


def pulse(taps=TAPS):
    """Receive pulse h(n), n in samples at FS, centred on (taps-1)/2."""
    fmax = (1.0 + ALPHA) / 2.0 * RS
    steps = 20000
    centre = (taps - 1) / 2.0
    out = []
    for n in range(taps):
        t = (n - centre) / FS
        acc = 0.0
        for i in range(steps + 1):
            f = -fmax + 2.0 * fmax * i / steps
            w = 1.0 if 0 < i < steps else 0.5     # trapezoid
            acc += w * rrc(f) * inv_sinc(f) * math.cos(2.0 * math.pi * f * t)
        out.append(acc * 2.0 * fmax / steps)
    return out


def flat_rrc(taps=TAPS):
    """The table NxdnPhy.c used before: plain RRC at 48 kHz, same geometry."""
    centre = (taps - 1) / 2.0
    out = []
    for n in range(taps):
        t = (n - centre) / SPS
        a = ALPHA
        if abs(t) < 1e-9:
            h = 1.0 - a + 4.0 * a / math.pi
        elif abs(abs(t) - 1.0 / (4.0 * a)) < 1e-9:
            h = a / math.sqrt(2.0) * ((1.0 + 2.0 / math.pi)
                                      * math.sin(math.pi / (4.0 * a))
                                      + (1.0 - 2.0 / math.pi)
                                      * math.cos(math.pi / (4.0 * a)))
        else:
            h = (math.sin(math.pi * t * (1.0 - a))
                 + 4.0 * a * t * math.cos(math.pi * t * (1.0 + a))) \
                / (math.pi * t * (1.0 - (4.0 * a * t) ** 2))
        out.append(h)
    return out


def noise_bandwidth(taps):
    """Two-sided noise bandwidth of the decimating filter, in symbols/s.

    The filter runs at 48 kHz and its output is taken every other sample, so the
    noise that reaches the slicer is sum_taps h^2 * (Fs/2) / (Fs/2) -- the
    equivalent noise bandwidth is Fs/2 * sum(h^2) / (sum(h)/1)^2 per output,
    which in units of the symbol rate is what the SNR gain is measured against.
    """
    h = [x / sum(taps) for x in taps]
    return 0.5 * FS * sum(x * x for x in h) / RS


if __name__ == '__main__':
    new = pulse()
    g = sum(new)
    new = [x / g for x in new]
    old = flat_rrc()
    old = [x / sum(old) for x in old]

    if '--check' in sys.argv:
        print('taps %d, noise bandwidth: old %.2f Rs, new %.2f Rs, gain %.2f dB'
              % (TAPS, noise_bandwidth(old), noise_bandwidth(new),
                 10 * math.log10(noise_bandwidth(old) / noise_bandwidth(new))))
        print('centre tap %.6f, sum %.6f' % (new[TAPS // 2], sum(new)))
        sys.exit(0)

    print('/* %d taps: H(f) D(f) at 48 kHz, 20 samples/symbol, a = 0.2 */'
          % TAPS)
    for i in range(0, TAPS, 4):
        row = ', '.join('%.9ef' % v for v in new[i:i + 4])
        print('    ' + row + (',' if i + 4 < TAPS else ''))
