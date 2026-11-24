#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Distortion study for the DMR receive chain.

Takes a discriminator capture that the firmware PHY decodes cleanly and applies
the distortions an analog audio path can add, so that the numbers the radio
shows (bursts, CACH, slot type, colour code, CRC-16) can be compared against a
capture made with the baseband dump.

    ./distort.py capture.wav outdir        # writes outdir/<name>.wav
    ./run.sh capture.wav                   # ... and runs the PHY on each one

Only numpy is needed: the filters are RBJ biquads applied by hand.
"""

import os
import sys
import wave

import numpy as np


def read_wav(path):
    w = wave.open(path)
    n, ch = w.getnframes(), w.getnchannels()
    a = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float64)
    if ch > 1:
        a = a.reshape(-1, ch)[:, 0]
    w.close()
    return a


def write_wav(path, samples):
    a = np.clip(np.round(samples), -32768, 32767).astype("<i2")
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(48000)
    w.writeframes(a.tobytes())
    w.close()


def biquad(x, b, a):
    """Direct form I, one sample at a time (captures are a few seconds)."""
    b = np.asarray(b, dtype=float) / a[0]
    a = np.asarray(a, dtype=float) / a[0]
    y = np.empty_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, xi in enumerate(x):
        yi = b[0] * xi + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        x2, x1 = x1, xi
        y2, y1 = y1, yi
        y[i] = yi
    return y


def highpass(x, fc, fs=48000.0, q=0.7071):
    w0 = 2.0 * np.pi * fc / fs
    alpha = np.sin(w0) / (2.0 * q)
    c = np.cos(w0)
    b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    a = [1 + alpha, -2 * c, 1 - alpha]
    return biquad(x, b, a)


def lowpass(x, fc, fs=48000.0, q=0.7071):
    w0 = 2.0 * np.pi * fc / fs
    alpha = np.sin(w0) / (2.0 * q)
    c = np.cos(w0)
    b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
    a = [1 + alpha, -2 * c, 1 - alpha]
    return biquad(x, b, a)


def agc(x, attack_ms=5.0, fs=48000.0, target_dbfs=-6.0):
    """Peak AGC with a slow release, the shape a speech processor has."""
    env = 0.0
    coef = np.exp(-1.0 / (attack_ms * 1e-3 * fs))
    target = 32767.0 * 10.0 ** (target_dbfs / 20.0)
    out = np.empty_like(x)
    for i, xi in enumerate(x):
        env = coef * env + (1.0 - coef) * abs(xi)
        gain = 1.0 if env < 1e-9 else min(8.0, target / env)
        out[i] = xi * gain
    return out


def add_noise(x, snr_db, seed=1):
    rng = np.random.default_rng(seed)
    p = np.mean(x ** 2)
    n = rng.normal(0.0, np.sqrt(p / 10.0 ** (snr_db / 10.0)), x.shape)
    return x + n


def main():
    src = sys.argv[1]
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    x = read_wav(src)
    peak = np.max(np.abs(x))
    print("%s: %d samples, peak %.0f" % (src, len(x), peak))

    variants = {}
    for name, level in (("clip95", 0.95), ("clip70", 0.70), ("clip50", 0.50),
                        ("clip35", 0.35)):
        variants[name] = np.clip(x, -level * peak, level * peak)
    # One-sided clipping: an AC-coupled input sitting off centre hits one rail
    # first, which is what an ADC does with a DC offset on the signal.
    variants["clip_asym"] = np.clip(x, -0.95 * peak, 0.45 * peak)
    variants["clip_pos"] = np.clip(x, -0.95 * peak, 0.25 * peak)
    variants["quant8"] = np.round(x / 128.0) * 128.0
    variants["quant12"] = np.round(x / 16.0) * 16.0
    variants["hpf150"] = highpass(x, 150.0)
    variants["hpf300"] = highpass(x, 300.0)
    variants["hpf1000"] = highpass(x, 1000.0)
    variants["lpf3000"] = lowpass(x, 3000.0)
    variants["lpf4500"] = lowpass(x, 4500.0)
    variants["agc"] = agc(x)
    variants["noise12"] = add_noise(x, 12.0)
    variants["noise9"] = add_noise(x, 9.0)
    variants["noise6"] = add_noise(x, 6.0)

    for name, y in variants.items():
        # Keep the level comparable: normalise like a capture of the same
        # signal, so the AGC and noise variants do not change the gain
        y = y / max(1e-9, np.max(np.abs(y))) * peak
        write_wav(os.path.join(outdir, name + ".wav"), y)
        print("  %-10s -> %s.wav" % (name, name))


if __name__ == "__main__":
    main()
