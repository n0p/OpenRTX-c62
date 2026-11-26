#!/usr/bin/env python3
"""Build a long, contiguous P25 C4FM discriminator capture from a known frame.

  python3 gen_stream.py tests/unit/p25_frame_bits.h /tmp/stream_20s.wav 20

Tiles the 1728 bits of one real LDU1 (tests/unit/p25_frame_bits.h) back to back
at 4800 symbols/s on a 48 kHz carrier-free baseband, with the same root-raised
cosine the transmitter uses, a residual carrier as DC and the amplitude of a
real discriminator capture.  The demodulator should hold its frame grid for the
whole file, which is what a live control channel looks like: that is the
steady-state cost, as opposed to the acquisition cost.
"""
import re
import sys

import numpy as np

FS, SPS, ALPHA, SPAN = 48000, 10, 0.2, 10
OUTER, DC = 6000, 0.15


def frame_bits(path):
    src = open(path).read()
    body = src[src.index("p25_test_frame_bytes"):]
    body = body[body.index("{") + 1:body.index("}")]
    by = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    bits = [(b >> (7 - i)) & 1 for b in by for i in range(8)]
    assert len(bits) == 1728
    return bits


def rrc():
    n = np.arange(-SPAN * SPS, SPAN * SPS + 1)
    t = n / SPS
    h = np.empty_like(t)
    for i, tt in enumerate(t):
        if abs(tt) < 1e-9:
            h[i] = 1 - ALPHA + 4 * ALPHA / np.pi
        else:
            h[i] = (np.sin(np.pi * tt * (1 - ALPHA))
                    + 4 * ALPHA * tt * np.cos(np.pi * tt * (1 + ALPHA))) / \
                   (np.pi * tt * (1 - (4 * ALPHA * tt) ** 2))
    return h / np.sqrt((h ** 2).sum())


def main(bits_path, out, seconds=20.0, phase=2.25):
    bits = frame_bits(bits_path)
    nsym = 864
    levels = np.array([1 / 3, 1.0, -1 / 3, -1.0])
    sym = np.array([levels[(bits[2 * k] << 1) | bits[2 * k + 1]] for k in range(nsym)])

    nframes = int(np.ceil(seconds * FS / (nsym * SPS)))
    h = rrc()
    total = nframes * nsym * SPS + 2 * SPAN * SPS
    x = np.zeros(total)
    imp = np.tile(sym, nframes)
    x[np.round(phase + np.arange(len(imp)) * SPS).astype(int)] = imp
    y = np.convolve(x, h, mode="same") * OUTER + DC * OUTER
    y = np.clip(np.round(y), -32768, 32767).astype("<i2")

    import wave
    w = wave.open(out, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(FS)
    w.writeframes(y.tobytes())
    w.close()
    print(f"{out}: {len(y)} samples, {len(y)/FS:.2f} s, {nframes} frames")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2],
         float(sys.argv[3]) if len(sys.argv) > 3 else 20.0)
