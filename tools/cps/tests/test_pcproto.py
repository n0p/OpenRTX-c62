#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
End-to-end test of the reference client against a simulated radio on a pty.

The device side is tests/host/pc_proto_host/device.c: the real protocol and the
real stores built for the host.  The client side is tools/cps, the one that
ships.  What this test therefore proves is the pair, over a byte stream with no
in-process shortcuts: framing, HELLO, the stores, the events, the log stream
with its losses, the debug gates, and that a production build refuses
everything it should.

    python3 tools/cps/tests/test_pcproto.py --device build/pc_proto_device \
        --device-nodebug build/pc_proto_device_nodebug
"""

import argparse
import json
import os
import pty
import signal
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pcproto as P  # noqa: E402


class Radio:
    """A device binary on a pty, with the client attached to the other end."""

    def __init__(self, binary, args=()):
        self.master, self.slave = pty.openpty()
        self.slave_name = os.ttyname(self.slave)
        self.proc = subprocess.Popen([binary, self.slave_name, *args],
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        # The client side is the master of the pty: the slave belongs to the
        # device binary, which opened it by path.
        self.port = P.Port(fd=self.master, timeout=3.0)
        self.link = P.Link(self.port, verbose=False)
        time.sleep(0.1)
        # Drain whatever the device said before we were listening.
        self.port.read(0.05)

    def close(self):
        try:
            self.link.close()
        except Exception:
            pass
        try:
            self.port.close()
        except Exception:
            pass
        try:
            os.close(self.master)
        except OSError:
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


class RadioTest(unittest.TestCase):
    """Tests both builds have to pass: the stores, the events, the session."""

    binary = None
    expect_debug = False

    def setUp(self):
        self.radio = Radio(self.binary)
        self.link = self.radio.link

    def tearDown(self):
        self.radio.close()

    # -- session -----------------------------------------------------------

    def test_hello_and_capabilities(self):
        h = self.link.connect()
        self.assertEqual(h.u16(P.T_VERSION), P.PROTO_VERSION)
        self.assertGreaterEqual(h.u16(P.T_VERSION), P.PROTO_VERSION_MIN)
        self.assertEqual(h.s(P.T_MODEL), "c62-sim")
        self.assertEqual(h.s(P.T_FIRMWARE), "host-test")
        self.assertTrue(h.u32(P.T_CAPS) & P.CAP_CHANNELS)
        self.assertTrue(h.u32(P.T_CAPS) & P.CAP_EVENTS)
        self.assertEqual(bool(h.u32(P.T_CAPS) & P.CAP_DEBUG), self.expect_debug)
        self.assertEqual(h.u16(P.T_CHCOUNT), 2)
        self.assertEqual(h.u16(P.T_SETTINGS_VER), 8)
        if self.expect_debug:
            self.assertNotEqual(self.link.token, 0)
        else:
            self.assertEqual(self.link.token, 0)

    def test_ping_and_counters(self):
        self.link.connect()
        f = self.link.request(P.MSG_PING)
        self.assertGreater(f.u32(P.T_UPTIME_MS), 0)
        f = self.link.request(P.MSG_COUNTERS)
        self.assertGreaterEqual(len(f.get(P.T_STATS)), 32)

    # -- channels ----------------------------------------------------------

    def test_channel_list_and_get(self):
        self.link.connect()
        chans = self.link.chan_list()
        self.assertEqual(len(chans), 2)
        self.assertEqual(chans[0]["name"], "CALL1")
        self.assertEqual(chans[1]["name"], "DMR1")
        self.assertEqual(chans[1]["dmr_cc"], 3)
        self.assertEqual(chans[1]["mode"], 2)

        one = self.link.chan_get(1)
        self.assertEqual(one["rx_freq"], 433500000)
        self.assertEqual(one["tx_freq"], 438100000)

    def test_channel_write_is_verified(self):
        self.link.connect()
        rec = self.link.chan_get(0)
        rec["name"] = "EDITED"
        rec["rx_freq"] = 145712500
        rec["dmr_cc"] = 9
        back = self.link.chan_set(0, rec)
        self.assertEqual(back["name"], "EDITED")
        self.assertEqual(back["dmr_cc"], 9)
        # And it survives a re-read: nothing cached on our side.
        self.assertEqual(self.link.chan_get(0)["name"], "EDITED")

    def test_channel_add_and_delete(self):
        self.link.connect()
        idx = self.link.chan_add({"name": "NEW", "rx_freq": 446006250,
                                  "tx_freq": 446006250, "power": 500,
                                  "mode": 1, "bandwidth": 1, "flags": 0x02})
        self.assertEqual(idx, 2)
        self.assertEqual(self.link.chan_count(), 3)
        self.link.chan_delete(2)
        self.assertEqual(self.link.chan_count(), 2)

    def test_channel_block_write(self):
        self.link.connect()
        recs = self.link.chan_list()
        recs[0]["name"] = "BLK0"
        recs[1]["name"] = "BLK1"
        self.assertTrue(self.link.chan_write_block(0, recs))
        self.assertEqual(self.link.chan_get(0)["name"], "BLK0")
        self.assertEqual(self.link.chan_get(1)["name"], "BLK1")

    def test_bad_index_and_bad_record(self):
        self.link.connect()
        with self.assertRaises(P.ProtoError) as e:
            self.link.chan_get(40)
        self.assertEqual(e.exception.code, 4)  # bad_index

        rec = self.link.chan_get(0)
        rec["rx_freq"] = 5  # outside every band the radio has
        with self.assertRaises(P.ProtoError) as e:
            self.link.chan_set(0, rec)
        self.assertEqual(e.exception.code, 5)  # invalid_value
        self.assertEqual(self.link.chan_get(0)["rx_freq"], 145500000)

    # -- contacts, zones, settings -----------------------------------------

    def test_contacts(self):
        self.link.connect()
        cts = self.link.contact_list()
        self.assertEqual(len(cts), 1)
        self.assertEqual(cts[0]["call"], "EA5RCH")
        self.assertEqual(cts[0]["dmr_id"], 2143001)
        f = self.link.request(P.MSG_CONTACT_ADD,
                              P.tlv_bytes(P.T_CONTACT, P.contact_pack(
                                  {"call": "EA5ABC", "name": "Ana",
                                   "dmr_id": 2143002, "mode": 1,
                                   "flags": 0})))
        self.assertEqual(f.u16(P.T_INDEX), 1)
        self.assertEqual(len(self.link.contact_list()), 2)

    def test_zones_and_import(self):
        self.link.connect()
        zones = self.link.zones()
        self.assertEqual(len(zones), 5)
        zc = self.link.zone_channel(2, 0)  # PMR1
        self.assertEqual(zc["name"], "PMR1")
        self.assertEqual(zc["rx"], 446006250)
        f = self.link.request(P.MSG_ZONE_IMPORT,
                              P.tlv_u8(P.T_ZONE, 2) + P.tlv_u16(P.T_INDEX, 0))
        self.assertEqual(self.link.chan_get(f.u16(P.T_INDEX))["name"], "PMR1")

    def test_settings_record_and_fields(self):
        self.link.connect()
        rec = self.link.settings_record()
        self.assertEqual(rec["version"], 8)
        self.assertEqual(len(rec["data"]), rec["size"])
        req = (P.tlv_str(P.T_SET_CALLSIGN, "EA5TEST") +
               P.tlv_u8(P.T_SET_SQLLEVEL, 5) +
               P.tlv_u32(P.T_SET_DMR_ID, 2143000))
        self.assertTrue(self.link.settings_set(req))
        f = self.link.settings_fields([P.T_SET_CALLSIGN, P.T_SET_DMR_ID,
                                       P.T_SET_SQLLEVEL])
        self.assertEqual(f.s(P.T_SET_CALLSIGN), "EA5TEST")
        self.assertEqual(f.u32(P.T_SET_DMR_ID), 2143000)
        self.assertEqual(f.u8(P.T_SET_SQLLEVEL), 5)

    def test_settings_out_of_range(self):
        self.link.connect()
        with self.assertRaises(P.ProtoError) as e:
            self.link.settings_set(P.tlv_u8(P.T_SET_BRIGHTNESS, 250))
        self.assertEqual(e.exception.code, 5)

    def test_status(self):
        self.link.connect()
        s = self.link.status()
        self.assertEqual(s["rx_freq"], 145500000)
        self.assertEqual(s["mode"], 1)
        self.assertEqual(s["rssi"], -73)
        self.assertEqual(s["tx_state"], 1)
        self.assertEqual(s["name"], "TEST1")
        # The power the radio would key with travels with the status.
        self.assertEqual(s["power_mw"], 500)
        self.assertEqual(s["tx_reason"], "none")

    # -- events ------------------------------------------------------------

    def test_events_arrive(self):
        self.link.connect()
        seen = set()
        deadline = time.time() + 3
        while time.time() < deadline and len(seen) < 3:
            for ev in self.link.pump(0.3):
                seen.add(ev.msg_id)
        self.assertIn(P.EVT_CHANNEL, seen)
        self.assertIn(P.EVT_PTT, seen)
        self.assertIn(P.EVT_MESSAGE, seen)

    def test_status_subscription(self):
        self.link.connect()
        self.assertEqual(self.link.subscribe_status(200), 200)
        got = [e for e in self.link.pump(1.2) if e.msg_id == P.EVT_STATUS]
        self.assertGreaterEqual(len(got), 2)

class ProtocolTest(RadioTest):
    """The debug namespace, on a build that has it."""

    expect_debug = True

    def test_log_stream_and_sequence(self):
        self.link.connect()
        f = self.link.log_subscribe(level=2, rate=50, module="")
        self.assertEqual(f.u16(P.T_RATE), 50)
        lines = []
        # Two rounds minimum, however fast the first one fills up: a client
        # that hands the same event out twice only shows it when a second
        # poll follows the first, which is why this test used to pass or fail
        # depending on how many lines the first 300 ms happened to carry.
        rounds = 0
        deadline = time.time() + 3
        while time.time() < deadline and (len(lines) < 4 or rounds < 2):
            rounds += 1
            for ev in self.link.pump(0.3):
                if ev.msg_id == P.EVT_LOG:
                    lines.append(ev)
        self.assertGreaterEqual(len(lines), 4)
        # Timestamped, numbered, and the numbers only go up.
        seqs = [ev.u32(P.T_LOG_SEQ) for ev in lines]
        self.assertEqual(seqs, sorted(seqs))
        # A number that arrives twice is one line delivered twice: the ring
        # numbers every line once and the client queue hands it out once.
        self.assertEqual(len(seqs), len(set(seqs)),
                         "log lines delivered twice: %r" % (seqs,))
        self.assertTrue(all(ev.u32(P.T_UPTIME_MS) >= 0 for ev in lines))
        self.assertTrue(any(ev.s(P.T_TEXT).startswith("tick")
                            for ev in lines))

    def test_log_module_filter(self):
        self.link.connect()
        # Nothing in this radio logs under "audio" any more: a subscription to
        # that tag must stay silent, which is the filter doing its job.
        self.link.log_subscribe(level=3, rate=50, module="audio")
        self.assertFalse([e for e in self.link.pump(1.2)
                          if e.msg_id == P.EVT_LOG])
        # And without a filter the same radio does talk.
        self.link.log_subscribe(level=2, rate=50, module="")
        self.assertTrue([e for e in self.link.pump(1.2)
                         if e.msg_id == P.EVT_LOG])

    def test_diag_fields(self):
        self.link.connect()
        d = self.link.diag()
        self.assertEqual(d["firmware"], "host-test")
        self.assertEqual(d["protocol"], P.PROTO_VERSION)
        self.assertGreater(d["uptime_ms"], 0)
        self.assertEqual(d["reset_cause"], 0x40)
        self.assertEqual(d["wdt_resets"], 1)
        self.assertEqual(d["heap_free"], 12345)
        self.assertEqual(d["heap_total"], 40960)
        self.assertEqual(d["psram_total"], 4194304)
        self.assertEqual(d["flash_used"], 480000)
        self.assertGreaterEqual(d["ram_total"], 262144)
        self.assertEqual(len(d["tasks"]), 2)
        self.assertEqual(d["tasks"][0]["name"], "main")
        self.assertLessEqual(d["tasks"][0]["unused"], d["tasks"][0]["size"])

    def test_register_read_and_guarded_write(self):
        self.link.connect()
        self.assertEqual(self.link.reg_read(0, 0x30), 0x1234)

        # Whitelisted, token and confirmation: accepted and read back.
        value, ok = self.link.reg_write(0, 0x30, 0x4321)
        self.assertEqual(value, 0x4321)
        self.assertTrue(ok)
        self.assertEqual(self.link.reg_read(0, 0x30), 0x4321)

        # Readable but not writable.
        with self.assertRaises(P.ProtoError) as e:
            self.link.reg_write(0, 0x10, 0x1111)
        self.assertEqual(e.exception.code, 5)

        # No confirmation.
        with self.assertRaises(P.ProtoError) as e:
            self.link.reg_write(0, 0x30, 0x1111, confirm=False)
        self.assertEqual(e.exception.code, 1)

        # A stale token is refused.
        self.link.token = (self.link.token ^ 0xDEAD) & 0xFFFFFFFF
        with self.assertRaises(P.ProtoError) as e:
            self.link.reg_write(0, 0x30, 0x1111)
        self.assertEqual(e.exception.code, 11)  # not_allowed

    def test_memory_read_is_bounded(self):
        self.link.connect()
        window = 0x40000000

        # Outside the window the platform hands over: refused, no data.
        with self.assertRaises(P.ProtoError) as e:
            self.link.mem_read(0x20000000, 16)
        self.assertEqual(e.exception.code, 5)

        # Inside it, and the length is clamped whatever we ask for.
        data = self.link.mem_read(window, 200)
        self.assertEqual(len(data), 128)
        self.assertEqual(data[0], 0)
        self.assertEqual(data[1], 1)

        # Without the token a read is not allowed at all.
        saved, self.link.token = self.link.token, 0
        try:
            with self.assertRaises(P.ProtoError) as e:
                self.link.mem_read(window, 16)
            self.assertEqual(e.exception.code, 11)
        finally:
            self.link.token = saved

    def test_capture_roundtrip(self):
        self.link.connect()
        data = self.link.capture(256)
        self.assertEqual(len(data), 512)
        samples = [int.from_bytes(data[i:i + 2], "little", signed=True)
                   for i in range(0, len(data), 2)]
        self.assertEqual(samples[0], 0)
        self.assertEqual(samples[1], 3)

    def test_baseband_and_tx_trace(self):
        self.link.connect()
        self.assertEqual(self.link.bb() & 0x80, 0)
        self.assertEqual(self.link.bb(1) & 0x03, 1)  # arm
        self.assertEqual(self.link.bb(3) & 0x03, 2)  # pause
        self.assertIn("tx stage=idle", self.link.tx_trace())


class ProductionTest(RadioTest):
    """Same device built without CONFIG_C62_DEBUG_PROTO: only the refusal."""

    def test_hello_without_debug_token(self):
        self.link.connect()
        self.assertEqual(self.link.token, 0)

    def test_every_debug_message_is_refused(self):
        self.link.connect()
        ids = [P.MSG_DBG_INFO, P.MSG_DBG_LOG_SUB, P.MSG_DBG_LOG_DRAIN,
               P.MSG_DBG_REG_READ, P.MSG_DBG_REG_WRITE, P.MSG_DBG_AGC,
               P.MSG_DBG_MEM_READ, P.MSG_DBG_CAP_START, P.MSG_DBG_CAP_STOP,
               P.MSG_DBG_CAP_READ, P.MSG_DBG_BB, P.MSG_DBG_TX_TRACE,
               P.MSG_DBG_RX_COUNTERS]
        payload = (P.tlv_u8(P.T_CHIP, 0) + P.tlv_u8(P.T_REG, 0x30) +
                   P.tlv_u16(P.T_VALUE, 0x1234) + P.tlv_u8(P.T_CONFIRM, 1) +
                   P.tlv_u32(P.T_TOKEN, 0xDEADBEEF) +
                   P.tlv_u32(P.T_ADDR, 0x40000000) +
                   P.tlv_u16(P.T_LENGTH, 16))
        for msg_id in ids:
            with self.assertRaises(P.ProtoError) as e:
                self.link.request(msg_id, payload)
            self.assertEqual(e.exception.code, 11,
                             "%s answered %s" % (hex(msg_id),
                                                 e.exception))

    def test_refusal_carries_no_data(self):
        self.link.connect()
        try:
            self.link.request(P.MSG_DBG_INFO, b"")
            self.fail("DBG_INFO answered")
        except P.ProtoError:
            pass
        # Nothing of the radio leaks through an event either.
        evs = self.link.pump(0.4)
        self.assertFalse([e for e in evs if e.msg_id == P.EVT_LOG])

    def test_stores_still_served(self):
        self.link.connect()
        self.assertEqual(self.link.chan_count(), 2)
        self.assertEqual(len(self.link.chan_list()), 2)


class BootShapeTest(unittest.TestCase):
    """
    The radio the firmware actually boots as, and the cable has to survive it.

    Two shapes, both of them reported from hardware, and one failure mode that
    used to take the whole protocol down:

      --boot-channel   a stored channel applied at boot (PC_MSG_CHAN_LASTUSED
                       left at 0).  On the real radio the screen and the keys
                       kept working and every protocol message, HELLO included,
                       got no answer at all, with no fault to show for it.
      --vfo            a VFO with no channel power and no transmit frequency of
                       its own: the PTT must still have a frequency and a
                       power to go to, and say 0 only when the setting is 0.
      --hold-lock-ms   the state lock held by another thread, which is what the
                       UI does while it runs its FSM: the protocol has to keep
                       answering, with PC_ERR_BUSY where a write really needs
                       the lock, never with silence.
    """

    binary = None

    def _radio(self, *args):
        radio = Radio(self.binary, args)
        radio.link.connect()
        return radio

    def test_boot_on_a_stored_channel_still_serves_hello(self):
        radio = self._radio("--boot-channel")
        try:
            h = radio.link.connect()
            self.assertEqual(h.u16(P.T_LASTUSED), 0)
            self.assertEqual(h.u16(P.T_CHCOUNT), 2)
            s = radio.link.status()
            self.assertEqual(s["rx_freq"], 145500000)
            self.assertEqual(s["power_mw"], 1000)   # the channel's own
        finally:
            radio.close()

    def test_vfo_transmit_frequency_and_power(self):
        radio = self._radio("--vfo")
        try:
            s = radio.link.status()
            # No transmit frequency in the record: simplex, the RX frequency.
            self.assertEqual(s["tx_freq"], 145500000)
            # No channel power: the setting, never zero.
            self.assertEqual(s["power_mw"], 500)
            self.assertEqual(s["tx_reason"], "none")
        finally:
            radio.close()

    def test_vfo_with_zero_power_says_why(self):
        radio = self._radio("--vfo", "--zero-power")
        try:
            s = radio.link.status()
            self.assertEqual(s["tx_freq"], 145500000)
            self.assertEqual(s["power_mw"], 0)
            self.assertEqual(s["tx_reason"], "no tx power")
        finally:
            radio.close()

    def test_protocol_answers_with_the_state_lock_held(self):
        # Longer than any timeout the handlers use, so a plain lock would park
        # the transport for the whole test.
        radio = self._radio("--hold-lock-ms", "1500")
        try:
            t0 = time.time()
            # HELLO and STATUS take no lock at all: they answer at once.
            s = radio.link.status()
            self.assertLess(time.time() - t0, 1.0)
            self.assertEqual(s["rx_freq"], 145500000)
            # The bench's instrument case: STATUS while the radio is busy has
            # to answer inside 100 ms, every time -- a status that waits for
            # the UI (or for the transmit path) is a measurement that cannot
            # be taken while transmitting.
            worst = 0.0
            for _ in range(10):
                t = time.monotonic()
                radio.link.status()
                worst = max(worst, time.monotonic() - t)
            self.assertLess(worst, 0.1,
                            "status took %.0f ms with the state lock held"
                            % (worst * 1000.0))
            # A read that does need the lock answers BUSY, not silence, and
            # the link is still alive afterwards.
            busy = False
            for _ in range(3):
                try:
                    radio.link.settings_record()
                except P.ProtoError as exc:
                    busy = busy or exc.code == 8   # PC_ERR_BUSY
                time.sleep(0.1)
            self.assertTrue(busy, "a held state lock must answer PC_ERR_BUSY")
            self.assertEqual(radio.link.status()["mode"], 1)
        finally:
            radio.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True)
    ap.add_argument("--device-nodebug", required=True)
    args, rest = ap.parse_known_args()

    ProtocolTest.binary = args.device
    ProductionTest.binary = args.device_nodebug
    BootShapeTest.binary = args.device

    suite = unittest.TestSuite()
    for case in (ProtocolTest, ProductionTest, BootShapeTest):
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(case))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
