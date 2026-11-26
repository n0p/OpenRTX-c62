#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Host tests of the bench tool's measuring half (tools/bench/correlate.py).

Synthetic signals with impairments that are known by construction: the test is
that the measurement gives them back, and that the tolerance claimed in
docs/BENCH.md is the tolerance the code actually delivers.  No radio and no
SDR are involved, so this runs anywhere numpy does.

    python3 tools/bench/tests/test_correlate.py
"""

import math
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import correlate as C  # noqa: E402


def fm_iq(seconds=0.5, fs=240000.0, offset_hz=0.0, tone_hz=0.0,
          deviation_hz=0.0, amplitude=0.3, noise_db=-90.0, gate=None,
          seed=1):
    """A carrier, possibly modulated and gated, at a known offset."""
    n = int(seconds * fs)
    i = np.arange(n)
    phase = 2 * math.pi * offset_hz * i / fs
    if tone_hz and deviation_hz:
        phase = phase + (deviation_hz / tone_hz) * np.sin(
            2 * math.pi * tone_hz * i / fs)
    iq = amplitude * np.exp(1j * phase)
    if gate is not None:
        t = i / fs
        iq = iq * ((t >= gate[0]) & (t < gate[1]))
    rng = np.random.default_rng(seed)
    noise = 10 ** (noise_db / 20.0) * (rng.normal(size=n) +
                                       1j * rng.normal(size=n)) / math.sqrt(2)
    return (iq + noise).astype(np.complex64)


class CarrierTest(unittest.TestCase):
    def test_offset_is_recovered(self):
        for want in (0.0, 137.0, -412.5, 1200.0):
            iq = fm_iq(offset_hz=want)
            got = C.carrier_offset_hz(iq, 240000.0)
            self.assertAlmostEqual(got, want, delta=2.0,
                                   msg="offset %s came back as %s" % (want, got))

    def test_offset_survives_noise(self):
        iq = fm_iq(offset_hz=250.0, noise_db=-40.0)
        got = C.carrier_offset_hz(iq, 240000.0)
        self.assertAlmostEqual(got, 250.0, delta=5.0)

    def test_power_tracks_amplitude(self):
        a = C.power_dbfs(fm_iq(amplitude=0.5))
        b = C.power_dbfs(fm_iq(amplitude=0.25))
        self.assertAlmostEqual(a - b, 20 * math.log10(2.0), delta=0.6)

    def test_occupied_bandwidth_of_a_clean_carrier_is_small(self):
        iq = fm_iq(seconds=1.0, offset_hz=0.0)
        bw = C.occupied_bandwidth_hz(iq, 240000.0, fraction=0.99)
        self.assertLess(bw, 200.0)

    def test_occupied_bandwidth_grows_with_deviation(self):
        narrow = C.occupied_bandwidth_hz(
            fm_iq(seconds=0.5, tone_hz=1000.0, deviation_hz=500.0), 240000.0)
        wide = C.occupied_bandwidth_hz(
            fm_iq(seconds=0.5, tone_hz=1000.0, deviation_hz=2000.0), 240000.0)
        self.assertGreater(wide, narrow * 2)


class DeviationTest(unittest.TestCase):
    def test_deviation_is_recovered(self):
        for dev in (500.0, 2100.0, 4000.0):
            iq = fm_iq(seconds=0.5, tone_hz=1000.0, deviation_hz=dev)
            d = C.deviation_hz(iq, 240000.0)
            self.assertAlmostEqual(d["p99_hz"], dev, delta=dev * 0.12,
                                   msg="deviation %s came back as %s" %
                                       (dev, d["p99_hz"]))

    def test_tone_frequency_is_recovered(self):
        iq = fm_iq(seconds=0.5, tone_hz=880.0, deviation_hz=1500.0)
        audio = C.demod_fm(iq, 240000.0)
        f = C.tone_frequency_hz(audio, 240000.0)
        self.assertAlmostEqual(f, 880.0, delta=3.0)

    def test_unmodulated_carrier_has_no_deviation(self):
        d = C.deviation_hz(fm_iq(seconds=0.3, noise_db=-70.0), 240000.0)
        self.assertLess(d["p99_hz"], 200.0)


class EdgeTest(unittest.TestCase):
    def test_edges_are_found_where_they_were_put(self):
        iq = fm_iq(seconds=1.0, gate=(0.20, 0.60))
        e = C.edge_times_s(iq, 240000.0, block_ms=0.5)
        self.assertAlmostEqual(e["on"], 0.20, delta=0.005)
        self.assertAlmostEqual(e["off"], 0.60, delta=0.005)

    def test_no_carrier_reports_no_edges(self):
        iq = fm_iq(seconds=0.3, noise_db=-40.0, gate=None)
        # Gate everything out: the capture is noise only.
        iq = (iq * 0.0 + 1e-6 * (np.random.default_rng(3).normal(
            size=len(iq)) + 1j * np.random.default_rng(4).normal(
                size=len(iq)))).astype(np.complex64)
        e = C.edge_times_s(iq, 240000.0)
        self.assertIsNone(e["on"])


class StabilityTest(unittest.TestCase):
    def test_a_steady_carrier_has_no_drift(self):
        ts, offs = C.frequency_series(fm_iq(seconds=2.0, offset_hz=50.0),
                                      240000.0, block_ms=100.0)
        st = C.stability(ts, offs, nominal_hz=145.5e6)
        self.assertLess(abs(st["drift_hz_s"]), 5.0)
        self.assertLess(st["std_hz"], 3.0)

    def test_ppm_is_computed_against_the_nominal(self):
        ts = np.arange(10, dtype=float)
        offs = np.full(10, 145.5)  # 145.5 Hz at 145.5 MHz
        st = C.stability(ts, offs, nominal_hz=145.5e6)
        self.assertAlmostEqual(st["ppm_mean"], 1.0, delta=0.01)


class ClockTest(unittest.TestCase):
    def test_fit_recovers_offset_and_ppm(self):
        # The radio's clock runs 40 ppm fast and started 12 s before us.
        radio = np.arange(0, 20000, 500, dtype=float)
        ppm = 40.0
        host = 12e9 + radio * 1e6 * (1 + ppm / 1e6)
        pairs = [(float(r), float(h), 3e6) for r, h in zip(radio, host)]
        fit = C.fit_clock(pairs)
        self.assertAlmostEqual(fit["offset_ns"], 12e9, delta=2e6)
        self.assertAlmostEqual(fit["ppm"], ppm, delta=1.0)
        self.assertLess(fit["rms_ms"], 1.0)

    def test_fit_needs_two_points(self):
        self.assertIsNone(C.fit_clock([]))
        self.assertIsNone(C.fit_clock([(1, 2, 3)]))

    def test_alignment_uncertainty_is_never_better_than_the_tick(self):
        pairs = [(0.0, 0.0, 1e6), (1000.0, 1e9, 1e6)]
        fit = C.fit_clock(pairs)
        self.assertGreaterEqual(C.uncertainty_ms(fit), 1.0)


class CrossCorrelationTest(unittest.TestCase):
    def test_lag_between_two_copies(self):
        fs = 48000.0
        t = np.arange(int(fs * 0.5)) / fs
        a = np.sin(2 * math.pi * 1000 * t)
        delay = 37  # samples
        # b is `a` delayed by 37 samples, so `a` is early: the lag is
        # negative under the "positive means a is late" convention.
        b = np.concatenate([np.zeros(delay), a])[:len(a)]
        xc = C.xcorr_delay_s(a, b, fs)
        self.assertEqual(xc["lag_samples"], -delay)
        self.assertAlmostEqual(xc["lag_s"], -delay / fs, places=6)
        self.assertGreater(xc["peak"], 0.9)

    def test_unrelated_signals_do_not_match(self):
        fs = 48000.0
        t = np.arange(int(fs * 0.5)) / fs
        rng = np.random.default_rng(11)
        a = np.sin(2 * math.pi * 1000 * t)
        b = rng.normal(size=len(a))
        xc = C.xcorr_delay_s(a, b, fs)
        self.assertLess(xc["peak"], 0.2)


class TraceTest(unittest.TestCase):
    def test_peak_of_a_synthetic_trace(self):
        f = np.linspace(145.4e6, 145.6e6, 501)
        dbm = -60 - 20 * np.abs(f - 145.5e6) / 1e6
        dbm[250] = -30.0
        p = C.trace_peak(f, dbm)
        self.assertAlmostEqual(p["freq_hz"], 145.5e6, delta=2e5)
        self.assertAlmostEqual(p["level_dbm"], -30.0, delta=0.5)

    def test_channel_power_of_a_trace(self):
        f = np.linspace(145.4e6, 145.6e6, 501)
        dbm = np.full_like(f, -90.0)
        dbm[240:260] = -40.0
        p = C.trace_channel_power(f, dbm, 145.5e6, 8000.0)
        self.assertGreater(p, -40.0)
        self.assertLess(p, -20.0)

    def test_mask_margin_is_negative_when_out_of_mask(self):
        f = np.linspace(-50000, 50000, 501)
        dbm = np.full_like(f, -80.0)
        dbm[np.abs(f) < 3000] = 0.0          # a narrow carrier at 0 dB
        dbm[np.abs(f - 12500.0) < 600] = -30.0  # and something next door
        # Next door is -30 dB against a -60 dB limit: 30 dB inside the mask.
        margin = C.trace_mask_margin(f, dbm, [(12500.0, -60.0)])
        self.assertGreater(margin["margin_db"], 0)
        # The carrier itself against a 0 dB limit: it is at 0, so -0.0 at
        # worst; against a limit of +3 dB it passes, so this one is a real
        # out-of-mask case only when the limit is below the peak.
        margin = C.trace_mask_margin(f, dbm, [(0.0, 3.0)])
        self.assertLess(margin["margin_db"], 1.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
