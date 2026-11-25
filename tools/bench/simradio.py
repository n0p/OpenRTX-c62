#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
A radio in the laptop: the bench namespace with no cable and no hardware.

It speaks the same protocol as BenchLink -- the same messages, the same TLV
payloads, encoded and decoded through tools/cps/pcproto.py -- so bench.py does
not know whether it is talking to a radio or to this.  What it does not share
is the interesting part: the sequencing, the timestamps and the report text are
the firmware's job, and this file re-implements just enough of them to run the
flow on a desk.

Being explicit about what this is: a model.  The capture it hands over is a
sine it synthesises, the RSSI is a triangle it computes from the frequency, and
the report text is produced here.  Anything a case measures against it is a
test of the measuring code, not a measurement of a radio.
"""

import math
import struct
import time

import pcproto as P

import benchproto as BP

RATE = 48000
TICK_MS = 10
TONE_HZ = 1000           # the tone the simulated receiver "hears"
PEAK_HZ = 145500000      # where the simulated carrier is strongest


class SimRadio:
    """The methods bench.py needs, with the protocol's answers."""

    name = "sim-radio"

    def __init__(self, tone_hz=TONE_HZ, peak_hz=PEAK_HZ, offset_hz=0.0,
                 noise_level=-95):
        self.tone_hz = tone_hz
        self.peak_hz = peak_hz
        self.offset_hz = offset_hz
        self.noise_level = noise_level

        self.t0 = time.monotonic()
        self.epoch = 0
        self.rx = peak_hz
        self.tx = peak_hz
        self.mode = 1
        self.bw = 2
        self.keyed = 0        # the state; ptt() is the method
        self.tone = 0
        self.level = 0
        self.ptt_on = 0
        self.ptt_off = 0
        self.tuned = None
        self.cap = None          # dict when a window is armed
        self.cap_sample = 0
        self.rssi_series = []
        self.rssi_wanted = 0
        self.sweep = None
        self.token = 0x51A3C0DE
        self.caps = BP.CAP_BENCH | P.CAP_EVENTS
        self.clock_pairs = []
        self.events = []

    # -- the clock ---------------------------------------------------------

    def tick(self):
        return int((time.monotonic() - self.t0) * 1000)

    def _stamp(self):
        """Record a (radio tick, host time, round trip) pair, as the real link
        does from the answers it receives.  The round trip here is a made-up
        1 ms: a local call has no cable."""
        host = time.monotonic_ns()
        self.clock_pairs.append((self.tick(), host, 1000000))
        return host

    # -- Link-compatible surface -------------------------------------------

    def has(self, cap):
        return bool(self.caps & cap)

    def require_bench(self):
        pass

    def connect(self):
        self.token = 0x51A3C0DE
        return self

    def close(self):
        pass

    def pump(self, seconds):
        return []

    def bench_edges(self):
        return list(self.events)

    # -- the seven messages ------------------------------------------------

    def arm(self):
        self._stamp()
        self.epoch += 1
        self.cap = None
        self.rssi_series = []
        self.rssi_wanted = 0
        self.sweep = None
        return {"epoch": self.epoch, "tick": self.tick(), "rate": RATE,
                "max_ms": 333}

    def tune(self, freq, mode=None, bw=None, tx=None):
        if freq < 18000000:
            raise P.ProtoError(5, BP.MSG_BENCH_TUNE, "frequency out of range")
        self.rx = int(freq)
        self.tx = int(tx or freq)
        if mode is not None:
            self.mode = mode
        if bw is not None:
            self.bw = bw
        self.tuned = self.tick()
        self._stamp()
        return {"rx": self.rx, "tx": self.tx, "mode": self.mode, "bw": self.bw,
                "delta_hz": 0, "tick": self.tuned}

    def ptt(self, on, tone=None, level=None):
        self.keyed = 1 if on else 0
        if on:
            self.tone = int(tone or 0) if 60 <= (tone or 0) <= 260 else 0
            self.level = int(level or 0)
            self.ptt_on = self.tick()
        else:
            self.ptt_off = self.tick()
            self.tone = 0
            self.level = 0
        self._stamp()
        self.events.append((self.keyed, self.tick()))
        return {"tx_state": 2 if self.keyed else 1, "tone": self.tone,
                "level": self.level, "tick": self.tick()}

    def capture_arm(self, ms):
        want = int(ms) * RATE // 1000
        self.cap = {"want": want, "written": 0, "done": False,
                    "t0": self.tick(), "ms": int(ms), "blocks": 0}
        return self.capture_status()

    def _fill(self):
        """Fill the armed window as the ticks go by: 10 ms per tick."""
        c = self.cap
        if not c or c["done"]:
            return
        due = min(c["want"], int((self.tick() - c["t0"]) * RATE / 1000))
        if due > c["written"]:
            c["blocks"] += (due - c["written"]) // 480
            c["written"] = due
        if c["written"] >= c["want"]:
            c["done"] = True

    def capture_status(self):
        if not self.cap:
            return {"t0": 0, "rate": RATE, "written": 0, "window_ms": 0,
                    "block": 0, "blocks": 0, "running": False,
                    "overflow": False}
        self._fill()
        c = self.cap
        return {"t0": c["t0"], "rate": RATE, "written": c["written"],
                "window_ms": c["ms"], "block": 480, "blocks": c["blocks"],
                "running": not c["done"],
                "overflow": c["written"] > c["want"]}

    def capture_read(self, offset, count=128):
        self._fill()
        c = self.cap or {}
        written = c.get("written", 0)
        t0 = c.get("t0", 0)
        n = max(0, min(count, written - offset))
        out = bytearray()
        for i in range(n):
            t = (offset + i) / RATE
            v = int(12000 * math.sin(2 * math.pi * self.tone_hz * t))
            out += struct.pack("<h", v)
        meta = {"t0": t0, "rate": RATE, "written": written,
                "window_ms": c.get("ms", 0), "block": 480,
                "blocks": c.get("blocks", 0),
                "running": not c.get("done", True), "overflow": False}
        return bytes(out), meta

    def capture(self, ms, timeout=None):
        self.capture_arm(ms)
        deadline = time.time() + 3
        while True:
            st = self.capture_status()
            if not st["running"] or time.time() > deadline:
                break
            time.sleep(0.01)
        raw = bytearray()
        off = 0
        while off < st["written"]:
            chunk, m = self.capture_read(off, 128)
            if not chunk:
                break
            raw += chunk
            off += len(chunk) // 2
        st.update(m)
        samples = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
        return samples, st

    def _rssi_now(self):
        d = abs(self.rx - self.peak_hz)
        level = self.noise_level + (40 - d // 5000 if d < 50000 else 0)
        return int(level)

    def rssi(self, n=1, timeout=None):
        self._stamp()
        self.rssi_wanted = n
        got = [(self.tick(), self._rssi_now())]
        deadline = time.time() + max(1.0, n * TICK_MS / 1000.0 + 1)
        while len(got) < n and time.time() < deadline:
            time.sleep(TICK_MS / 1000.0)
            got.append((self.tick(), self._rssi_now()))
        self.rssi_series = got
        return got

    def sweep(self, f0, f1, step, dwell_ms=100, timeout=None):
        self._stamp()
        points = []
        hz = int(f0)
        while hz < int(f1) and len(points) < 96:
            self.rx = hz
            points.append((hz, self._rssi_now()))
            hz += int(step)
        self.sweep = {"f0": f0, "f1": f1, "step": step, "dwell": dwell_ms,
                      "total": len(points), "t0": self.tick()}
        return points, self.sweep

    def report(self):
        return "\n".join([
            "bench run %d  (simulated radio)" % self.epoch,
            "tune   rx %d Hz tx %d Hz mode %d bw %d" % (self.rx, self.tx,
                                                        self.mode, self.bw),
            "ptt    %s on %d ms off %d ms tone %d Hz level %d" % (
                "keyed" if self.keyed else "off", self.ptt_on, self.ptt_off,
                self.tone, self.level),
            "capture %d ms at %d Hz t0 %d" % (
                (self.cap or {}).get("ms", 0), RATE,
                (self.cap or {}).get("t0", 0)),
            "rssi   %d samples" % len(self.rssi_series),
            "sweep  %d points" % (len(self.sweep and [1] or []) or 0),
            "note   this is a simulator: nothing here was measured",
        ]) + "\n"


def open_sim_radio(**kw):
    return SimRadio(**kw)
