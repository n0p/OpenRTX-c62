#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Client of the bench namespace of the radio protocol (docs/BENCH.md).

This is the layer between the cable and the measurement cases: it speaks the
same framing, session and TLV codec as the CPS client (tools/cps/pcproto.py,
imported and not forked) and adds the seven bench messages plus the thing that
makes a measurement possible at all -- a record of when each answer arrived on
this side, so the radio's tick and the PC's clock can be lined up afterwards.

The time line, in one paragraph: the radio stamps every answer and every TX/RX
edge with its own milliseconds-since-boot.  We stamp the arrival here with
time.monotonic_ns() and keep the round trip.  A least-squares fit over those
pairs (correlate.fit_clock) gives the offset and the rate difference of the two
clocks; the capture carries its own t0 and 48 kHz sample index, so the samples
land on that line exactly.  Nothing is assumed about the radio's crystal.

What this module does NOT do: measure.  It moves the radio and records what it
says and when.  The measuring is in correlate.py, and the instruments are in
sdr.py.
"""

import os
import struct
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "cps"))

import pcproto as P  # noqa: E402  (the CPS client's framing and session)

# --- message identifiers (core/pc_proto.h, enum pcProtoMsg) -----------------

MSG_BENCH_ARM = 0x0601
MSG_BENCH_TUNE = 0x0602
MSG_BENCH_PTT = 0x0603
MSG_BENCH_CAPTURE = 0x0604
MSG_BENCH_RSSI = 0x0605
MSG_BENCH_SWEEP = 0x0606
MSG_BENCH_REPORT = 0x0607

EVT_BENCH_EDGE = 0x0086

CAP_BENCH = 0x00004000
CAP_DEBUG = 0x00002000

# --- tags (enum pcProtoTag) ------------------------------------------------

T_EPOCH = 0x0100
T_TICK = 0x0101
T_T0 = 0x0102
T_WRITTEN = 0x0103
T_TONE = 0x0104
T_LEVEL = 0x0105
T_BW = 0x0106
T_BLOCK = 0x0107
# The transmit power of the run, mW: TUNE in, TUNE/PTT/ARM out
# (docs/BENCH.md).  A run that does not send it keys with the radio's own
# power, and one whose radio has none is refused at PTT instead of keying at
# 0 mW.
T_POWER = 0x0113
T_BLOCKS = 0x0108
T_STEP = 0x0109
T_F0 = 0x010A
T_F1 = 0x010B
T_DWELL = 0x010C
T_POINTS = 0x010D
T_DELTA = 0x010E
T_WINDOW = 0x010F
T_RSSI_TS = 0x0110
T_POINTS_TS = 0x0111
T_OVERFLOW = 0x0112

# Tags that already existed in the core protocol.
T_STATUS = P.T_STATUS
T_TOKEN = P.T_TOKEN
T_RX_FREQ = P.T_RX_FREQ
T_TX_FREQ = P.T_TX_FREQ
T_MODE = P.T_MODE
T_TX_STATE = P.T_TX_STATE
T_RATE = P.T_RATE
T_LENGTH = P.T_LENGTH
T_OFFSET = P.T_OFFSET
T_COUNT = P.T_COUNT
T_DATA = P.T_DATA
T_TEXT = P.T_TEXT
T_RUNNING = P.T_RUNNING
T_RSSI = P.T_RSSI

MSG_NAMES = {
    MSG_BENCH_ARM: "bench.arm",
    MSG_BENCH_TUNE: "bench.tune",
    MSG_BENCH_PTT: "bench.ptt",
    MSG_BENCH_CAPTURE: "bench.capture",
    MSG_BENCH_RSSI: "bench.rssi",
    MSG_BENCH_SWEEP: "bench.sweep",
    MSG_BENCH_REPORT: "bench.report",
}

# Bandwidth on the wire: 0 keeps it, else enum bandwidth + 1.
BW_12K5 = 1
BW_25K = 2
BW_6K25 = 3


class BenchError(P.ProtoError):
    """A bench request the radio refused."""


class BenchLink(P.Link):
    """
    A session on the cable with the bench namespace on top.

    Every answer is recorded as (radio tick, host time, round trip) in
    `clock_pairs`, which is what correlate.fit_clock() turns into an offset.
    """

    def __init__(self, port, verbose=False):
        super().__init__(port, client_caps=P.CAP_EVENTS, verbose=verbose)
        self.clock_pairs = []   # (radio_ms, host_ns_mid, rtt_ns)
        self.last = {}          # last answer per message, for the report
        # Power every TUNE carries unless the case asks for another one.  The
        # CLI sets it from --power (500 mW by default): a bench run keys the
        # PA, and on a VFO the radio has no channel power to fall back on
        # (docs/BENCH.md, docs/TX_SAFETY.md).
        self.default_power = 0

    # -- the part that makes it a measurement ------------------------------

    def _request(self, msg_id, payload=b"", timeout=None):
        t0 = time.monotonic_ns()
        r = self.request(msg_id, payload, timeout=timeout)
        t1 = time.monotonic_ns()
        tick = r.u32(T_TICK, None)
        if tick is None:
            tick = r.u32(P.T_UPTIME_MS, None)
        if tick is not None:
            # The radio stamped before it answered, so the middle of the round
            # trip is the best estimate of when that stamp happened here.
            self.clock_pairs.append((tick, (t0 + t1) // 2, t1 - t0))
        self.last[msg_id] = r
        return r

    def token_tlv(self):
        if not self.token:
            raise P.ProtoError(
                11, None,
                "no session token: the firmware needs C62_DEBUG_PROTO and "
                "C62_BENCH_PROTO")
        return P.tlv_u32(T_TOKEN, self.token)

    def require_bench(self):
        if not self.has(CAP_BENCH):
            raise P.ProtoError(
                11, None,
                "this firmware has no bench namespace (C62_BENCH_PROTO off)")

    # -- the seven messages ------------------------------------------------

    def arm(self):
        self.require_bench()
        f = self._request(MSG_BENCH_ARM, self.token_tlv())
        return {
            "epoch": f.u32(T_EPOCH),
            "tick": f.u32(T_TICK),
            "rate": f.u16(T_RATE),
            "max_ms": f.u16(T_LENGTH),
            "power_mw": f.u16(T_POWER),
        }

    def tune(self, freq, mode=None, bw=None, tx=None, power=None):
        self.require_bench()
        p = P.tlv_u32(T_RX_FREQ, int(freq))
        if tx:
            p += P.tlv_u32(T_TX_FREQ, int(tx))
        if mode is not None:
            p += P.tlv_u8(T_MODE, mode)
        if bw is not None:
            p += P.tlv_u8(T_BW, bw)
        if power is None:
            power = self.default_power
        if power:
            p += P.tlv_u16(T_POWER, int(power))
        f = self._request(MSG_BENCH_TUNE, p + self.token_tlv())
        return {
            "rx": f.u32(T_RX_FREQ),
            "tx": f.u32(T_TX_FREQ),
            "mode": f.u8(T_MODE),
            "bw": f.u8(T_BW),
            "power_mw": f.u16(T_POWER),
            "delta_hz": struct.unpack("<i", struct.pack("<I", f.u32(T_DELTA)))[0],
            "tick": f.u32(T_TICK),
        }

    def ptt(self, on, tone=None, level=None):
        self.require_bench()
        p = P.tlv_u8(T_TX_STATE, 1 if on else 0)
        if tone is not None:
            p += P.tlv_u16(T_TONE, int(tone))
        if level is not None:
            p += P.tlv_u8(T_LEVEL, int(level))
        # The answer comes first and the TX/RX edge event right behind it; the
        # link queues the event as it reads, so it lands in self.events for
        # bench_edges() without any extra work here.
        f = self._request(MSG_BENCH_PTT, p + self.token_tlv())
        return {
            "tx_state": f.u8(T_TX_STATE),
            "tone": f.u16(T_TONE),
            "level": f.u8(T_LEVEL),
            "power_mw": f.u16(T_POWER),
            "tick": f.u32(T_TICK),
        }

    def capture_arm(self, ms):
        self.require_bench()
        f = self._request(MSG_BENCH_CAPTURE,
                          P.tlv_u16(T_WINDOW, int(ms)) + self.token_tlv())
        return self._capture_meta(f)

    def capture_status(self):
        f = self._request(MSG_BENCH_CAPTURE, self.token_tlv())
        return self._capture_meta(f)

    def _capture_meta(self, f):
        return {
            "t0": f.u32(T_T0),
            "rate": f.u16(T_RATE),
            "written": f.u32(T_WRITTEN),
            "window_ms": f.u16(T_WINDOW),
            "block": f.u16(T_BLOCK),
            "blocks": f.u32(T_BLOCKS),
            "running": bool(f.u8(T_RUNNING)),
            "overflow": bool(f.u8(T_OVERFLOW)),
        }

    def capture_read(self, offset, count=128):
        f = self._request(MSG_BENCH_CAPTURE,
                          P.tlv_u32(T_OFFSET, offset) +
                          P.tlv_u16(T_LENGTH, count) + self.token_tlv())
        data = f.get(T_DATA, b"")
        return data, self._capture_meta(f)

    def capture(self, ms, timeout=None):
        """
        Arm a window of `ms` milliseconds, wait for it to fill and return
        (samples as int16 list, meta).  The meta carries t0, the rate and the
        block shape, which is what puts the samples on the time line.
        """
        meta = self.capture_arm(ms)
        want = (int(ms) * meta["rate"]) // 1000 if meta["rate"] else 0
        deadline = time.time() + (timeout if timeout is not None else
                                  max(2.0, ms / 1000.0 * 3 + 1))
        while True:
            st = self.capture_status()
            meta = st
            if not st["running"] or (want and st["written"] >= want):
                break
            if time.time() > deadline:
                break
            time.sleep(0.02)

        raw = bytearray()
        off = 0
        while off < meta["written"]:
            n = min(128, meta["written"] - off)
            chunk, m = self.capture_read(off, n)
            if not chunk:
                break
            raw += chunk
            off += len(chunk) // 2
            meta.update(m)
        samples = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
        return samples, meta

    def rssi(self, n=1, timeout=None):
        self.require_bench()
        f = self._request(MSG_BENCH_RSSI,
                          P.tlv_u8(T_COUNT, n) + self.token_tlv())
        deadline = time.time() + (timeout if timeout is not None else
                                  (n * 0.01) + 2.0)
        while True:
            got = self._rssi_list(f)
            if len(got) >= n or time.time() > deadline:
                return got
            time.sleep(0.02)
            # A poll carries no COUNT: it reads the series, it does not
            # restart it.
            f = self._request(MSG_BENCH_RSSI, self.token_tlv())

    def _rssi_list(self, f):
        raw = f.get(T_RSSI_TS, b"")
        out = []
        for i in range(0, len(raw) - 5, 6):
            tick, dbm = struct.unpack("<Ih", raw[i:i + 6])
            out.append((tick, dbm))
        return out

    def sweep(self, f0, f1, step, dwell_ms=100, timeout=None):
        self.require_bench()
        f = self._request(MSG_BENCH_SWEEP,
                          P.tlv_u32(T_F0, int(f0)) + P.tlv_u32(T_F1, int(f1)) +
                          P.tlv_u32(T_STEP, int(step)) +
                          P.tlv_u16(T_DWELL, int(dwell_ms)) + self.token_tlv())
        points = self._sweep_points(f)
        total = f.u16(T_POINTS)
        deadline = time.time() + (timeout if timeout is not None
                                  else total * (dwell_ms / 1000.0 + 0.03) + 5)
        meta = {"f0": f.u32(T_F0), "f1": f.u32(T_F1), "step": f.u32(T_STEP),
                "dwell": f.u16(T_DWELL), "total": total, "t0": f.u32(T_T0)}
        while len(points) < total and time.time() < deadline:
            time.sleep(0.02)
            f = self._request(MSG_BENCH_SWEEP, self.token_tlv())
            meta["running"] = bool(f.u8(T_RUNNING))
            points += self._sweep_points(f)
            if not meta["running"] and len(points) >= f.u32(T_WRITTEN):
                break
        return points[:total], meta

    def _sweep_points(self, f):
        raw = f.get(T_POINTS_TS, b"")
        out = []
        for i in range(0, len(raw) - 5, 6):
            hz, dbm = struct.unpack("<Ih", raw[i:i + 6])
            out.append((hz, dbm))
        return out

    def report(self):
        text = ""
        off = 0
        while True:
            f = self._request(MSG_BENCH_REPORT,
                              P.tlv_u32(T_OFFSET, off) if off else b"")
            total = f.u16(T_LENGTH)
            chunk = f.get(T_TEXT, b"")
            text += chunk.decode("utf-8", "replace")
            off += len(chunk)
            if not chunk or len(text) >= total:
                return text

    # -- events ------------------------------------------------------------

    def bench_edges(self):
        """The TX/RX edge events received so far, as (tx_state, tick)."""
        out = []
        for ev in self.events:
            if ev.msg_id == EVT_BENCH_EDGE:
                out.append((ev.u8(T_TX_STATE), ev.u32(T_TICK)))
        return out


def open_link(device, baud=115200, timeout=2.0):
    """Open the cable and complete HELLO."""
    port = P.Port(device, baud=baud, timeout=timeout)
    link = BenchLink(port)
    link.connect()
    return link


def describe(link):
    """A one-screen summary of what the radio on the cable can do."""
    caps = link.caps
    names = []
    for bit, name in ((P.CAP_CHANNELS, "channels"), (P.CAP_ZONES, "zones"),
                      (CAP_DEBUG, "debug"), (CAP_BENCH, "bench")):
        if caps & bit:
            names.append(name)
    return {
        "model": link.hello.s(P.T_MODEL),
        "firmware": link.hello.s(P.T_FIRMWARE),
        "protocol": link.hello.u16(P.T_VERSION),
        "caps": " ".join(names),
        "bench": bool(caps & CAP_BENCH),
        "rate": link.hello.u16(T_RATE),
    }
