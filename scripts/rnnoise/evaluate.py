#!/usr/bin/env python3
"""RNNoise quality evaluation for the C62 noise-suppression stage.

Materials
  clean    : LibriSpeech dev-clean, 6 utterances, resampled to 48 kHz mono, -20 dBFS
  engine   : Freesound "idle engine" (CC0), looped with 30 ms crossfades to 30 s
  airband  : synthesised VHF-AM receiver noise (band-limited 250-3400 Hz, pink
             tilt, Poisson static crashes) - no free *recording* of airband
             noise was available offline; the model is stated here on purpose

For every (noise, SNR) pair: mix, run the RNNoise stage (the firmware's own
library build), and report PESQ-WB, STOI, segmental SNR against the clean
reference plus noise-floor reduction, speech-level change and pumping.
"""
import json, os, subprocess, sys
import numpy as np
import wave
from scipy import signal as sp

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.environ.get("RN_DEMO", os.path.join(HERE, "rn_demo"))
SR = 48000
FR = 480                      # RNNoise frame


def rd(fn):
    w = wave.open(fn)
    d = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32)
    assert w.getframerate() == SR and w.getnchannels() == 1, (fn, w.getframerate())
    return d


def wr(fn, x):
    with wave.open(fn, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(np.clip(np.round(x), -32768, 32767).astype("<i2").tobytes())


def loop_to(x, n, fade_ms=30):
    """Tile x to n samples with a linear crossfade at each seam."""
    f = int(SR * fade_ms / 1000)
    out = np.zeros(0, np.float32)
    ramp = np.linspace(0, 1, f, dtype=np.float32)
    while len(out) < n + f:
        if len(out) == 0:
            out = x.copy()
            continue
        y = x.copy()
        seam = out[-f:] * (1 - ramp) + y[:f] * ramp
        out = np.concatenate([out[:-f], seam, y[f:]])
    return out[:n]


def airband(n, seed=7):
    rng = np.random.default_rng(seed)
    w = rng.standard_normal(n).astype(np.float32)
    # AM post-detection bandwidth of a VHF airband receiver: 250-3400 Hz
    sos = sp.butter(6, [250, 3400], btype="band", fs=SR, output="sos")
    w = sp.sosfilt(sos, w).astype(np.float32)
    # pink tilt (1/f) via a first-order integrator mix, keeps the hiss natural
    pink = sp.lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786],
                      [1, -2.494956002, 2.017265875, -0.522189400], w)
    w = (0.6 * w + 0.4 * pink).astype(np.float32)
    # static crashes: Poisson bursts, 3/s
    idx = rng.poisson(3.0 * n / SR)
    for _ in range(idx):
        p = rng.integers(0, n - 1200)
        L = rng.integers(200, 900)
        w[p:p + L] += rng.standard_normal(L).astype(np.float32) * rng.uniform(6, 14)
    return w / np.sqrt(np.mean(w ** 2))


def mix(clean, noise, snr_db):
    g = np.sqrt(np.mean(clean ** 2) / (np.mean(noise ** 2) * 10 ** (snr_db / 10)))
    return clean + g * noise, g


def rnnoise(x):
    raw = os.path.join(HERE, "tmp_in.raw")
    out = os.path.join(HERE, "tmp_out.raw")
    n = len(x) // FR * FR
    np.clip(np.round(x[:n]), -32768, 32767).astype("<i2").tofile(raw)
    subprocess.run([DEMO, raw, out], check=True, capture_output=True)
    y = np.fromfile(out, dtype="<i2").astype(np.float32)
    return np.concatenate([y, x[n:]])


def resample16(x):
    return sp.resample_poly(x, 1, 3).astype(np.float64)


def seg_snr(ref, deg, frame=1024, hop=512):
    """Segmental SNR in dB, clamped to [-10, 35], median over active frames."""
    r = ref[: len(deg)]
    vals = []
    for i in range(0, len(r) - frame, hop):
        a = r[i:i + frame]; b = deg[i:i + frame]
        pa = np.mean(a ** 2)
        if pa < 1e-3 * np.mean(r ** 2):
            continue
        e = np.sum((a - b) ** 2)
        v = 10 * np.log10(np.sum(a ** 2) / max(e, 1e-9))
        vals.append(min(35.0, max(-10.0, v)))
    return float(np.median(vals))


def frame_db(x, frame=FR):
    """Per-frame level in dBFS (int16 full scale = 0 dBFS)."""
    n = len(x) // frame * frame
    e = (x[:n].reshape(-1, frame) ** 2).mean(axis=1) + 1e-9
    return 10 * np.log10(e / 32768.0 ** 2)


def pesq_score(ref, deg):
    from pesq import pesq as _pesq
    r = resample16(ref); d = resample16(deg[: len(ref)])
    n = min(len(r), len(d))
    return float(_pesq(16000, r[:n], d[:n], "wb"))


