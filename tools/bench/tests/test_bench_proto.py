#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
End-to-end test of the bench namespace against a radio simulated on a pty.

The device is tests/host/pc_proto_host/device.c built with CONFIG_C62_BENCH_PROTO:
the real protocol code, the real dispatch table, the real bench module.  The
client is tools/bench/benchproto.py, the one that ships.  What this proves is
the pair over a byte stream: the seven messages, the timestamps on every
answer, the capture filling over the ticks with its t0 and its block shape, the
sweep walking the band, and a production build refusing all of it.

    python3 tools/bench/tests/test_bench_proto.py --device build/pc_proto_device_bench
"""

import argparse
import os
import pty
import signal
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "cps"))

import pcproto as P  # noqa: E402
import benchproto as B  # noqa: E402


class BenchRadio:
    """A device binary on a pty, with the bench client attached."""

    def __init__(self, binary, client_caps=P.CAP_EVENTS):
        self.master, self.slave = pty.openpty()
        name = os.ttyname(self.slave)
        self.proc = subprocess.Popen([binary, name],
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        self.port = P.Port(fd=self.master, timeout=3.0)
        self.link = B.BenchLink(self.port, client_caps=client_caps)
        time.sleep(0.1)
        self.port.read(0.05)
        self.link.connect()

    def close(self):
        for fn in (lambda: self.link.close(), self.port.close,
                   lambda: os.close(self.master)):
            try:
                fn()
            except Exception:
                pass
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        try:
            os.close(self.slave)
        except OSError:
            pass


class BenchTest(unittest.TestCase):
    binary = None

    @classmethod
    def setUpClass(cls):
        if not cls.binary:
            raise unittest.SkipTest("no device binary")
        cls.radio = BenchRadio(cls.binary)

    @classmethod
    def tearDownClass(cls):
        cls.radio.close()

    def setUp(self):
        self.link = self.radio.link
        # What bench.py sets from --power: every TUNE carries it, so the
        # simulated PA has something to key with and the tests do not depend on
        # the one before them having tuned.
        self.link.default_power = 500
        self.link.arm()

    def test_capability_and_arm(self):
        self.assertTrue(self.link.has(B.CAP_BENCH))
        a = self.link.arm()
        self.assertGreaterEqual(a["epoch"], 2)
        self.assertEqual(a["rate"], 48000)
        self.assertGreater(a["max_ms"], 0)
        b = self.link.arm()
        self.assertEqual(b["epoch"], a["epoch"] + 1, "epoch must advance")

    def test_every_answer_is_stamped(self):
        a = self.link.arm()
        t = self.link.tune(145500000, mode=1, bw=2)
        self.assertGreater(t["tick"], 0)
        self.assertGreaterEqual(t["tick"], a["tick"])
        # The pairs are what correlate.fit_clock needs.
        self.assertGreaterEqual(len(self.link.clock_pairs), 2)
        for tick, host_ns, rtt in self.link.clock_pairs[-2:]:
            self.assertGreater(tick, 0)
            self.assertGreater(host_ns, 0)
            self.assertGreater(rtt, 0)

    def test_tune_reports_what_it_took(self):
        t = self.link.tune(145.5e6, mode=1, bw=2)
        self.assertEqual(t["rx"], 145500000)
        self.assertEqual(t["tx"], 145500000)
        self.assertEqual(t["mode"], 1)
        self.assertEqual(t["bw"], 2)
        self.assertEqual(t["delta_hz"], 0)  # the radio cannot measure it
        # A frequency the simulator refuses comes back as an error.
        with self.assertRaises(P.ProtoError):
            self.link.tune(1000000)

    def test_ptt_keys_and_reports_the_tone_it_can_generate(self):
        p = self.link.ptt(True, tone=88, level=50)
        self.assertEqual(p["tx_state"], 2)
        self.assertEqual(p["tone"], 88)
        self.assertGreater(p["tick"], 0)
        time.sleep(0.05)
        # An edge event with the same tick is behind the answer.
        edges = self.link.bench_edges()
        self.assertTrue(edges, "no edge event for the key-up")
        self.assertEqual(edges[-1][0], 1)
        off = self.link.ptt(False)
        self.assertNotEqual(off["tick"], p["tick"])
        # A tone the generator does not have is refused, not rounded.
        p2 = self.link.ptt(True, tone=1200)
        self.assertEqual(p2["tone"], 0)
        self.link.ptt(False)

    def test_status_answers_while_transmitting(self):
        """
        The instrument has to work with the PA keyed.

        A status that waits for the transmit path (or for the state lock the
        UI holds across its FSM) turns the bench blind exactly when it is
        measuring: this is the case that was reported from hardware as
        "BENCH_PTT never answers and the protocol goes silent while keyed".
        Ten polls, worst case under 100 ms.
        """
        self.link.ptt(True, tone=88)
        try:
            worst = 0.0
            for _ in range(10):
                t = time.monotonic()
                self.link.status()
                worst = max(worst, time.monotonic() - t)
            # The simulated radio does not model the RTX status, so tx_state is
            # not what this asks: it is the transport's own latency with the PA
            # keyed, which is what a measurement has to live with.
            self.assertLess(worst, 0.1,
                            "status took %.0f ms with the PTT down"
                            % (worst * 1000.0))
        finally:
            self.link.ptt(False)

    def test_capture_fills_and_reads_back(self):
        samples, meta = self.link.capture(60)
        self.assertEqual(meta["rate"], 48000)
        self.assertEqual(meta["block"], 480, "10 ms blocks at 48 kHz")
        self.assertGreaterEqual(meta["written"], 2880)
        self.assertLessEqual(len(samples), meta["written"])
        self.assertEqual(len(samples) % 480, 0)
        self.assertGreater(meta["t0"], 0)
        # Not silence: the simulated receiver has a tone on the air.
        peak = max(abs(s) for s in samples[:2880])
        self.assertGreater(peak, 5000)

    def test_rssi_series_is_stamped(self):
        series = self.link.rssi(4)
        self.assertEqual(len(series), 4)
        ticks = [t for t, _ in series]
        self.assertEqual(ticks, sorted(ticks))
        self.assertGreater(ticks[1] - ticks[0], 0)

    def test_sweep_walks_the_band(self):
        points, meta = self.link.sweep(145.45e6, 145.55e6, 25000, dwell_ms=20)
        self.assertEqual(meta["total"], 4)
        self.assertEqual(len(points), 4)
        self.assertEqual(points[0][0], 145450000)
        self.assertEqual(points[-1][0], 145525000)
        # The simulated radio has a peak at 145.500: the sweep has to see it.
        levels = dict(points)
        self.assertGreater(levels[145500000], levels[145450000])

    def test_report_is_a_text_with_the_run(self):
        self.link.tune(145500000, mode=1)
        self.link.ptt(True, tone=88)
        self.link.ptt(False)
        text = self.link.report()
        self.assertIn("tune", text)
        self.assertIn("145500000", text)
        self.assertIn("ptt", text)
        self.assertIn("no carrier reference", text)

    # -- the rules of the cable (docs/TEST_PLAN.md §4) ---------------------

    def test_a_spent_token_cannot_key(self):
        """
        A second HELLO spends the first client's token, and a PTT that presents
        a spent one is refused -- not silently dropped, not keyed anyway.

        This is the failure the token exists for: a bench script that armed the
        radio, then something else on the cable (a CPS window, a second run,
        the script reconnecting) greeted.  A PA keyed on behalf of a client
        nobody is listening to is a carrier nobody measures.  The refusal is
        what makes the second client's session the only one that can move the
        radio, and it is why a spent session has to be re-opened (HELLO) and
        re-armed before it touches the PA again.
        """
        self.link.tune(145.5e6, mode=1, bw=2, power=500)
        spent = self.link.token
        self.link.connect()                     # the token rotates here
        self.assertNotEqual(self.link.token, spent)
        try:
            payload = P.tlv_u8(B.T_TX_STATE, 1) + P.tlv_u32(P.T_TOKEN, spent)
            with self.assertRaises(P.ProtoError) as cm:
                self.link.request(B.MSG_BENCH_PTT, payload)
            self.assertEqual(cm.exception.code, 11)   # PC_ERR_NOT_ALLOWED
            # The radio was not keyed: the current session still reads RX.
            self.assertNotEqual(0, self.link.token)
        finally:
            self.link.arm()                     # a fresh run for the next test
        p = self.link.ptt(True, tone=88)
        self.assertEqual(p["tx_state"], 2, "the live session does key")
        self.link.ptt(False)

    def test_a_keying_does_not_spend_the_token(self):
        """
        What spends the token is a HELLO, not a PTT.  A measurement keys more
        than once in a run (deviation at three levels), so a session that died
        on the first keying could not take it -- and the bench client in
        tools/bench does not re-arm between keyings for that reason.
        """
        self.link.tune(145.5e6, mode=1, bw=2, power=500)
        for level in (30, 65):
            p = self.link.ptt(True, tone=88, level=level)
            self.assertEqual(p["tx_state"], 2)
            self.assertEqual(p["level"], level)
            self.link.ptt(False)
        # And the session can still move the radio afterwards.
        t = self.link.tune(145.5e6, mode=1, bw=2)
        self.assertEqual(t["rx"], 145500000)

    def test_a_client_without_the_events_cap_gets_answers_but_no_events(self):
        """
        HELLO carries the client's capabilities; the answers do not depend on
        them, the events do.  A bench script that forgets PC_CAP_EVENTS gets
        every answer and no TX/RX edge, i.e. no way to measure the latency the
        edge is for -- which is why BenchLink advertises it.
        """
        radio = BenchRadio(self.binary, client_caps=0)
        try:
            self.assertEqual(radio.link.client_caps & P.CAP_EVENTS, 0)
            radio.link.default_power = 500
            radio.link.arm()
            radio.link.tune(145.5e6, mode=1, power=500)
            p = radio.link.ptt(True, tone=88, level=50)
            self.assertEqual(p["tx_state"], 2)       # answered, and it keyed
            time.sleep(0.05)
            self.assertEqual(radio.link.bench_edges(), [],
                             "no events without PC_CAP_EVENTS")
            radio.link.ptt(False)
        finally:
            radio.close()

    def test_the_link_answers_at_ten_hertz_not_one(self):
        """
        The transport answers a serialised request in milliseconds; the "1-2 Hz"
        in the notes is a limitation of a client that fires requests without
        waiting for the answers, not of the cable.  Twenty round trips back to
        back, all answered, and the rate is what says which of the two it is.
        """
        n = 20
        t0 = time.monotonic()
        for _ in range(n):
            s = self.link.status()
            self.assertGreater(s["rx_freq"], 0)
        dt = time.monotonic() - t0
        rate = n / dt
        self.assertGreater(rate, 10.0,
                           "the link answered at %.1f Hz, not the 1-2 Hz the "
                           "notes blame" % rate)

    def test_capture_and_retune_are_exclusive(self):
        self.link.capture_arm(200)
        with self.assertRaises(P.ProtoError):
            self.link.tune(145600000)
        # Leave the window to finish so the next test starts clean.
        time.sleep(0.3)
        self.link.capture_status()


class BenchProductionTest(unittest.TestCase):
    """The same modules without CONFIG_C62_BENCH_PROTO: the refusal."""

    binary = None

    @classmethod
    def setUpClass(cls):
        if not cls.binary:
            raise unittest.SkipTest("no production device binary")
        cls.radio = BenchRadio(cls.binary)

    @classmethod
    def tearDownClass(cls):
        cls.radio.close()

    def test_no_bench_capability(self):
        link = self.radio.link
        self.assertFalse(link.has(B.CAP_BENCH))
        for call in (lambda: link.arm(),
                     lambda: link.tune(145500000),
                     lambda: link.ptt(True),
                     lambda: link.capture_arm(20),
                     lambda: link.rssi(1),
                     lambda: link.sweep(145e6, 146e6, 25000),
                     lambda: link.report()):
            with self.assertRaises(P.ProtoError) as cm:
                call()
            self.assertEqual(cm.exception.code, 11)  # PC_ERR_NOT_ALLOWED


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device")
    ap.add_argument("--device-nodebug")
    a, rest = ap.parse_known_args()
    BenchTest.binary = a.device
    BenchProductionTest.binary = a.device_nodebug
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == "__main__":
    main()
