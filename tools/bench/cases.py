#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The measurement cases: one procedure per thing the SDR can check.

Each case is a function of a Context (radio, receiver, output directory) that
returns a Result: the numbers, the steps it took, and what those numbers mean.
The point of the file is that the *procedure* is written down once, in code
and in docs/BENCH.md, so two people running the same bench get comparable
figures instead of two different experiments.

What each case needs from the instrument is declared in `needs`: the runner
refuses a case whose instrument cannot do it, instead of quietly producing a
report full of holes.
"""

import os
import time

import numpy as np

import correlate as C
from sdr import KIND_IQ, KIND_SPECTRUM, KIND_ENVELOPE


class Result:
    def __init__(self, name, title):
        self.name = name
        self.title = title
        self.steps = []        # what was done, in order
        self.numbers = {}      # what came out
        self.notes = []        # what it means, and what it does not
        self.artifacts = []    # files written

    def step(self, text):
        self.steps.append(text)

    def note(self, text):
        self.notes.append(text)

    def ok(self):
        return bool(self.numbers)


class Context:
    def __init__(self, radio, sdr, outdir, freq_hz, tone_hz, level, quiet=False,
                 log=print):
        self.radio = radio
        self.sdr = sdr
        self.outdir = outdir
        self.freq_hz = int(freq_hz)
        self.tone_hz = tone_hz
        self.level = level
        self.quiet = quiet
        self.log = log
        os.makedirs(outdir, exist_ok=True)

    def save(self, name, data):
        path = os.path.join(self.outdir, name)
        np.save(path, data)
        return path


def _fmt_hz(hz):
    if hz is None:
        return "-"
    if abs(hz) >= 1e6:
        return "%.6f MHz" % (hz / 1e6)
    if abs(hz) >= 1e3:
        return "%.3f kHz" % (hz / 1e3)
    return "%.1f Hz" % hz


def _capture(ctx, kind, seconds, rate_hz=None, **kw):
    return ctx.sdr.capture(kind, ctx.freq_hz, seconds, rate_hz=rate_hz, **kw)


# ---------------------------------------------------------------------------
# 1. Carrier frequency: what the radio was told against what is on the air
# ---------------------------------------------------------------------------

needs_carrier = (KIND_IQ, KIND_SPECTRUM)


def case_carrier(ctx):
    """
    Procedure: arm, tune to f, key with no modulation (tone 0), capture, read
    the carrier frequency, compare with the programmed one.

    Expected at the SDR: a single narrow line.  The difference is the radio's
    error: the synthesizer's reference, its step and its calibration, all in
    one number.  A radio that cannot be measured here is a radio whose
    frequency is only "what the chip was told".
    """
    r = Result("carrier", "Carrier frequency against the programmed one")
    ctx.radio.arm()
    t = ctx.radio.tune(ctx.freq_hz, bw=2)
    r.step("tune %s -> radio says %s" % (_fmt_hz(ctx.freq_hz),
                                         _fmt_hz(t["rx"])))
    p = ctx.radio.ptt(True, tone=0, level=ctx.level)
    r.step("key with no modulation, radio tick %d" % p["tick"])
    try:
        time.sleep(0.15)  # let the PA and the chip settle
        if ctx.sdr.has(KIND_IQ):
            cap = _capture(ctx, KIND_IQ, 0.5)
            r.artifacts.append(ctx.save("carrier_iq.npy", cap.data))
            offset = C.carrier_offset_hz(cap.data, cap.rate_hz)
            power = C.power_dbfs(cap.data)
            bw = C.occupied_bandwidth_hz(cap.data, cap.rate_hz)
            err = offset  # the SDR's centre is the programmed frequency
            r.numbers["error_hz"] = round(err, 1) if err is not None else None
            r.numbers["power_dbfs"] = round(power, 1) if power else None
            r.numbers["occupied_bw_hz"] = round(bw, 0) if bw else None
            r.numbers["source"] = "iq"
            r.note("the I/Q centre is the frequency the radio was told: the "
                   "offset is the radio's error, positive means high")
        else:
            cap = _capture(ctx, KIND_SPECTRUM, 0.5, span_hz=100000.0)
            r.artifacts.append(ctx.save("carrier_trace.npy",
                                        np.vstack(cap.data)))
            peak = C.trace_peak(*cap.data)
            if peak:
                r.numbers["error_hz"] = round(peak["freq_hz"] - ctx.freq_hz,
                                              1)
                r.numbers["level_dbm"] = round(peak["level_dbm"], 1)
                r.numbers["source"] = "spectrum"
                r.note("the analyser is calibrated, so the level is absolute;"
                       " its own frequency reference sets the floor of the "
                       "error figure (typ. 0.1 ppm = 15 Hz at 145 MHz)")
    finally:
        ctx.radio.ptt(False)
        r.step("unkey")

    if r.numbers.get("error_hz") is not None:
        r.note("compare against the channel raster: an error under half a "
               "step is indistinguishable from the tuning step itself")
    return r


# ---------------------------------------------------------------------------
# 2. Deviation (FM)
# ---------------------------------------------------------------------------

needs_deviation = (KIND_IQ,)


def case_deviation(ctx):
    """
    Procedure: key with a sub-audible tone at a known level, capture the I/Q,
    demodulate and measure the instantaneous frequency.

    Expected at the SDR: a carrier that swings symmetrically by +-dev.  The
    radio's CTCSS generator is the known modulator; what is unknown -- and
    measured -- is how many hertz a level step produces, and whether the
    deviation is symmetric and independent of the level setting.
    """
    r = Result("deviation", "FM deviation against the level setting")
    ctx.radio.arm()
    ctx.radio.tune(ctx.freq_hz, bw=2)
    steps = [ctx.level] if ctx.level else [30, 65, 100]
    out = []
    for level in steps:
        p = ctx.radio.ptt(True, tone=ctx.tone_hz, level=level)
        try:
            time.sleep(0.15)
            cap = _capture(ctx, KIND_IQ, 0.5)
            if level == steps[0]:
                r.artifacts.append(ctx.save("deviation_iq.npy", cap.data))
            d = C.deviation_hz(cap.data, cap.rate_hz)
            tone = C.tone_frequency_hz(C.demod_fm(cap.data, cap.rate_hz),
                                       cap.rate_hz)
            out.append({"level": level, "tone_hz_applied": p["tone"],
                        "p99_hz": round(d["p99_hz"], 1) if d else None,
                        "peak_hz": round(d["peak_hz"], 1) if d else None,
                        "dc_hz": round(d["dc_hz"], 1) if d else None,
                        "tone_measured_hz": round(tone, 1) if tone else None})
        finally:
            ctx.radio.ptt(False)
        r.step("level %d: tone %d Hz, deviation %s" % (
            level, p["tone"], out[-1]["p99_hz"]))
    r.numbers["per_level"] = out
    if out and all(o["p99_hz"] for o in out):
        r.numbers["hz_per_level_step"] = round(
            (out[-1]["p99_hz"] - out[0]["p99_hz"]) / max(steps[-1] - steps[0],
                                                         1), 2)
    r.note("the measured tone frequency is the cross-check that the "
           "modulation is the one that was asked for; a tone at 0 Hz means "
           "the radio refused its generator (docs/BENCH.md, tone range)")
    r.note("dc_hz is the carrier offset as the discriminator sees it: it "
           "should match the carrier case, and a difference between the two "
           "means the transmitter moves when it is modulated")
    r.note("12.5 kHz channels want 2.5 kHz peak deviation, 25 kHz want 5 kHz: "
           "that is the figure to compare this against")
    return r


# ---------------------------------------------------------------------------
# 3. Relative power
# ---------------------------------------------------------------------------

needs_power = (KIND_IQ, KIND_SPECTRUM)


def case_power(ctx):
    """
    Procedure: key at each power setting the radio offers, measure the power in
    the channel bandwidth.

    Expected at the SDR: steps proportional to the settings.  Relative, not
    absolute, with an RTL-SDR (no calibration); with the analyser the level is
    dBm at its own port, which still is not the antenna unless the cable loss
    is known.
    """
    r = Result("power", "Transmitted power against the radio's settings")
    ctx.radio.arm()
    ctx.radio.tune(ctx.freq_hz, bw=2)
    readings = []
    # The radio's power is a channel property; the bench does not expose it, so
    # what can be measured here is the level stability at one setting and the
    # difference between keyed and unkeyed.
    p = ctx.radio.ptt(True, tone=0, level=0)
    try:
        time.sleep(0.2)
        if ctx.sdr.has(KIND_IQ):
            cap = _capture(ctx, KIND_IQ, 0.3)
            readings.append({"setting": "keyed",
                             "db": round(C.channel_power_db(
                                 cap.data, cap.rate_hz, 12500.0), 1)})
            r.artifacts.append(ctx.save("power_iq.npy", cap.data))
        else:
            cap = _capture(ctx, KIND_SPECTRUM, 0.3, span_hz=100000.0)
            readings.append({"setting": "keyed",
                             "db": round(C.trace_channel_power(
                                 *cap.data, ctx.freq_hz, 12500.0), 1)})
    finally:
        ctx.radio.ptt(False)
    time.sleep(0.1)
    if ctx.sdr.has(KIND_IQ):
        cap = _capture(ctx, KIND_IQ, 0.3)
        readings.append({"setting": "unkeyed",
                         "db": round(C.channel_power_db(cap.data, cap.rate_hz,
                                                        12500.0), 1)})
    r.numbers["readings"] = readings
    if len(readings) == 2:
        r.numbers["keyed_over_unkeyed_db"] = round(
            readings[0]["db"] - readings[1]["db"], 1)
        r.note("the keyed/unkeyed difference is the only power figure this "
               "setup can give without a calibrated tap: it is the on/off "
               "ratio, not the watts")
    r.note("absolute power needs the analyser at the antenna port and the "
           "cable loss written down next to it")
    return r


# ---------------------------------------------------------------------------
# 4. Spectral mask
# ---------------------------------------------------------------------------

needs_mask = (KIND_IQ, KIND_SPECTRUM)

# Relative mask, in dB below the carrier peak, against offset in kHz.  It is
# the shape every 12.5 kHz service expects: -60 dB at one channel away is the
# usual ETSI figure for a mobile transmitter, and this is a bench, not a
# certification lab.
MASK_12K5 = [(0.0, 0.0), (6.25, -3.0), (12.5, -60.0), (25.0, -70.0),
             (50.0, -80.0)]


def case_mask(ctx):
    """
    Procedure: key with the modulator the mask is for (the mode's own signal;
    here an unmodulated carrier plus the tone), capture a wide spectrum and
    measure it against a relative mask.

    Expected at the SDR: the energy inside the channel and the skirt falling
    away by the offset.  What the radio cannot tell you is whether it passes:
    the absolute limit belongs to the standard and the licence.
    """
    r = Result("mask", "Spectral mask of the transmitted signal")
    ctx.radio.arm()
    ctx.radio.tune(ctx.freq_hz, bw=2)
    p = ctx.radio.ptt(True, tone=ctx.tone_hz, level=ctx.level)
    try:
        time.sleep(0.2)
        if ctx.sdr.has(KIND_IQ):
            cap = _capture(ctx, KIND_IQ, 0.5, rate_hz=480000.0)
            r.artifacts.append(ctx.save("mask_iq.npy", cap.data))
            f, db = C.spectrum(cap.data, cap.rate_hz)
            peak = C.carrier_offset_hz(cap.data, cap.rate_hz) or 0.0
            rel = db - db.max()
            points = [(off * 1e3, rel[int(np.argmin(
                np.abs(f - (peak + off * 1e3))))]) for off, _ in MASK_12K5]
            r.numbers["measured_db_at_offset"] = [
                {"offset_khz": off, "db_below_peak": round(lvl, 1)}
                for off, lvl in points]
            margin = C.trace_mask_margin(
                f - peak, db - db.max(), [(o * 1e3, lim) for o, lim in
                                          MASK_12K5])
            r.numbers["worst_margin_db"] = (round(margin["margin_db"], 1)
                                            if margin else None)
            r.numbers["worst_offset_khz"] = (round(margin["offset_hz"] / 1e3, 1)
                                             if margin else None)
        else:
            cap = _capture(ctx, KIND_SPECTRUM, 0.5, span_hz=200000.0,
                           rbw_hz=1000.0)
            r.artifacts.append(ctx.save("mask_trace.npy", np.vstack(cap.data)))
            freqs, dbm = cap.data
            peak = C.trace_peak(freqs, dbm)
            rel = dbm - peak["level_dbm"]
            r.numbers["measured_db_at_offset"] = [
                {"offset_khz": off,
                 "db_below_peak": round(float(rel[int(np.argmin(
                     np.abs(freqs - (peak["freq_hz"] + off * 1e3))))]), 1)}
                for off, _ in MASK_12K5]
            margin = C.trace_mask_margin(
                freqs - peak["freq_hz"], rel,
                [(o * 1e3, lim) for o, lim in MASK_12K5])
            r.numbers["worst_margin_db"] = (round(margin["margin_db"], 1)
                                            if margin else None)
        r.step("mask measured relative to the carrier, %s" % (
            "I/Q" if ctx.sdr.has(KIND_IQ) else "analyser trace"))
    finally:
        ctx.radio.ptt(False)
    r.note("the mask here is a bench reference (-60 dB at +-12.5 kHz), not a "
           "regulatory limit: passing it says the transmitter is sane, not "
           "that it is legal")
    return r


# ---------------------------------------------------------------------------
# 5. Sensitivity: CRC errors against the level at the antenna
# ---------------------------------------------------------------------------

needs_sensitivity = (KIND_IQ, KIND_SPECTRUM, KIND_ENVELOPE)


def case_sensitivity(ctx):
    """
    Procedure: put a modulated carrier on the tuned frequency (a second
    transmitter, or the same radio through a dummy load and an attenuator),
    step the level down, and read the radio's own receive counters at each
    step.  The instrument measures the *level*; the radio measures its *own*
    decode.

    Expected at the SDR: nothing but a level.  The bit error rate is the
    radio's, and it is the only honest source for it: this bench cannot
    demodulate the mode's PHY on the PC side, and pretending to would just
    add a second decoder.
    """
    r = Result("sensitivity", "Decode errors against the received level")
    ctx.radio.arm()
    ctx.radio.tune(ctx.freq_hz)
    r.step("tune to %s" % _fmt_hz(ctx.freq_hz))
    rows = []
    for step_db in (0, -6, -12, -18, -24, -30):
        if ctx.sdr.has(KIND_IQ):
            cap = _capture(ctx, KIND_IQ, 0.3)
            level = C.channel_power_db(cap.data, cap.rate_hz, 12500.0)
            units = "dBFS"
        else:
            cap = _capture(ctx, KIND_SPECTRUM, 0.3, span_hz=100000.0)
            level = C.trace_channel_power(*cap.data, ctx.freq_hz, 12500.0)
            units = "dBm"
        # The radio's own counters: the same source the `rx` remote command
        # prints, over the debug namespace.
        counters = None
        try:
            counters = ctx.radio.rx_counters()
        except Exception:
            counters = None
        rows.append({"level": round(level, 1) if level is not None else None,
                     "units": units, "counters": counters})
        r.step("level %s %s: counters %s" % (
            rows[-1]["level"], units,
            "read" if counters else "unavailable (no CONFIG_C62_MODE_STATS)"))
        time.sleep(0.2)
    r.numbers["rows"] = rows
    levels = [row["level"] for row in rows if row["level"] is not None]
    if len(levels) > 1 and max(levels) - min(levels) < 0.5:
        r.note("the level did not change between the steps: this run has "
               "nothing that can step it (an attenuator or a signal "
               "generator does, and the simulation cannot), so what is left "
               "is one level reading and no curve")
    r.note("the level column is what the instrument sees; the counters are "
           "what the radio decoded.  Without a way to step the level "
           "electrically the operator does it by hand and the tool only "
           "records the pairs")
    if not any(row["counters"] for row in rows):
        r.note("no receive counters in this build: the sensitivity curve "
               "cannot be produced, only the level can be measured")
    return r


# ---------------------------------------------------------------------------
# 6. TX/RX switching time
# ---------------------------------------------------------------------------

needs_switching = (KIND_IQ, KIND_ENVELOPE)


def case_switching(ctx):
    """
    Procedure: start a capture, key in the middle of it, unkey before it ends,
    and measure where the carrier rises and falls.

    Expected at the SDR: a rise and a fall a few milliseconds wide, inside a
    window that also holds the silence before and after, which is what makes
    the two edges measurable in one take.  The difference between the moment
    the key command went out and the moment the carrier appeared is the key-up
    latency of the chain: the rtx thread's period, the audio route and the PA.

    With a zero-span analyser there is no window to key inside of: it sweeps
    time itself and the tool cannot trigger it, so what it gives is the level
    with the carrier up and down, and the instrument's own time resolution is
    the limit.  The case says so rather than quoting a latency it did not
    measure.
    """
    r = Result("switching", "Transmit and receive switching times")
    ctx.radio.arm()
    ctx.radio.tune(ctx.freq_hz, bw=2)

    window_s = 1.2
    key_at, unkey_at = 0.30, 0.75

    if ctx.sdr.has(KIND_IQ):
        # The receiver is told what the air will do (the simulator needs it;
        # a real receiver does not care), then the capture runs while the
        # radio is keyed from this thread.
        if hasattr(ctx.sdr, "schedule"):
            ctx.sdr.schedule = (key_at, unkey_at)
        t_key = None
        t_unkey = None
        try:
            import threading
            holder = {}

            def capture():
                holder["cap"] = _capture(ctx, KIND_IQ, window_s)

            th = threading.Thread(target=capture)
            t_start = time.monotonic()
            th.start()
            time.sleep(key_at)
            t_key = time.monotonic() - t_start
            p = ctx.radio.ptt(True, tone=0, level=0)
            time.sleep(max(0.0, unkey_at - key_at))
            t_unkey = time.monotonic() - t_start
            ctx.radio.ptt(False)
            th.join(timeout=window_s + 10)
            cap = holder.get("cap")
        finally:
            if hasattr(ctx.sdr, "schedule"):
                ctx.sdr.schedule = None
        if cap is None:
            r.note("the receiver did not return a capture: nothing to measure")
            return r
        fs = cap.rate_hz
        iq = cap.data
        r.artifacts.append(ctx.save("switching_iq.npy", iq))
        e = C.edge_times_s(iq, fs, block_ms=0.5)
        r.numbers["key_command_s"] = round(t_key, 3)
        r.numbers["unkey_command_s"] = round(t_unkey, 3)
        r.numbers["carrier_on_s"] = (round(e["on"], 3)
                                     if e["on"] is not None else None)
        r.numbers["carrier_off_s"] = (round(e["off"], 3)
                                      if e["off"] is not None else None)
        if e["on"] is not None:
            r.numbers["key_up_latency_ms"] = round((e["on"] - t_key) * 1000, 1)
        if e["off"] is not None:
            r.numbers["key_down_latency_ms"] = round(
                (e["off"] - t_unkey) * 1000, 1)
        r.numbers["floor_db"] = (round(e["floor_db"], 1)
                                 if e["floor_db"] is not None else None)
        if e["on"] is not None and e["off"] is not None:
            r.numbers["burst_s"] = round(e["off"] - e["on"], 3)
        r.step("keyed at %.3f s and unkeyed at %.3f s of a %.1f s capture"
               % (t_key, t_unkey, window_s))
    else:
        up = None
        down = None
        p = ctx.radio.ptt(True, tone=0, level=0)
        cap_up = _capture(ctx, KIND_ENVELOPE, 0.5)
        ctx.radio.ptt(False)
        time.sleep(0.2)
        cap_dn = _capture(ctx, KIND_ENVELOPE, 0.5)
        r.artifacts.append(ctx.save("switching_env_keyed.npy",
                                    np.vstack(cap_up.data)))
        r.artifacts.append(ctx.save("switching_env_idle.npy",
                                    np.vstack(cap_dn.data)))
        up = float(np.percentile(cap_up.data[1], 75))
        down = float(np.percentile(cap_dn.data[1], 75))
        r.numbers["keyed_dbm"] = round(up, 1)
        r.numbers["idle_dbm"] = round(down, 1)
        r.numbers["on_off_db"] = round(up - down, 1)
        r.numbers["sweep_s"] = 0.5
        r.step("two zero-span sweeps, keyed and idle")
        r.note("a zero-span analyser cannot be keyed inside a window from "
               "here (its sweep needs a trigger this tool does not set), so "
               "the on/off level is measured and the latency is not")
        p = {"tick": None}
    r.numbers["radio_key_tick_ms"] = p.get("tick")
    r.note("the latency is dominated by the rtx thread's 40 Hz period: expect "
           "0-30 ms, and a figure below the period is not achievable")
    r.note("the two latencies are not symmetric: the PA comes up through the "
           "audio route and goes down through the guard's exit path")
    return r


# ---------------------------------------------------------------------------
# 7. Carrier stability
# ---------------------------------------------------------------------------

needs_stability = (KIND_IQ, KIND_SPECTRUM)


def case_stability(ctx):
    """
    Procedure: key for a few seconds and measure the carrier frequency in
    blocks.  The spread is the transmitter's short-term stability, the slope is
    its warm-up drift.

    Expected at the SDR: a line that wanders by a few tens of hertz in the
    first second, then settles.  In ppm: a crystal that is inside 2 ppm is a
    normal one; 20 ppm is a cheap one; a drifting line means the PA is heating
    the reference.
    """
    r = Result("stability", "Carrier stability over a keyed period")
    ctx.radio.arm()
    ctx.radio.tune(ctx.freq_hz, bw=2)
    ctx.radio.ptt(True, tone=0, level=0)
    try:
        time.sleep(0.2)
        if ctx.sdr.has(KIND_IQ):
            cap = _capture(ctx, KIND_IQ, 3.0, rate_hz=240000.0)
            r.artifacts.append(ctx.save("stability_iq.npy", cap.data))
            ts, offs = C.frequency_series(cap.data, cap.rate_hz, block_ms=100.0)
            st = C.stability(ts, offs, nominal_hz=ctx.freq_hz)
            if st:
                r.numbers.update({k: (round(v, 3) if isinstance(v, float)
                                      else v) for k, v in st.items()})
                r.numbers["blocks"] = int(len(offs))
        else:
            peaks = []
            for _ in range(8):
                cap = _capture(ctx, KIND_SPECTRUM, 0.2, span_hz=20000.0,
                               rbw_hz=100.0)
                p = C.trace_peak(*cap.data)
                if p:
                    peaks.append((p["freq_hz"], p["level_dbm"]))
                time.sleep(0.2)
            d = C.trace_peak_drift(peaks)
            if d:
                r.numbers["mean_hz"] = round(d["mean_hz"], 1)
                r.numbers["spread_hz"] = round(d["spread_hz"], 1)
                r.numbers["sweeps"] = d["n"]
                err = d["mean_hz"] - ctx.freq_hz
                r.numbers["error_hz"] = round(err, 1)
                r.numbers["ppm"] = round(err / ctx.freq_hz * 1e6, 2)
                r.note("with the analyser the spread includes its own sweep "
                       "resolution: at 100 Hz RBW and 20 kHz span a few bins "
                       "of movement is the instrument, not the radio")
    finally:
        ctx.radio.ptt(False)
    r.note("a keyed PA heats the reference: the first second is warm-up and "
           "should be reported separately from the settled figure")
    return r


CASES = {
    "carrier": (case_carrier, needs_carrier),
    "deviation": (case_deviation, needs_deviation),
    "power": (case_power, needs_power),
    "mask": (case_mask, needs_mask),
    "sensitivity": (case_sensitivity, needs_sensitivity),
    "switching": (case_switching, needs_switching),
    "stability": (case_stability, needs_stability),
}


def run_case(name, ctx):
    fn, needs = CASES[name]
    for kind in needs:
        if ctx.sdr.has(kind):
            break
    else:
        raise RuntimeError(
            "%s needs one of %s and %s offers none of them" % (
                name, ", ".join(needs), ctx.sdr.name))
    return fn(ctx)
