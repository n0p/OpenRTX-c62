#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
The bench: drive the radio, capture from the receiver, print the numbers.

    tools/bench/bench.py --device /dev/ttyUSB0 --sdr rtl run all
    tools/bench/bench.py --device /dev/ttyUSB0 --sdr anritsu run carrier mask
    tools/bench/bench.py --sim run all            # nothing plugged in

Each run writes a directory under --out (default runs/<timestamp>/...) with:

    report.txt      what you just read on the screen
    report.json     the same, for a script
    captures/*.npy  every capture, so a figure can be re-made without the radio
    radio_capture.npy, radio_capture.json   the radio's own window and its t0

and prints the report.  docs/BENCH.md has the protocol, the cases and the
precision each number can claim; this file is the CLI.
"""

import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.join(_HERE, "..", "cps"))

import numpy as np

import benchproto as B
import cases as CS
import correlate as C
import sdr as S


# ---------------------------------------------------------------------------

def _fmt_hz(hz):
    if hz is None:
        return "-"
    if abs(hz) >= 1e6:
        return "%.6f MHz" % (hz / 1e6)
    return "%.3f kHz" % (hz / 1e3)


def _fmt_row(label, value, unit=""):
    return "  %-22s %s%s" % (label, value, (" " + unit) if unit else "")


def open_radio(args):
    if args.sim or not args.device:
        import simradio
        return simradio.open_sim_radio(), "simulated radio (no cable)"
    link = B.open_link(args.device, baud=args.baud)
    info = B.describe(link)
    if not info["bench"]:
        raise SystemExit(
            "the radio on %s has no bench namespace: flash a build with "
            "-DC62_BENCH_PROTO=ON -DC62_DEBUG_PROTO=ON (docs/BENCH.md)"
            % args.device)
    return link, "radio %s, firmware %s" % (info["model"], info["firmware"])


def radio_state(radio):
    """
    What the radio is doing right now, for a simulated receiver to follow.

    Both a BenchLink and the simulated radio answer the same question through
    their last answers; the point is that the synthetic air is driven by the
    commands the cases actually sent, so "key with tone 88 at level 50" puts a
    carrier modulated at 88 Hz and half the calibrated deviation on the air.
    """
    st = {"keyed": False, "tone": 0, "level": 0}
    if hasattr(radio, "keyed"):          # the simulated radio
        st["keyed"] = bool(radio.keyed)
        st["tone"] = int(radio.tone)
        st["level"] = int(radio.level)
        return st
    last = getattr(radio, "last", {})
    ptt = last.get(B.MSG_BENCH_PTT)
    if ptt is not None:
        st["keyed"] = ptt.u8(B.T_TX_STATE) == 2
        st["tone"] = ptt.u16(B.T_TONE)
        st["level"] = ptt.u8(B.T_LEVEL)
    return st


def open_receiver(args, radio=None):
    if args.sdr == "sim":
        truth = S.SimTruth(freq_hz=args.freq,
                           offset_hz=args.sim_error_hz,
                           amplitude=10 ** (args.sim_level_db / 20.0),
                           tone_hz=args.sim_tone_hz,
                           deviation_hz=args.sim_deviation_hz,
                           t_on=args.sim_key_s, t_off=args.sim_key_s + 0.4,
                           noise_db=-80.0, peak_dbm=args.sim_peak_dbm)
        state = (lambda: radio_state(radio)) if radio is not None else None
        return S.Sim(truth, state=state), \
            "simulated receiver (offsets and deviations are the model's, not " \
            "measurements)"
    if args.sdr == "rtl":
        return S.RtlSdr(ppm=args.rtl_ppm, gain_db=args.rtl_gain), \
            "RTL-SDR (uncalibrated: relative levels only)"
    if args.sdr == "anritsu":
        return S.Anritsu(host=args.anritsu_host), \
            "Anritsu MS2090A at %s (no I/Q: spectrum and zero span only)" % \
            args.anritsu_host
    if args.sdr == "soapy":
        return S.Soapy(args.soapy_args), "SoapySDR device"
    raise SystemExit("unknown --sdr %r" % args.sdr)


# ---------------------------------------------------------------------------

def align_captures(ctx):
    """
    The correlation the whole tool is for: the same signal, in the radio's
    discriminator and in the receiver's demodulator, put on one time line.

    The lag between the two is measured by cross-correlation; what the protocol
    predicted is computed from the clock fit.  When both agree, the anchor is
    good and every other number in the report can be placed in time.
    """
    r = CS.Result("align", "Radio capture against the receiver's, on one clock")
    if not ctx.sdr.has(S.KIND_IQ):
        r.note("needs an I/Q receiver: the analyser cannot give the "
               "demodulated audio to correlate against")
        return r

    ctx.radio.arm()
    ctx.radio.tune(ctx.freq_hz, bw=2)
    r.step("tune %s, receive only" % _fmt_hz(ctx.freq_hz))
    # A simulated air is silent unless the radio keys, and the radio cannot
    # hear itself: put somebody else's carrier on the frequency, or this case
    # would correlate two silences.
    if isinstance(ctx.sdr, S.Sim):
        ctx.sdr.truth.always_on = True
        # And it is modulated with the tone the simulated radio's own
        # discriminator carries, or there is nothing to correlate.
        ctx.sdr.truth.tone_hz = ctx.sdr.truth.tone_hz or 1000.0
        ctx.sdr.truth.deviation_hz = ctx.sdr.truth.deviation_hz or 2100.0

    # The radio's window first, then the receiver's, so both are valid at the
    # same time; the radio's t0 and the receiver's wall stamp are what line
    # them up.
    samples, meta = ctx.radio.capture(300)
    cap = ctx.sdr.capture(S.KIND_IQ, ctx.freq_hz, 0.3)
    r.artifacts.append(ctx.save("align_radio.npy", np.array(samples)))
    r.artifacts.append(ctx.save("align_iq.npy", cap.data))

    radio_audio = np.array(samples, dtype=np.float64)
    sdr_audio = C.demod_fm(cap.data, cap.rate_hz)
    # Bring the SDR's audio to the radio's rate by plain decimation: the
    # signals of interest are audio-band, well under both Nyquist limits.
    step = max(1, int(round(cap.rate_hz / (meta.get("rate") or 48000))))
    sdr_audio = sdr_audio[::step]
    fs = (meta.get("rate") or 48000)

    # Correlate the steady part only.  A capture that starts or ends with the
    # carrier coming up or going down has a phase step there, and that step is
    # an impulse of several kHz that would dominate the energy and hide the
    # signal itself.  So: keep the middle of the stretch where the carrier is
    # present in both.
    env_t, env_db = C.envelope_db(cap.data, cap.rate_hz, block_ms=1.0)
    steady = None
    if len(env_db):
        thr = float(np.percentile(env_db, 10)) + 10.0
        above = np.where(env_db > thr)[0]
        if len(above) > 40:
            # 10 ms of guard at each end, in blocks of 1 ms.
            steady = (int(above[0]) + 10, int(above[-1]) - 10)
    if steady is not None:
        a, b = steady
        sdr_audio = C.demod_fm(
            cap.data[a * int(cap.rate_hz / 1000):b * int(cap.rate_hz / 1000)],
            cap.rate_hz)[::step]
        n_radio = min(len(radio_audio), len(sdr_audio))
        radio_audio = radio_audio[:n_radio]
        r.step("correlating %d ms of carrier out of the captures"
               % int(1000 * len(sdr_audio) / fs))

    xc = C.xcorr_delay_s(radio_audio, sdr_audio, fs)
    if xc:
        r.numbers["lag_s"] = round(xc["lag_s"], 4)
        r.numbers["lag_samples"] = xc["lag_samples"]
        r.numbers["match"] = round(xc["peak"], 3)
    tone_radio = C.tone_in_audio(radio_audio, fs)
    r.numbers["radio_tone_hz"] = round(tone_radio, 1) if tone_radio else None
    tone_sdr = C.tone_frequency_hz(sdr_audio, fs)
    r.numbers["sdr_tone_hz"] = round(tone_sdr, 1) if tone_sdr else None
    r.numbers["radio_t0_ms"] = meta.get("t0")
    r.numbers["radio_rate"] = fs
    r.numbers["radio_blocks"] = meta.get("blocks")
    r.numbers["radio_block_len"] = meta.get("block")

    if xc and xc["peak"] < 0.2:
        r.note("the two captures do not look like the same signal (normalised "
               "correlation %.2f): either the radio was not receiving the "
               "receiver's signal, or the demodulator settings differ -- in a "
               "simulated run they are two independent models, so a low match "
               "there means nothing" % xc["peak"])
    r.note("a lag on the order of the capture period is expected: the window "
           "starts when the radio armed it and the receiver started later")
    return r


def clock_report(radio, receiver):
    """Turn the collected (tick, host time) pairs into a statement."""
    pairs = getattr(radio, "clock_pairs", [])
    fit = C.fit_clock(pairs)
    out = {"pairs": len(pairs)}
    if fit is None:
        out["note"] = ("fewer than two timed answers: no clock fit, and "
                       "without it the alignment is the tick granularity "
                       "(10 ms) and nothing better")
        return out, None
    # The raw offset is host monotonic nanoseconds at radio tick 0, which is a
    # number only a machine likes; what a person wants to know is how long ago
    # the radio booted, and by how much its clock disagrees with this one.
    booted = (time.monotonic_ns() - fit["offset_ns"]) / 1e9
    out.update({"radio_booted_s_ago": round(booted, 1),
                "ppm": round(fit["ppm"], 2),
                "scatter_ms": round(fit["rms_ms"], 2),
                "alignment_ms": C.uncertainty_ms(fit)})
    return out, fit


# ---------------------------------------------------------------------------

def render(results, header, clock, args):
    w = 78
    lines = []
    lines.append("=" * w)
    lines.append("BENCH REPORT".center(w))
    lines.append("=" * w)
    for k, v in header.items():
        lines.append("  %-14s %s" % (k, v))
    lines.append("")
    for r in results:
        lines.append("-" * w)
        lines.append("%s  %s" % (r.name.upper(), r.title))
        lines.append("-" * w)
        if r.steps:
            lines.append(" procedure")
            for s in r.steps:
                lines.append("   . " + s)
        if r.numbers:
            lines.append(" numbers")
            for k, v in r.numbers.items():
                if isinstance(v, (int, float)):
                    lines.append(_fmt_row(k, v))
                elif isinstance(v, str):
                    lines.append(_fmt_row(k, v))
                elif isinstance(v, list) and v and isinstance(v[0], dict):
                    for entry in v:
                        lines.append("   " + "  ".join(
                            "%s=%s" % (kk, vv) for kk, vv in entry.items()))
                elif v is not None:
                    lines.append(_fmt_row(k, json.dumps(v)))
        if r.notes:
            lines.append(" reading")
            for n in r.notes:
                for i, chunk in enumerate(_wrap(n, w - 6)):
                    lines.append("   " + ("* " if i == 0 else "  ") + chunk)
        if r.artifacts:
            lines.append(" artifacts")
            for a in r.artifacts:
                lines.append("   " + os.path.basename(a))
        lines.append("")
    lines.append("-" * w)
    lines.append("TIME BASE")
    lines.append("-" * w)
    for k, v in clock.items():
        lines.append(_fmt_row(k, v))
    lines.append(_fmt_row("tick source", "k_uptime_get(), ms since boot"))
    lines.append(_fmt_row("resolution", "10 ms (protocol tick) / 20.8 us "
                                       "(48 kHz sample)"))
    lines.append("")
    lines.append("=" * w)
    lines.append("Simulated data is a model, not a measurement.  Read "
                 "docs/BENCH.md".center(w))
    lines.append("=" * w)
    return "\n".join(lines)


def _wrap(text, width):
    words = text.split()
    out, cur = [], ""
    for wd in words:
        if len(cur) + len(wd) + 1 > width:
            out.append(cur)
            cur = wd
        else:
            cur = (cur + " " + wd).strip()
    if cur:
        out.append(cur)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Measure a C62 against an SDR (docs/BENCH.md)")
    ap.add_argument("--device", help="programming cable, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--sdr", default="sim",
                    choices=["sim", "rtl", "soapy", "anritsu"])
    ap.add_argument("--freq", default="145.500M",
                    help="frequency to measure, default 145.500M")
    ap.add_argument("--tone", type=int, default=88,
                    help="sub-audible tone in Hz, default 88")
    ap.add_argument("--level", type=int, default=0,
                    help="level setting for the deviation case, 0 = the "
                         "standard steps")
    ap.add_argument("--power", type=int, default=500,
                    help="transmit power every TUNE asks the radio for, mW "
                         "(default 500).  A run keys the PA, and outside a "
                         "memory channel there is no channel power to inherit: "
                         "0 refuses the keying instead of transmitting into "
                         "nothing")
    ap.add_argument("--out", default=None)
    ap.add_argument("--sim", action="store_true",
                    help="simulated radio: nothing plugged in")
    ap.add_argument("--rtl-ppm", type=int, default=0)
    ap.add_argument("--rtl-gain", default=None)
    ap.add_argument("--soapy-args", default="")
    ap.add_argument("--anritsu-host", default="10.205.52.71")
    ap.add_argument("--sim-error-hz", type=float, default=0.0,
                    help="simulated carrier error")
    ap.add_argument("--sim-deviation-hz", type=float, default=2100.0)
    ap.add_argument("--sim-tone-hz", type=float, default=1000.0)
    ap.add_argument("--sim-level-db", type=float, default=-10.0)
    ap.add_argument("--sim-key-s", type=float, default=0.10)
    ap.add_argument("--sim-peak-dbm", type=float, default=-40.0)
    ap.add_argument("cmd", nargs="?", default="run",
                    choices=["run", "info", "cases"])
    ap.add_argument("names", nargs="*")
    args = ap.parse_args(argv)

    if args.cmd == "cases":
        for name, (fn, needs) in CS.CASES.items():
            print("%-12s %s" % (name, (fn.__doc__ or "").strip().split("\n")[0]))
        print("%-12s %s" % ("align", "radio capture vs receiver capture"))
        return 0

    freq = _parse_freq(args.freq)
    radio, radio_desc = open_radio(args)
    if hasattr(radio, "default_power"):
        # Every case's TUNE carries it unless the case asks for another one.
        radio.default_power = int(args.power)

    if args.cmd == "info":
        print("radio:      %s" % radio_desc)
        if hasattr(radio, "hello"):
            for k, v in B.describe(radio).items():
                print("  %-10s %s" % (k, v))
        print("receiver:   %s" % args.sdr)
        return 0

    recv, recv_desc = open_receiver(args, radio)
    outdir = args.out or os.path.join(
        _HERE, "runs", time.strftime("%Y%m%d-%H%M%S"))

    header = {
        "radio": radio_desc,
        "receiver": recv_desc,
        "frequency": _fmt_hz(freq),
        "power": "%d mW" % args.power,
        "tone": "%d Hz" % args.tone,
        "started": time.strftime("%Y-%m-%d %H:%M:%S"),
        "simulated": "yes" if (args.sim or args.sdr == "sim") else "no",
    }
    ctx = CS.Context(radio, recv, os.path.join(outdir, "captures"), freq,
                     args.tone, args.level)

    names = args.names or ["all"]
    if "all" in names:
        names = list(CS.CASES.keys()) + ["align"]
    print("bench: %s | %s | %s | %d mW"
          % (radio_desc, recv_desc, _fmt_hz(freq), args.power))

    want_align = "align" in names
    names = [n for n in names if n != "align"]

    results = []
    for name in names:
        if name not in CS.CASES:
            raise SystemExit("unknown case %r (see `bench.py cases`)" % name)
        try:
            res = CS.run_case(name, ctx)
        except Exception as exc:  # a broken case must not lose the others
            res = CS.Result(name, "failed")
            res.note("this case did not run: %s: %s" % (
                type(exc).__name__, exc))
        results.append(res)
        print("  %-12s %s" % (name, "ok" if res.ok() else "no numbers"))

    if want_align or "all" in (args.names or ["all"]):
        try:
            res = align_captures(ctx)
        except Exception as exc:
            res = CS.Result("align", "failed")
            res.note("alignment did not run: %s: %s" % (
                type(exc).__name__, exc))
        results.append(res)
        print("  %-12s %s" % ("align", "ok" if res.ok() else "no numbers"))

    clock, _fit = clock_report(radio, recv)
    text = render(results, header, clock, args)

    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "report.txt"), "w") as fh:
        fh.write(text + "\n")
    with open(os.path.join(outdir, "report.json"), "w") as fh:
        json.dump({
            "header": header,
            "clock": clock,
            "results": [
                {"name": r.name, "title": r.title, "steps": r.steps,
                 "numbers": r.numbers, "notes": r.notes,
                 "artifacts": [os.path.basename(a) for a in r.artifacts]}
                for r in results],
        }, fh, indent=2)

    print()
    print(text)
    print()
    print("artifacts in %s" % outdir)

    if hasattr(radio, "close"):
        try:
            radio.close()
        except Exception:
            pass
    recv.close()
    return 0 if any(r.ok() for r in results) else 1


def _parse_freq(text):
    t = str(text).strip().lower()
    mult = 1
    if t.endswith("mhz") or t.endswith("m"):
        mult = int(1e6)
        t = t.rstrip("hz").rstrip("m")
    elif t.endswith("khz") or t.endswith("k"):
        mult = int(1e3)
        t = t.rstrip("hz").rstrip("k")
    elif t.endswith("hz"):
        t = t[:-2]
    return int(round(float(t) * mult))


if __name__ == "__main__":
    sys.exit(main())
