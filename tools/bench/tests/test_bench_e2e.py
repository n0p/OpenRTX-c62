#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
End-to-end test of the bench tool with nothing plugged in: a simulated radio
and a simulated receiver, driven through the CLI the same way a bench run is.

What it proves is the flow and the arithmetic together: that every case runs to
the end, that the numbers it reads back are the ones the model put on the air,
that the report and the artifacts land, and that the same cases work when the
receiver can only give a spectrum (the Anritsu) or only an envelope.

    python3 tools/bench/tests/test_bench_e2e.py
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOL = os.path.join(_HERE, "..")
sys.path.insert(0, _TOOL)
sys.path.insert(0, os.path.join(_TOOL, "..", "cps"))

import cases as CS  # noqa: E402
import sdr as S  # noqa: E402
import simradio  # noqa: E402


class CliRun(unittest.TestCase):
    def test_full_run_in_simulation(self):
        out = tempfile.mkdtemp(prefix="bench-")
        err_hz = 137.0
        cmd = [sys.executable, os.path.join(_TOOL, "bench.py"),
               "--sim", "--sdr", "sim", "--sim-error-hz", str(err_hz),
               "--sim-deviation-hz", "2100", "--out", out, "run", "all"]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stderr[-3000:])
        self.assertTrue(os.path.exists(os.path.join(out, "report.txt")))
        with open(os.path.join(out, "report.json")) as fh:
            rep = json.load(fh)

        by_name = {r["name"]: r for r in rep["results"]}
        for name in ("carrier", "deviation", "power", "mask", "sensitivity",
                     "switching", "stability", "align"):
            self.assertIn(name, by_name, "case %s produced no result" % name)

        # The carrier error is what the model put in, within a hertz or two.
        self.assertAlmostEqual(by_name["carrier"]["numbers"]["error_hz"],
                               err_hz, delta=5.0)

        # Deviation at the radio's full level is the model's calibrated one.
        dev = by_name["deviation"]["numbers"]["per_level"]
        self.assertEqual(len(dev), 3)
        self.assertAlmostEqual(dev[-1]["p99_hz"], 2100.0, delta=150.0)
        # ...and it grows with the level.
        self.assertGreater(dev[1]["p99_hz"], dev[0]["p99_hz"])
        # The tone that was asked for is the tone measured.
        self.assertAlmostEqual(dev[-1]["tone_measured_hz"], 88.0, delta=3.0)

        # Keyed against unkeyed has to be a big difference, not a rounding.
        self.assertGreater(by_name["power"]["numbers"]
                           ["keyed_over_unkeyed_db"], 40.0)

        # The alignment of the two captures: same signal, same tone.
        al = by_name["align"]["numbers"]
        self.assertGreater(al["match"], 0.5)
        self.assertAlmostEqual(al["radio_tone_hz"], al["sdr_tone_hz"],
                               delta=2.0)

        # The clock fit needs a dozen pairs to be worth anything.
        self.assertGreater(rep["clock"]["pairs"], 10)
        self.assertIn("alignment_ms", rep["clock"])

        # The artifacts the cases wrote are there.
        capdir = os.path.join(out, "captures")
        self.assertTrue(os.path.isdir(capdir))
        self.assertGreaterEqual(len(os.listdir(capdir)), 5)

    def test_info_and_cases(self):
        p = subprocess.run([sys.executable, os.path.join(_TOOL, "bench.py"),
                            "--sim", "cases"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0)
        for name in CS.CASES:
            self.assertIn(name, p.stdout)

    def test_a_radio_without_the_namespace_is_refused(self):
        # A device path that is not a radio at all: the tool has to say what
        # is missing, not crash with a traceback.
        p = subprocess.run([sys.executable, os.path.join(_TOOL, "bench.py"),
                            "--device", "/dev/null", "--sdr", "sim",
                            "run", "carrier"],
                           capture_output=True, text=True, timeout=60)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("bench", (p.stderr + p.stdout).lower())


class SpectrumOnly(unittest.TestCase):
    """
    The cases as the analyser sees them: no I/Q, only a calibrated trace.

    This is the branch the MS2090A uses (its I/Q option is not installed), and
    without this test it would only ever run on the hardware.
    """

    def setUp(self):
        self.out = tempfile.mkdtemp(prefix="bench-spec-")

    def _ctx(self, kinds):
        saved = S.Sim.kinds
        S.Sim.kinds = kinds
        self.addCleanup(lambda: setattr(S.Sim, "kinds", saved))
        radio = simradio.open_sim_radio()
        truth = S.SimTruth(freq_hz=145.5e6, offset_hz=80.0,
                           tone_hz=1000.0, deviation_hz=2100.0,
                           peak_dbm=-40.0, always_on=True)
        recv = S.Sim(truth, state=lambda: radio.keyed and
                     {"keyed": True, "tone": radio.tone, "level": radio.level})
        return CS.Context(radio, recv, self.out, 145500000, 88, 100)

    def test_carrier_from_a_trace(self):
        ctx = self._ctx((S.KIND_SPECTRUM,))
        res = CS.run_case("carrier", ctx)
        self.assertIsNotNone(res.numbers.get("error_hz"))
        self.assertAlmostEqual(res.numbers["error_hz"], 80.0, delta=2000.0)

    def test_mask_and_stability_from_traces(self):
        ctx = self._ctx((S.KIND_SPECTRUM,))
        mask = CS.run_case("mask", ctx)
        self.assertIn("measured_db_at_offset", mask.numbers)
        stab = CS.run_case("stability", ctx)
        self.assertIn("spread_hz", stab.numbers)

    def test_switching_from_a_zero_span_envelope(self):
        ctx = self._ctx((S.KIND_ENVELOPE,))
        res = CS.run_case("switching", ctx)
        # No window to key inside of: the level step is what there is, and
        # the case must say it did not measure a latency.
        self.assertIn("on_off_db", res.numbers)
        self.assertNotIn("key_up_latency_ms", res.numbers)
        self.assertTrue(any("latency is not" in n for n in res.notes))

    def test_deviation_needs_iq_and_says_so(self):
        ctx = self._ctx((S.KIND_SPECTRUM,))
        with self.assertRaises(RuntimeError):
            CS.run_case("deviation", ctx)


if __name__ == "__main__":
    unittest.main(verbosity=2)