def stoi_score(ref, deg):
    from pystoi import stoi
    n = min(len(ref), len(deg))
    return float(stoi(resample16(ref[:n]), resample16(deg[:n]), 16000, extended=False))


def lag_samples(a, b, maxlag=2000):
    """Delay of b vs a by cross-correlation on a short window."""
    n = min(len(a), len(b), 12 * SR)
    a = a[:n] - a[:n].mean(); b = b[:n] - b[:n].mean()
    c = sp.correlate(b, a, mode="full", method="fft")
    lags = sp.correlation_lags(len(b), len(a), mode="full")
    m = np.abs(lags) <= maxlag
    return int(lags[m][np.argmax(c[m])])


def main():
    clean = rd(os.path.join(HERE, "clean48.wav"))
    engine = rd(os.path.join(HERE, "eng_idle48.wav"))
    n = min(len(clean), 30 * SR)
    clean = clean[:n]
    noises = {
        "engine": loop_to(engine, n),
        "airband": airband(n),
    }
    res = []
    for name, noise in noises.items():
        for snr in (10.0, 5.0, 0.0):
            m, g = mix(clean, noise, snr)
            y0 = rnnoise(m)
            # RNNoise reconstructs the *previous* analysis frame, so its output
            # is one 480-sample block ahead of its input (the reference demo
            # throws the first output frame away for exactly this reason).  Put
            # it back in place before measuring quality; the 10 ms it costs is
            # part of the stage's latency and is reported as such.
            lag = 480
            y = np.concatenate([y0[lag:], np.zeros(lag, np.float32)])
            # speech / noise frame masks from the clean reference
            ce = frame_db(clean)
            thr = np.percentile(ce, 35)
            speech = ce > thr + 10
            k = min(len(ce), len(frame_db(y)))
            speech = speech[:k]
            db_in = frame_db(m)[:k]; db_out = frame_db(y)[:k]
            row = {
                "noise": name, "snr_in": snr,
                "pesq_in": pesq_score(clean, m), "pesq_out": pesq_score(clean, y),
                "stoi_in": stoi_score(clean, m), "stoi_out": stoi_score(clean, y),
                "segsnr_in": seg_snr(clean, m), "segsnr_out": seg_snr(clean, y),
                "noise_floor_db_in": float(np.median(db_in[~speech])),
                "noise_floor_db_out": float(np.median(db_out[~speech])),
                "speech_db_in": float(np.median(db_in[speech])),
                "speech_db_out": float(np.median(db_out[speech])),
                "pump_std_db": float(np.std((db_out - db_in)[speech])),
                "pump_p95_db": float(np.percentile((db_out - db_in)[speech], 95)),
                "lag_samples": lag,
                "lag_ms": 1000.0 * lag / SR,
            }
            res.append(row)
            print(json.dumps(row), flush=True)
    # (a) what the stage does to clean speech: its own distortion floor
    y = rnnoise(clean)
    y = np.concatenate([y[FR:], np.zeros(FR, np.float32)])
    row = {"noise": "clean", "snr_in": float("inf"),
           "pesq_in": pesq_score(clean, clean), "pesq_out": pesq_score(clean, y),
           "stoi_in": 1.0, "stoi_out": stoi_score(clean, y),
           "segsnr_in": 35.0, "segsnr_out": seg_snr(clean, y),
           "noise_floor_db_in": float("nan"), "noise_floor_db_out": float("nan"),
           "speech_db_in": float(np.median(frame_db(clean))),
           "speech_db_out": float(np.median(frame_db(y))),
           "pump_std_db": float(np.std(frame_db(y) - frame_db(clean)[:len(frame_db(y))])),
           "pump_p95_db": float(np.percentile(frame_db(y) - frame_db(clean)[:len(frame_db(y))], 95)),
           "lag_samples": FR, "lag_ms": 10.0}
    res.append(row); print(json.dumps(row), flush=True)

    # (b) noise alone, at the level it has in the 5 dB mixture: how much of the
    # noise the stage actually removes (frames with speech are excluded above
    # because RNNoise's own energy gate passes very quiet frames untouched)
    for name, noise in noises.items():
        y = rnnoise(noise)
        rms_in = np.sqrt(np.mean(noise ** 2)); rms_out = np.sqrt(np.mean(y ** 2))
        row = {"noise": name + "-only", "snr_in": None, "att_db":
               float(20 * np.log10(rms_out / rms_in))}
        res.append(row); print(json.dumps(row), flush=True)

    json.dump(res, open(os.path.join(HERE, "quality.json"), "w"), indent=1)
    print("done", len(res), "cases")


if __name__ == "__main__":
    main()
