"""Inferencia real de la UNet (pesos preentrenados upstream vbelz/Speech-enhancement,
MIT) sobre audio de 8 kHz, y comparacion con RNNoise v0.1.1.

Pipeline replicado linea a linea de prediction_denoise.py + data_tools.py:
  audio -> bloques de 8064 muestras -> STFT(255, hop 63, center, hann)
        -> |S| en dB ref=max -> scaled_in (x+46)/50 -> UNet
        -> inv_scaled_ou (x*82+6) -> db_noisy - pred -> ISTFT con fase del ruidoso
El `*10` final del repo se omite a proposito (es una ganancia arbitraria que
solo sirve para que write_wav no salga bajisimo).

Uso: .venv/bin/python scripts/unet_infer.py [--rnnoise /ruta/rnnoise_demo]
"""
import argparse
import os
import subprocess
import wave

import h5py
import numpy as np
from scipy.signal import resample_poly

SR = 8000
N_FFT, HOP, FRAME = 255, 63, 8064
WIN = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N_FFT) / N_FFT)


# ------------------------------------------------------------ stft / istft
def stft(x):
    xp = np.pad(x, N_FFT // 2, mode="reflect")
    n = 1 + (len(xp) - N_FFT) // HOP
    fr = np.lib.stride_tricks.as_strided(
        xp, shape=(n, N_FFT), strides=(xp.strides[0] * HOP, xp.strides[0]))
    S = np.fft.rfft(fr * WIN, axis=1).T
    return np.abs(S), S / np.maximum(np.abs(S), 1e-20)


def istft(mag, pha, length):
    fr = np.fft.irfft((mag * pha).T, n=N_FFT)
    out = np.zeros(length + 2 * N_FFT)
    w = np.zeros_like(out)
    for i, f in enumerate(fr):
        out[i * HOP:i * HOP + N_FFT] += f * WIN
        w[i * HOP:i * HOP + N_FFT] += WIN ** 2
    out /= np.maximum(w, 1e-9)
    return out[N_FFT // 2:N_FFT // 2 + length]


def amp_to_db(m, top_db=80.0):
    db = 20 * np.log10(np.maximum(m, 1e-12) / max(np.max(m), 1e-12))
    return np.maximum(db, db.max() - top_db)


# ------------------------------------------------------------------- red
def load_weights(path):
    f = h5py.File(path, "r")
    ks = []
    for i in range(24, 48):
        g = f[f"model_weights/conv2d_{i}/conv2d_{i}"]
        ks.append((g["kernel:0"][()].astype(np.float32), g["bias:0"][()].astype(np.float32)))
    f.close()
    return ks


def im2col(x, k, pad):
    c, h, w = x.shape
    xp = np.pad(x, ((0, 0), (pad, pad), (pad, pad)))
    cols = np.empty((h * w, c * k * k), dtype=np.float32)
    idx = 0
    for i in range(k):
        for j in range(k):
            cols[:, idx * c:(idx + 1) * c] = xp[:, i:i + h, j:j + w].transpose(1, 2, 0).reshape(h * w, c)
            idx += 1
    return cols


def conv(x, kern, bias):
    kh, kw, cin, cout = kern.shape
    assert kh == kw
    cols = im2col(x, kh, kh // 2)
    y = cols @ kern.reshape(-1, cout) + bias
    return y.reshape(x.shape[1], x.shape[2], cout).transpose(2, 0, 1)


def lrelu(x, a=0.3):
    return np.where(x > 0, x, a * x).astype(np.float32)


def pool(x):
    c, h, w = x.shape
    return x.reshape(c, h // 2, 2, w // 2, 2).max(axis=(2, 4))


def conv_asym(x, kern, bias, pt=0, pb=1, pl=0, pr=1):
    """Conv2D con padding asimetrico (TF 'same' con kernel par pone el extra al final)."""
    kh, kw, cin, cout = kern.shape
    c, h, w = x.shape
    xp = np.pad(x, ((0, 0), (pt, pb), (pl, pr)))
    ho, wo = h, w
    cols = np.empty((ho * wo, cin * kh * kw), dtype=np.float32)
    idx = 0
    for i in range(kh):
        for j in range(kw):
            cols[:, idx * cin:(idx + 1) * cin] = xp[:, i:i + ho, j:j + wo].transpose(1, 2, 0).reshape(ho * wo, cin)
            idx += 1
    return (cols @ kern.reshape(-1, cout) + bias).reshape(ho, wo, cout).transpose(2, 0, 1)


def upconv(x, kern, bias):
    """UpSampling2D(2) nearest + Conv2D(k=2, padding='same') tal cual lo hace Keras:
    la rejilla replicada 2h x 2w se convoluciona con pad (0,1,0,1)."""
    c, h, w = x.shape
    u = np.repeat(np.repeat(x, 2, axis=1), 2, axis=2)
    return conv_asym(u, kern, bias)


def unet(x, K):
    """x: (1,128,128) -> (1,128,128). K: 24 kernels en orden de creacion."""
    i = 0
    c11 = lrelu(conv(x, *K[i])); i += 1
    c12 = lrelu(conv(c11, *K[i])); i += 1
    p1 = pool(c12)
    c21 = lrelu(conv(p1, *K[i])); i += 1
    c22 = lrelu(conv(c21, *K[i])); i += 1
    p2 = pool(c22)
    c31 = lrelu(conv(p2, *K[i])); i += 1
    c32 = lrelu(conv(c31, *K[i])); i += 1
    p3 = pool(c32)
    c41 = lrelu(conv(p3, *K[i])); i += 1
    c42 = lrelu(conv(c41, *K[i])); i += 1
    p4 = pool(c42)
    c51 = lrelu(conv(p4, *K[i])); i += 1
    c52 = lrelu(conv(c51, *K[i])); i += 1
    u6 = lrelu(upconv(c52, *K[i])); i += 1
    c6 = lrelu(conv(np.concatenate([c42, u6], 0), *K[i])); i += 1
    c6 = lrelu(conv(c6, *K[i])); i += 1
    u7 = lrelu(upconv(c6, *K[i])); i += 1
    c7 = lrelu(conv(np.concatenate([c32, u7], 0), *K[i])); i += 1
    c7 = lrelu(conv(c7, *K[i])); i += 1
    u8 = lrelu(upconv(c7, *K[i])); i += 1
    c8 = lrelu(conv(np.concatenate([c22, u8], 0), *K[i])); i += 1
    c8 = lrelu(conv(c8, *K[i])); i += 1
    u9 = lrelu(upconv(c8, *K[i])); i += 1
    c9 = lrelu(conv(np.concatenate([c12, u9], 0), *K[i])); i += 1
    c9 = lrelu(conv(c9, *K[i])); i += 1
    c9 = lrelu(conv(c9, *K[i])); i += 1
    out = np.tanh(conv(c9, *K[i]))
    return out[0]


def denoise_unet(y, K, log=None):
    n_blocks = len(y) // FRAME
    out = np.zeros(n_blocks * FRAME)
    for b in range(n_blocks):
        seg = y[b * FRAME:(b + 1) * FRAME]
        mag, pha = stft(seg)
        db = amp_to_db(mag)
        xin = ((db + 46) / 50).astype(np.float32)[None, :, :]
        pred = unet(xin, K) * 82 + 6                     # inv_scaled_ou
        den = db - pred
        out[b * FRAME:(b + 1) * FRAME] = istft(10 ** (den / 20), pha, FRAME)
        if log:
            log.append((db, pred, den))
    return out


# ------------------------------------------------------------------ audio
def read_wav(path, sr=None):
    from scipy.io import wavfile
    sr0, a = wavfile.read(path)
    a = a.astype(np.float64)
    if a.ndim > 1:
        a = a.mean(axis=1)
    if np.issubdtype(np.dtype(a.dtype), np.integer) or np.abs(a).max() > 1.5:
        a = a / 32768.0
    return a, sr0


def write_wav(path, x, sr=SR):
    x = np.clip(x, -1, 1)
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes((x * 32767).astype("<i2").tobytes())
    w.close()


def synth_noise(kind, n, rng):
    w = rng.standard_normal(n)
    f = np.fft.rfftfreq(n, 1 / SR)
    if kind == "airband":
        g = 1 / (1 + np.exp(-(f - 250) / 80)) / (1 + np.exp((f - 3200) / 150)) + 0.02
        y = np.fft.irfft(np.fft.rfft(w) * g, n=n)
    elif kind == "engine":
        t = np.arange(n) / SR
        y = sum((1.0 / k) * np.sin(2 * np.pi * k * 47 * t + rng.uniform(0, 6)) for k in range(1, 18))
        y = y * (1 + 0.35 * np.sin(2 * np.pi * 3.1 * t)) + 0.25 * w
    elif kind == "wind":
        g = 1 / (1 + (f / 400.0) ** 2)
        t = np.arange(n) / SR
        y = np.fft.irfft(np.fft.rfft(w) * g, n=n) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.7 * t) ** 2)
    else:
        y = w
    return y / (np.std(y) + 1e-12)


def snr_db(ref, est):
    n = min(len(ref), len(est))
    ref, est = ref[:n], est[:n]
    return 10 * np.log10(np.sum(ref ** 2) / (np.sum((ref - est) ** 2) + 1e-20))


def segsnr(ref, est, n=160):
    """SNR segmental (20 ms) con suelo de -10 dB / techo 35 dB, en dB."""
    m = min(len(ref), len(est)) // n
    out = []
    for i in range(m):
        r = ref[i * n:(i + 1) * n]
        e = est[i * n:(i + 1) * n]
        if np.sum(r ** 2) < 1e-8:
            continue
        out.append(10 * np.log10(np.sum(r ** 2) / (np.sum((r - e) ** 2) + 1e-20)))
    out = np.clip(out, -10, 35)
    return float(np.mean(out))


def metrics(ref, est):
    from pesq import pesq
    from pystoi import stoi
    n = min(len(ref), len(est))
    ref, est = ref[:n], est[:n]
    try:
        p = pesq(SR, ref.astype(np.float32), est.astype(np.float32), "nb")
    except Exception as e:                                   # noqa: BLE001
        p = float("nan")
    try:
        s = stoi(ref, est, SR, extended=False)
    except Exception:                                        # noqa: BLE001
        s = float("nan")
    g = float(np.dot(est, ref) / (np.dot(est, est) + 1e-20))
    return dict(snr=snr_db(ref, est), snr_g=snr_db(ref, g * est), g=g,
                segsnr=segsnr(ref, est), pesq=p, stoi=s)


def rnnoise(y8, demo, tmpdir="work/poc"):
    """RNNoise v0.1.1 trabaja a 48 kHz mono int16: subir x6, procesar, bajar /6.
    El demo descarta la primera trama (10 ms) y espera muestras en escala int16."""
    os.makedirs(tmpdir, exist_ok=True)
    y48 = resample_poly(y8, 6, 1)
    n = (len(y48) // 480) * 480
    pcm = np.clip(y48[:n], -1, 1)
    pcm = pcm / (np.max(np.abs(pcm)) + 1e-12) * 0.5      # -6 dBFS
    fi, fo = f"{tmpdir}/rn_in.raw", f"{tmpdir}/rn_out.raw"
    with open(fi, "wb") as f:
        f.write((pcm * 32767).astype("<i2").tobytes())
    subprocess.run([demo, fi, fo], check=True, capture_output=True)
    out48 = np.frombuffer(open(fo, "rb").read(), dtype="<i2").astype(np.float64) / 32768
    # el demo descarta la 1a trama *procesada*: la salida ya empieza en t=10 ms,
    # asi que no hay que rellenar; verificado por correlacion cruzada (lag 0).
    return resample_poly(out48, 1, 6)[:len(y8)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rnnoise", default="")
    ap.add_argument("--outdir", default="work/poc")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    K = load_weights("vendor/upstream_weights/model_unet.h5")
    print("kernels:", [k[0].shape for k in K])

    # voz real de 8 kHz decodificada por el path DMR (misma clase de senal que la C62)
    voice, sr = read_wav("/home/n0p/src/c62/work/opendmr-port/audio/osr.wav")
    assert sr == SR
    nb = len(voice) // FRAME
    clean = voice[:nb * FRAME]
    dur = nb * FRAME
    rng = np.random.default_rng(1)

    print(f"\nvoz: osr.wav {dur/SR:.2f} s ({nb} bloques de 1.008 s)")
    hdr = (f"{'ruido':<9}{'SNRin':>7}{'proceso':>9}{'SNR':>7}{'SNR*':>7}{'PESQ':>7}"
           f"{'STOI':>7}{'segSNR':>8}")
    print(hdr)
    print("-" * len(hdr))
    rows = []
    for kind in ("airband", "engine", "wind"):
        noise = synth_noise(kind, dur, rng)
        for snr_in in (0, 5, 10, 20):
            nz = noise * np.sqrt(np.mean(clean ** 2) / np.mean(noise ** 2) / 10 ** (snr_in / 10))
            noisy = clean + nz
            m0 = metrics(clean, noisy)
            print(f"{kind:<9}{snr_in:>7}{'ruidoso':>9}{m0['snr']:>7.1f}{m0['snr_g']:>7.1f}"
                  f"{m0['pesq']:>7.2f}{m0['stoi']:>7.3f}{m0['segsnr']:>8.1f}")
            dr = None
            du = denoise_unet(noisy, K)
            imp = [snr_db(clean[i * FRAME:(i + 1) * FRAME], du[i * FRAME:(i + 1) * FRAME]) >
                   snr_db(clean[i * FRAME:(i + 1) * FRAME], noisy[i * FRAME:(i + 1) * FRAME])
                   for i in range(nb)]
            m1 = metrics(clean, du)
            print(f"{'':<9}{'':>7}{'UNet':>9}{m1['snr']:>7.1f}{m1['snr_g']:>7.1f}"
                  f"{m1['pesq']:>7.2f}{m1['stoi']:>7.3f}{m1['segsnr']:>8.1f}"
                  f"  [{sum(imp)}/{nb} bloques mejor, g={m1['g']:.2f}]")
            if a.rnnoise:
                dr = rnnoise(noisy, a.rnnoise)
                m2 = metrics(clean, dr)
                print(f"{'':<9}{'':>7}{'RNNoise':>9}{m2['snr']:>7.1f}{m2['snr_g']:>7.1f}"
                      f"{m2['pesq']:>7.2f}{m2['stoi']:>7.3f}{m2['segsnr']:>8.1f}"
                      f"  [g={m2['g']:.2f}]")
                rows.append((kind, snr_in, m0, m1, m2))
            else:
                rows.append((kind, snr_in, m0, m1, None))
            if snr_in == 5:
                write_wav(f"{a.outdir}/clean_{kind}.wav", clean)
                write_wav(f"{a.outdir}/noisy_{kind}.wav", noisy)
                write_wav(f"{a.outdir}/unet_{kind}.wav", du)
                if a.rnnoise:
                    write_wav(f"{a.outdir}/rnnoise_{kind}.wav", dr)
    np.save(f"{a.outdir}/metrics.npy", np.array(rows, dtype=object), allow_pickle=True)


if __name__ == "__main__":
    main()
