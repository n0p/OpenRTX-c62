#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Receivers, behind one interface.

A bench has whatever instrument it has, and the instruments do not measure the
same things.  Rather than pretend they do, each backend declares what it can
produce and the cases ask for a kind:

  IQ        complex samples at a sample rate  -> correlate.py does the rest
  SPECTRUM  a power trace in dBm against frequency
  ENVELOPE  power in dBm against time (zero span)

| backend  | IQ | SPECTRUM | ENVELOPE | notes                            |
|----------|----|----------|----------|----------------------------------|
| rtl      | x  | -        | -        | RTL-SDR via the rtl_sdr CLI      |
| soapy    | x  | -        | -        | any SoapySDR device, if the      |
|          |    |          |          | python bindings are installed    |
| anritsu  | -  | x        | x        | MS2090A over SCPI; no I/Q on our |
|          |    |          |          | unit (licence 0124 not bought)   |
| sim      | x  | x        | x        | synthetic, for the laptop        |

An RTL-SDR cannot transmit and neither can the analyser, which is why the
sensitivity case is written the way it is: the instrument measures the level,
the radio reports its own CRC errors (docs/BENCH.md, case 5).
"""

import math
import socket
import subprocess
import time

import numpy as np

KIND_IQ = "iq"
KIND_SPECTRUM = "spectrum"
KIND_ENVELOPE = "envelope"


class Capture:
    """One take from an instrument, with what is needed to interpret it."""

    def __init__(self, kind, data, rate_hz=None, center_hz=None, meta=None):
        self.kind = kind
        self.data = data            # np.complex64 | (freqs, dbm) | (t, dbm)
        self.rate_hz = rate_hz
        self.center_hz = center_hz
        self.meta = meta or {}
        self.t_wall_ns = time.monotonic_ns()

    def __repr__(self):
        if self.kind == KIND_IQ:
            return "<iq %.3f Ms/s %d samples @ %.6f MHz>" % (
                (self.rate_hz or 0) / 1e6, len(self.data),
                (self.center_hz or 0) / 1e6)
        return "<%s %d points>" % (self.kind, len(self.data[0]))


class BackendError(RuntimeError):
    pass


class Backend:
    name = "none"
    kinds = ()

    def has(self, kind):
        return kind in self.kinds

    def bandwidth_hint_hz(self):
        """A sane capture bandwidth for narrowband work, in Hz."""
        return 240000.0

    def capture(self, kind, center_hz, seconds, rate_hz=None, gain_db=None):
        raise NotImplementedError

    def close(self):
        pass


# ---------------------------------------------------------------------------
# RTL-SDR
# ---------------------------------------------------------------------------

class RtlSdr(Backend):
    """
    The cheap one, and the one that is always in the bag.

    8-bit I/Q at up to 2.4 Ms/s, no absolute calibration, no transmit.  Driven
    through the rtl_sdr CLI rather than a python binding: one binary, no
    build, and its output is exactly the interleaved bytes we want.
    """

    name = "rtl"
    kinds = (KIND_IQ,)

    def __init__(self, device_index=0, ppm=0, gain_db=None):
        self.index = device_index
        self.ppm = ppm
        self.gain = gain_db

    def bandwidth_hint_hz(self):
        # 240 kHz: enough for a 25 kHz channel with room for the skirt, small
        # enough that 8 bits are not spent on spectrum nobody asks about.
        return 240000.0

    def capture(self, kind, center_hz, seconds, rate_hz=None, gain_db=None):
        if kind != KIND_IQ:
            raise BackendError("an RTL-SDR only produces I/Q")
        fs = float(rate_hz or self.bandwidth_hint_hz())
        if fs < 225001 or fs > 3200000:
            raise BackendError("RTL-SDR sample rate out of range: %g" % fs)
        n = int(fs * seconds)
        n -= n % 2
        cmd = ["rtl_sdr", "-f", "%d" % int(center_hz), "-s", "%d" % int(fs),
               "-n", "%d" % n, "-d", "%d" % self.index]
        if self.ppm:
            cmd += ["-p", "%d" % self.ppm]
        g = gain_db if gain_db is not None else self.gain
        if g is not None:
            cmd += ["-g", "%s" % g]
        cmd.append("-")
        try:
            out = subprocess.run(cmd, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=seconds + 20)
        except FileNotFoundError:
            raise BackendError("rtl_sdr not installed (pacman -S rtl-sdr)")
        except subprocess.TimeoutExpired:
            raise BackendError("rtl_sdr did not finish in time")
        if out.returncode != 0 or len(out.stdout) < 2:
            raise BackendError("rtl_sdr failed: %s" %
                               out.stderr.decode("utf-8", "replace").strip())
        raw = np.frombuffer(out.stdout, dtype=np.uint8).astype(np.float32)
        raw = (raw - 127.5) / 127.5
        iq = raw[0::2] + 1j * raw[1::2]
        return Capture(KIND_IQ, iq.astype(np.complex64), rate_hz=fs,
                       center_hz=float(center_hz),
                       meta={"backend": "rtl", "ppm": self.ppm,
                             "gain": g, "dropped": out.stderr.decode(
                                 "utf-8", "replace").count("lost")})


# ---------------------------------------------------------------------------
# SoapySDR
# ---------------------------------------------------------------------------

class Soapy(Backend):
    """Any device SoapySDR knows, if its python bindings are installed."""

    name = "soapy"
    kinds = (KIND_IQ,)

    def __init__(self, args=""):
        try:
            import SoapySDR  # noqa: F401
        except ImportError:
            raise BackendError(
                "python SoapySDR bindings not installed: use the RTL-SDR "
                "backend or install SoapySDR's python module")
        self.args = args

    def capture(self, kind, center_hz, seconds, rate_hz=None, gain_db=None):
        if kind != KIND_IQ:
            raise BackendError("SoapySDR backends produce I/Q only")
        import SoapySDR
        from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32
        fs = float(rate_hz or self.bandwidth_hint_hz())
        dev = SoapySDR.Device(self.args) if self.args else \
            SoapySDR.Device(SoapySDR.Device.enumerate()[0])
        dev.setSampleRate(SOAPY_SDR_RX, 0, fs)
        dev.setFrequency(SOAPY_SDR_RX, 0, center_hz)
        if gain_db is not None:
            dev.setGain(SOAPY_SDR_RX, 0, gain_db)
        stream = dev.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32)
        dev.activateStream(stream)
        n = int(fs * seconds)
        buf = np.zeros(4096, dtype=np.complex64)
        chunks = []
        got = 0
        while got < n:
            sr = dev.readStream(stream, [buf], len(buf), timeoutUs=200000)
            if sr.ret > 0:
                chunks.append(buf[:sr.ret].copy())
                got += sr.ret
            elif sr.ret < 0 and sr.ret != -1:
                break
        dev.deactivateStream(stream)
        dev.closeStream(stream)
        data = np.concatenate(chunks) if chunks else np.zeros(0, np.complex64)
        return Capture(KIND_IQ, data, rate_hz=fs, center_hz=float(center_hz),
                       meta={"backend": "soapy"})


# ---------------------------------------------------------------------------
# Anritsu MS2090A over SCPI
# ---------------------------------------------------------------------------

class Anritsu(Backend):
    """
    The analyser, over a raw SCPI socket.

    It gives absolute power in dBm (calibrated, unlike the RTL-SDR) and it can
    hold a frequency to a hertz, which is what the carrier error case wants.
    What it cannot give is I/Q: the IQ capture commands exist in the firmware
    of our unit but answer -203, the 0124 option is not installed.  So no
    deviation, no demodulated audio, no edges from I/Q -- the envelope case
    uses its zero-span mode instead.
    """

    name = "anritsu"
    kinds = (KIND_SPECTRUM, KIND_ENVELOPE)

    def __init__(self, host="10.205.52.71", port=9001, timeout=15.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self._buf = b""

    # -- transport ---------------------------------------------------------

    def _connect(self):
        if self.sock is not None:
            return
        self.sock = socket.create_connection((self.host, self.port),
                                             timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        self.send("*CLS")
        self.send(":TRAC1:TYPE NORM")   # a max-hold left behind by someone
        self._idn = self.query("*IDN?")  # else reads a frozen ghost

    def send(self, cmd):
        self._connect()
        self.sock.sendall(cmd.encode() + b"\n")

    def query(self, cmd):
        self.send(cmd)
        return self._readline()

    def _readline(self):
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise BackendError("analyser closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("latin-1").strip()

    def _read_block(self):
        """Read one IEEE #A<len><data> block: the trace comes back without a
        trailing newline, so a readline() here dies on a timeout."""
        self._connect()
        while not self._buf.startswith(b"#"):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise BackendError("analyser closed the connection")
            self._buf += chunk
        ndig = int(chr(self._buf[1]))
        while len(self._buf) < 2 + ndig:
            self._buf += self.sock.recv(65536)
        length = int(self._buf[2:2 + ndig])
        while len(self._buf) < 2 + ndig + length:
            self._buf += self.sock.recv(65536)
        body = self._buf[2 + ndig:2 + ndig + length]
        self._buf = self._buf[2 + ndig + length:]
        return body

    # -- measurements ------------------------------------------------------

    def spectrum(self, center_hz, span_hz, rbw_hz=None, points=None):
        self.send(":FREQ:CENT %.0f" % center_hz)
        self.send(":FREQ:SPAN %.0f" % span_hz)
        if rbw_hz:
            self.send(":BAND:RES %.0f" % rbw_hz)
        self.send(":INIT:CONT 0")
        self.send(":INIT")
        self.query("*OPC?")
        self.send(":TRAC:DATA? 1")
        body = self._read_block()
        dbm = np.array([float(x) for x in
                        body.decode("latin-1").replace("\n", "").split(",")
                        if x.strip()], dtype=np.float64)
        n = len(dbm)
        f0 = center_hz - span_hz / 2.0
        freqs = f0 + np.arange(n) * (span_hz / max(n - 1, 1))
        return Capture(KIND_SPECTRUM, (freqs, dbm), rate_hz=None,
                       center_hz=center_hz,
                       meta={"backend": "anritsu", "span": span_hz,
                             "rbw": rbw_hz, "points": n})

    def envelope(self, center_hz, seconds, rbw_hz=3e6):
        """Zero span: power against time, for edge timing."""
        self.send(":FREQ:CENT %.0f" % center_hz)
        self.send(":FREQ:SPAN 0")
        self.send(":BAND:RES %.0f" % rbw_hz)
        self.send(":INIT:CONT 0")
        self.send(":INIT")
        self.query("*OPC?")
        self.send(":TRAC:DATA? 1")
        body = self._read_block()
        dbm = np.array([float(x) for x in
                        body.decode("latin-1").replace("\n", "").split(",")
                        if x.strip()], dtype=np.float64)
        t = np.linspace(0.0, seconds, len(dbm))
        return Capture(KIND_ENVELOPE, (t, dbm), rate_hz=len(dbm) / seconds,
                       center_hz=center_hz,
                       meta={"backend": "anritsu", "zero_span": True,
                             "rbw": rbw_hz,
                             "note": "the time axis is the sweep, calibrated "
                                     "by the operator"})

    def capture(self, kind, center_hz, seconds, rate_hz=None, gain_db=None,
                span_hz=None, rbw_hz=None):
        if kind == KIND_SPECTRUM:
            span = span_hz or 500000.0
            return self.spectrum(center_hz, span, rbw_hz=rbw_hz)
        if kind == KIND_ENVELOPE:
            return self.envelope(center_hz, seconds, rbw_hz=rbw_hz or 3e6)
        raise BackendError("the analyser has no I/Q on this unit "
                           "(option 0124 not installed): use a spectrum or a "
                           "zero-span envelope")

    def close(self):
        if self.sock is not None:
            try:
                self.send(":INIT:CONT 1")
            except Exception:
                pass
            self.sock.close()
            self.sock = None


# ---------------------------------------------------------------------------
# The simulator
# ---------------------------------------------------------------------------

class SimTruth:
    """
    What the air is doing, for the simulator.

    The numbers here are the ones a case is supposed to recover, so a test can
    assert that the measuring code gives them back.  They are a model, not a
    measurement, and the report says so.
    """

    def __init__(self, freq_hz=145.5e6, offset_hz=0.0, amplitude=0.30,
                 tone_hz=0.0, deviation_hz=0.0, t_on=0.10, t_off=0.40,
                 noise_db=-80.0, seconds=1.0, peak_dbm=-40.0,
                 always_on=False):
        self.freq_hz = freq_hz
        self.offset_hz = offset_hz
        self.amplitude = amplitude
        self.tone_hz = tone_hz
        self.deviation_hz = deviation_hz
        self.t_on = t_on
        self.t_off = t_off
        self.noise_db = noise_db
        self.seconds = seconds
        self.peak_dbm = peak_dbm
        # With this, the carrier is on the air whether the radio keyed or not:
        # it is somebody else's transmitter.  The alignment case needs it,
        # because there is nothing to correlate while nothing is transmitting.
        self.always_on = always_on


class Sim(Backend):
    """
    A synthetic receiver: the same interface, no hardware.

    With no `truth` it invents a clean carrier at the requested frequency, so
    the flow runs; with one, the case that reads it back is testing the
    maths and the protocol together, which is what the host tests do.
    """

    name = "sim"
    kinds = (KIND_IQ, KIND_SPECTRUM, KIND_ENVELOPE)

    def __init__(self, truth=None, state=None):
        """
        `state` is a callable returning what the radio is doing right now:
        {"keyed": bool, "tone": Hz, "level": 0..100}.  With it the synthetic
        air follows the radio's own commands, which is the only way the numbers
        a case reads back can mean anything; without it the truth is fixed and
        the same signal is on the air whether the radio keyed or not.
        """
        self.truth = truth or SimTruth()
        self.state = state
        # What the air will do during the next capture, in seconds from its
        # start: (key_on, key_off).  The switching case needs the carrier to
        # come up in the middle of a window, which a state probe taken at the
        # start cannot express.
        self.schedule = None

    def _now(self):
        st = {}
        if self.state is not None:
            try:
                st = self.state() or {}
            except Exception:
                st = {}
        t = self.truth
        radio_keyed = bool(st.get("keyed", False))
        keyed = True if t.always_on else radio_keyed
        if not st:
            keyed = (t.t_on <= 0.0 and t.t_off > 1e9) or t.amplitude > 0
        tone = st.get("tone", None)
        if t.always_on and not radio_keyed:
            # Somebody else's transmitter: it carries its own modulation, not
            # the one the radio was told to generate.
            tone = t.tone_hz
        if tone is None:
            tone = t.tone_hz
        level = st.get("level", None)
        if level is None or (t.always_on and not radio_keyed):
            # The external source is not the radio: level 0 does not mean
            # "leave it as it is", it means the truth's own deviation.
            level = 100
        # The radio's level is a percentage of its own calibrated deviation,
        # which is what REG_40 does on the real thing (docs/BENCH.md).
        dev = t.deviation_hz * (max(0, min(100, level)) / 100.0) if level else 0.0
        return keyed, float(tone), float(dev)

    def capture(self, kind, center_hz, seconds, rate_hz=None, gain_db=None,
                **kw):
        t = self.truth
        secs = seconds or t.seconds
        keyed, tone, dev = self._now()
        if kind == KIND_IQ:
            fs = float(rate_hz or 240000.0)
            n = int(fs * secs)
            i = np.arange(n)
            rng = np.random.default_rng(1234)
            phase = np.zeros(n)
            base = 2.0 * math.pi * t.offset_hz * i / fs
            # The tone modulates the carrier: the phase is the integral of the
            # instantaneous frequency, which is what an FM modulator does.
            if dev and tone > 0:
                phase = (dev / tone) * np.sin(2.0 * math.pi * tone * i / fs)
            iq = t.amplitude * np.exp(1j * (base + phase))
            # Gated: a schedule says exactly when the carrier is up during the
            # window; failing that, the state probe says whether it is up at
            # all; failing that, t_on and t_off do.
            tt = i / fs
            if self.schedule is not None:
                on, off = self.schedule
                gate = (tt >= on) & (tt < off)
                iq = iq * gate
            elif self.state is not None:
                iq = iq * (1.0 if keyed else 0.0)
            else:
                gate = (tt >= t.t_on) & (tt < t.t_off)
                iq = iq * gate
            noise = (10 ** (t.noise_db / 20.0)) * (
                rng.normal(size=n) + 1j * rng.normal(size=n)) / math.sqrt(2)
            return Capture(KIND_IQ, (iq + noise).astype(np.complex64),
                           rate_hz=fs, center_hz=center_hz,
                           meta={"backend": "sim", "truth": vars(t)})

        if kind == KIND_SPECTRUM:
            span = kw.get("span_hz", 500000.0)
            npts = kw.get("points", 501)
            freqs = center_hz - span / 2 + np.arange(npts) * span / (npts - 1)
            dbm = np.full(npts, t.peak_dbm - 55.0)
            on = keyed if self.state is not None else \
                ((t.t_on < secs) and (t.t_off > 0))
            if on:
                df = freqs - (center_hz + t.offset_hz)
                shape = np.where(np.abs(df) <= 15000,
                                 t.peak_dbm - (np.abs(df) / 15000.0) ** 2 * 3.0,
                                 -200.0)
                dbm = np.maximum(dbm, shape)
            dbm = dbm + np.random.default_rng(7).normal(scale=0.2, size=npts)
            return Capture(KIND_SPECTRUM, (freqs, dbm), center_hz=center_hz,
                           meta={"backend": "sim", "truth": vars(t)})

        if kind == KIND_ENVELOPE:
            rate = 20000.0
            n = int(rate * secs)
            tvec = np.arange(n) / rate
            dbm = np.full(n, t.peak_dbm - 55.0)
            if self.state is not None:
                up = np.full(n, bool(keyed))
            else:
                up = (tvec >= t.t_on) & (tvec < t.t_off)
            dbm[up] = t.peak_dbm
            dbm = dbm + np.random.default_rng(9).normal(scale=0.3, size=n)
            return Capture(KIND_ENVELOPE, (tvec, dbm), rate_hz=rate,
                           center_hz=center_hz,
                           meta={"backend": "sim", "truth": vars(t)})

        raise BackendError("unknown capture kind %r" % kind)


# ---------------------------------------------------------------------------
# Factory
# ---------------------------------------------------------------------------

def open_backend(name, **kw):
    name = (name or "sim").lower()
    if name == "rtl":
        return RtlSdr(**kw)
    if name == "soapy":
        return Soapy(**kw)
    if name == "anritsu":
        return Anritsu(**kw)
    if name == "sim":
        return Sim(**kw)
    raise BackendError("unknown receiver %r (rtl, soapy, anritsu, sim)" % name)
