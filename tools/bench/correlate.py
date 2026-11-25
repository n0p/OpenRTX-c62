#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The measuring half of the bench: turn captures into numbers.

Two kinds of capture arrive here and they are not the same thing, so the
functions are grouped by what they can say:

  * I/Q (RTL-SDR, and any future coherent receiver): everything.  Carrier
    frequency and its error against what the radio was told, FM deviation,
    power, occupied bandwidth, TX/RX edges, carrier stability over time, and
    the audio the receiver hears.
  * A spectrum trace (Anritsu MS2090A: it has no I/Q licence): the frequency
    of the peak, the power in a channel, the shape against a mask, and the
    drift of the peak from sweep to sweep.  No deviation and no edges.
  * A zero-span envelope (the Anritsu's SPAN 0 mode): power against time, so
    TX/RX switching times, and nothing else.

Every function that cannot answer says so by returning None instead of a
number: a report with a hole in it is worth more than a plausible number that
was made up.  The tolerances each measurement can claim are in docs/BENCH.md.

No scipy: numpy is enough for a bench tool and one less thing to install.
"""

import math

import numpy as np


# ---------------------------------------------------------------------------
# Clocks
# ---------------------------------------------------------------------------

def fit_clock(pairs):
    """
    Line up the radio's tick with this machine's clock.

    `pairs` is [(radio_ms, host_ns_mid, rtt_ns)], one per answer.  A straight
    line host_ns = a + b * radio_ms is fitted by least squares, weighting each
    point by 1/rtt so a slow answer counts less than a quick one.

    Returns a dict with:
      offset_ns   host time of radio tick 0 (a)
      slope       ns of host time per ms of radio (1e6 when the crystals agree)
      ppm         the radio's error against this clock, in ppm
      rms_ms      the scatter of the fit, i.e. how well the two clocks agree
      n           points used

    Two points are enough to fit, ten are enough to trust: the scatter is what
    says how much of the offset is real and how much is UART and scheduling.
    """
    pts = [p for p in pairs if p[2] > 0]
    if len(pts) < 2:
        return None
    radio = np.array([p[0] for p in pts], dtype=np.float64)
    host = np.array([p[1] for p in pts], dtype=np.float64)
    w = np.array([1.0 / max(p[2], 1.0) for p in pts], dtype=np.float64)

    # Weighted least squares on a straight line.
    wsum = w.sum()
    mx = (w * radio).sum() / wsum
    my = (w * host).sum() / wsum
    sxx = (w * (radio - mx) ** 2).sum()
    if sxx <= 0:
        return None
    slope = (w * (radio - mx) * (host - my)).sum() / sxx
    offset = my - slope * mx

    resid = host - (offset + slope * radio)
    rms = float(np.sqrt((w * resid ** 2).sum() / wsum)) / 1e6  # ms
    ppm = (slope / 1e6 - 1.0) * 1e6
    return {"offset_ns": float(offset), "slope": float(slope),
            "ppm": float(ppm), "rms_ms": rms, "n": len(pts)}


def radio_to_host_ns(fit, radio_ms):
    """The radio's tick, on this machine's clock."""
    if fit is None:
        return None
    return fit["offset_ns"] + fit["slope"] * radio_ms


def uncertainty_ms(fit):
    """
    What to quote as the alignment error: the fit's own scatter, floored at
    the granularity of the radio's 10 ms tick, and never better than 1 ms.
    """
    if fit is None:
        return None
    return round(max(fit["rms_ms"], 1.0, 10.0), 1)


# ---------------------------------------------------------------------------
# I/Q
# ---------------------------------------------------------------------------

def _window(n):
    return np.hanning(n) if n > 8 else np.ones(n)


def _parabolic(mag, i):
    """Sub-bin peak position by fitting a parabola to the three bins around i."""
    if i <= 0 or i >= len(mag) - 1:
        return float(i)
    a, b, c = mag[i - 1], mag[i], mag[i + 1]
    den = (a - 2 * b + c)
    if den == 0:
        return float(i)
    return i + 0.5 * (a - c) / den


def spectrum(iq, fs, nfft=None):
    """Power spectrum of the whole capture, in dB, shifted so DC is in the
    middle: returns (freqs_hz_relative_to_dc, db)."""
    iq = np.asarray(iq, dtype=np.complex128)
    n = len(iq)
    if nfft is None:
        nfft = 1 << int(math.ceil(math.log2(max(n, 1024))))
    nfft = min(nfft, 1 << 18)
    # Welch over as many segments as fit, so one capture gives a stable shape.
    seg = min(n, nfft)
    k = max(1, n // seg)
    acc = np.zeros(seg)
    win = _window(seg)
    for i in range(k):
        chunk = iq[i * seg:(i + 1) * seg]
        if len(chunk) < seg:
            break
        acc += np.abs(np.fft.fftshift(np.fft.fft(chunk * win))) ** 2
    acc /= max(k, 1)
    acc /= max(win.sum() ** 2, 1)
    db = 10.0 * np.log10(np.maximum(acc, 1e-30))
    f = np.fft.fftshift(np.fft.fftfreq(seg, 1.0 / fs))
    return f, db


def carrier_offset_hz(iq, fs):
    """
    Where the carrier is, relative to the capture centre, in Hz.

    A tone at the centre would come out at 0; the value is what the SDR hears
    minus what the radio was told it was transmitting.  Precision is a small
    fraction of a bin with the parabolic interpolation, i.e. tenths of a hertz
    on a one-second capture at 240 kHz -- far better than the radio's own
    tuning step, which is the point: the error being measured is the radio's.
    """
    iq = np.asarray(iq, dtype=np.complex128)
    if len(iq) < 256:
        return None
    f, db = spectrum(iq, fs, nfft=1 << int(math.ceil(math.log2(len(iq)))))
    i = int(np.argmax(db))
    pos = _parabolic(db, i)
    bin_hz = fs / len(db)
    return float(pos * bin_hz + f[0])


def power_dbfs(iq):
    """Mean power of the capture, in dB relative to a full-scale sine."""
    iq = np.asarray(iq, dtype=np.complex128)
    if len(iq) == 0:
        return None
    return float(10.0 * np.log10(max(np.mean(np.abs(iq) ** 2), 1e-20)))


def channel_power_db(iq, fs, bandwidth_hz):
    """
    Power inside a bandwidth around DC, in dB relative to a full-scale sine.

    This is a *relative* number: an RTL-SDR has no absolute calibration, so
    what it measures is comparable between two takes on the same setup and
    nothing else.  For absolute levels the Anritsu is the instrument.
    """
    f, db = spectrum(iq, fs)
    m = np.abs(f) <= bandwidth_hz / 2.0
    if not m.any():
        return None
    # Sum the linear powers of the bins inside the channel, then back to dB.
    return float(10.0 * np.log10(np.sum(10.0 ** (db[m] / 10.0)) + 1e-30))


def occupied_bandwidth_hz(iq, fs, fraction=0.99):
    """The width that holds `fraction` of the power, in Hz."""
    f, db = spectrum(iq, fs)
    p = 10.0 ** (db / 10.0)
    order = np.argsort(f)
    p = p[order]
    f = f[order]
    c = np.cumsum(p)
    if c[-1] <= 0:
        return None
    lo = f[np.searchsorted(c, c[-1] * (1 - fraction) / 2)]
    hi = f[np.searchsorted(c, c[-1] * (1 + fraction) / 2)]
    return float(hi - lo)


def demod_fm(iq, fs, decim=1):
    """
    FM demodulation: the phase difference between consecutive samples, scaled
    to Hz.  This is the same quantity the radio's discriminator delivers, so
    the two captures can be compared sample by sample.
    """
    iq = np.asarray(iq, dtype=np.complex128)
    if len(iq) < 2:
        return np.zeros(0)
    d = iq[1:] * np.conj(iq[:-1])
    audio = np.angle(d) * (fs / (2.0 * math.pi))
    if decim > 1:
        audio = audio[::decim]
    return audio


def tone_frequency_hz(audio, fs):
    """Dominant frequency of a demodulated capture, with sub-bin precision."""
    a = np.asarray(audio, dtype=np.float64)
    if len(a) < 64:
        return None
    a = a - a.mean()
    nfft = 1 << int(math.ceil(math.log2(len(a))))
    mag = np.abs(np.fft.rfft(a * _window(len(a)), nfft))
    i = int(np.argmax(mag))
    pos = _parabolic(mag, i)
    return float(pos * fs / nfft)


def deviation_hz(iq, fs, percentile=99.0):
    """
    Peak FM deviation, in Hz.

    The instantaneous frequency of the capture is the deviation the modulator
    produced, plus whatever constant the carrier's own offset puts there (a
    receiver whose local oscillator is 137 Hz off hears a 137 Hz DC in its
    discriminator, which is physics and not modulation).  So the DC is taken
    out first and returned separately as `dc_hz`: that number is the carrier
    error as the discriminator sees it, and `p99_hz` is the modulation.

    The 99th percentile rather than the maximum, so one noisy sample cannot
    invent a kilohertz; both are returned.
    """
    inst = demod_fm(iq, fs)
    if len(inst) < 64:
        return None
    dc = float(inst.mean())
    ac = inst - dc
    mag = np.abs(ac)
    return {"p99_hz": float(np.percentile(mag, percentile)),
            "peak_hz": float(mag.max()),
            "rms_hz": float(np.sqrt(np.mean(ac ** 2))),
            "dc_hz": dc}


def tone_amplitude_db(audio):
    """Level of the demodulated tone against a full-scale sine, in dB."""
    a = np.asarray(audio, dtype=np.float64)
    if len(a) == 0:
        return None
    return float(10.0 * np.log10(max(np.mean(a ** 2), 1e-20)))


def envelope_db(iq, fs, block_ms=1.0):
    """Power against time, in dB, at block_ms resolution."""
    iq = np.asarray(iq, dtype=np.complex128)
    n = max(1, int(fs * block_ms / 1000.0))
    k = len(iq) // n
    if k == 0:
        return np.zeros(0), np.zeros(0)
    p = np.abs(iq[:k * n].reshape(k, n)) ** 2
    db = 10.0 * np.log10(np.maximum(p.mean(axis=1), 1e-20))
    t = np.arange(k) * (n / fs)
    return t, db


def edge_times_s(iq, fs, above_db=10.0, block_ms=0.5):
    """
    When the carrier comes up and goes down, in seconds from the start of the
    capture, at the resolution of the block period.

    `above_db` is how far over the noise floor the signal has to be: 10 dB
    keeps a burst of noise from being read as a carrier.  Returns
    {"on": t or None, "off": t or None, "floor_db": ...}.
    """
    t, db = envelope_db(iq, fs, block_ms=block_ms)
    if len(db) < 4:
        return {"on": None, "off": None, "floor_db": None}
    floor = float(np.percentile(db, 10))
    thr = floor + above_db
    above = db > thr
    if not above.any():
        return {"on": None, "off": None, "floor_db": floor}
    on = float(t[int(np.argmax(above))])
    # The last sample above the threshold, plus one block: the carrier was up
    # during that block.
    last = int(len(above) - 1 - np.argmax(above[::-1]))
    off = float(t[last] + block_ms / 1000.0) if last < len(above) - 1 else None
    return {"on": on, "off": off, "floor_db": floor,
            "peak_db": float(db[above].max())}


def frequency_series(iq, fs, block_ms=50.0):
    """
    The carrier frequency, block by block: what a stability measurement is
    made of.  Returns (times_s, offsets_hz).
    """
    iq = np.asarray(iq, dtype=np.complex128)
    n = max(64, int(fs * block_ms / 1000.0))
    k = len(iq) // n
    ts, fs_hz = [], []
    for i in range(k):
        off = carrier_offset_hz(iq[i * n:(i + 1) * n], fs)
        if off is None:
            continue
        ts.append((i + 0.5) * n / fs)
        fs_hz.append(off)
    return np.array(ts), np.array(fs_hz)


def stability(ts, offsets, nominal_hz=None):
    """
    How steady the carrier is: mean, standard deviation, drift, and the same
    numbers in ppm when the nominal frequency is known.
    """
    if len(offsets) < 3:
        return None
    offs = np.asarray(offsets, dtype=np.float64)
    mean = float(offs.mean())
    std = float(offs.std(ddof=1))
    # Least squares for the drift: hertz per second.
    tt = np.asarray(ts, dtype=np.float64)
    tt = tt - tt[0]
    if tt[-1] > 0:
        drift = float(np.polyfit(tt, offs, 1)[0])
    else:
        drift = None
    out = {"mean_hz": mean, "std_hz": std, "drift_hz_s": drift,
           "span_s": float(tt[-1]) if len(tt) else 0.0}
    if nominal_hz:
        out["ppm_mean"] = mean / nominal_hz * 1e6
        out["ppm_std"] = std / nominal_hz * 1e6
        if drift is not None:
            out["ppm_per_s"] = drift / nominal_hz * 1e6
    return out


def xcorr_delay_s(a, b, fs, max_lag_s=0.5):
    """
    Lag between two audio captures of the same signal, in seconds, plus how
    well they match.

    This is the correlation of the two captures the task is about: the radio's
    discriminator against the SDR's demodulation.  A positive lag means `a` is
    late relative to `b`.  The normalised peak (0..1) says whether the two are
    the same signal at all -- below ~0.2 they are not.
    """
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    if len(a) < 64 or len(b) < 64:
        return None
    a = a - a.mean()
    b = b - b.mean()
    n = 1 << int(math.ceil(math.log2(len(a) + len(b))))
    # A * conj(B): with this convention the peak lands at +d when
    # a[n] = b[n - d], i.e. a positive lag means `a` is late -- the reading
    # the printout uses.
    x = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    max_lag = int(min(len(a), len(b), fs * max_lag_s))
    idx = np.concatenate((np.arange(0, max_lag),
                          np.arange(n - max_lag, n)))
    seg = x[idx]
    j = int(np.argmax(seg))
    lag = idx[j]
    if lag >= n - max_lag:
        lag -= n
    den = math.sqrt(float(np.sum(a * a)) * float(np.sum(b * b)))
    peak = float(seg[j] / den) if den > 0 else 0.0
    return {"lag_s": float(lag / fs), "peak": peak,
            "lag_samples": int(lag)}


def tone_in_audio(samples, fs):
    """Frequency of a captured discriminator/audio window, for the radio's own
    capture: what it heard, without any SDR in the way."""
    return tone_frequency_hz(np.asarray(samples, dtype=np.float64), fs)


# ---------------------------------------------------------------------------
# Spectrum traces (Anritsu, or synthetic)
# ---------------------------------------------------------------------------

def trace_peak(freqs_hz, dbm):
    """Peak of a spectrum trace with sub-bin interpolation: (freq, level)."""
    f = np.asarray(freqs_hz, dtype=np.float64)
    d = np.asarray(dbm, dtype=np.float64)
    if len(f) < 8:
        return None
    i = int(np.argmax(d))
    pos = _parabolic(d, i)
    bin_hz = (f[1] - f[0]) if len(f) > 1 else 1.0
    return {"freq_hz": float(f[0] + pos * bin_hz), "level_dbm": float(d[i])}


def trace_channel_power(freqs_hz, dbm, center_hz, bandwidth_hz):
    """Power in a channel from a trace, in dBm (the analyser is calibrated)."""
    f = np.asarray(freqs_hz, dtype=np.float64)
    d = np.asarray(dbm, dtype=np.float64)
    m = np.abs(f - center_hz) <= bandwidth_hz / 2.0
    if not m.any():
        return None
    return float(10.0 * np.log10(np.sum(10.0 ** (d[m] / 10.0)) + 1e-30))


def trace_mask_margin(freqs_hz, dbm, mask_points):
    """
    Worst margin against a mask, in dB (negative means out of mask).

    `mask_points` is [(freq_hz, limit_dbm), ...] measured *relative to the
    carrier*: the mask in the standards is relative, and so is a measurement
    made with an uncalibrated receiver.
    """
    f = np.asarray(freqs_hz, dtype=np.float64)
    d = np.asarray(dbm, dtype=np.float64)
    worst = None
    for hz, limit in mask_points:
        i = int(np.argmin(np.abs(f - hz)))
        margin = float(d[i] - limit)
        if worst is None or margin < worst[1]:
            worst = (hz, margin)
    return None if worst is None else {"offset_hz": worst[0],
                                       "margin_db": worst[1]}


def trace_peak_drift(traces):
    """
    How far the peak moves between sweeps: [(freq_hz, level_dbm)], oldest
    first.  Returns the spread, which is the analyser's own resolution plus
    whatever the carrier did.
    """
    fs = [t[0] for t in traces if t is not None]
    if len(fs) < 2:
        return None
    return {"mean_hz": float(np.mean(fs)), "spread_hz": float(max(fs) - min(fs)),
            "n": len(fs)}
