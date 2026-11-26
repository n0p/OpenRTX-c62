"""Auditoria del pipeline DSP de UNET-Audio-Denoiser con audio real de 8 kHz.

No hay pesos preentrenados en el repo: aqui no se mide calidad de red, se mide
el TECHO del algoritmo que la red aprende (resta de magnitudes en dB con la fase
del ruidoso, por bloques de 1 s) y su sensibilidad a errores del estimador.

Uso: python3 scripts/pipeline_audit.py
"""
import wave

import numpy as np

SR = 8000
N_FFT = 255
HOP = 63
FRAME = 8064          # args.py --frame_length
DIM = N_FFT // 2 + 1  # 128 bins
WIN = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N_FFT) / N_FFT)   # hann periodico


def stft(x, center=True):
    if center:
        x = np.pad(x, N_FFT // 2, mode="reflect")
    n = 1 + (len(x) - N_FFT) // HOP
    fr = np.lib.stride_tricks.as_strided(
        x, shape=(n, N_FFT), strides=(x.strides[0] * HOP, x.strides[0]))
    S = np.fft.rfft(fr * WIN, axis=1).T
    return np.abs(S), np.exp(1j * np.angle(S))


def istft(mag, phase, length):
    fr = np.fft.irfft((mag * phase).T, n=N_FFT)
    out = np.zeros(length + 2 * N_FFT)
    wsum = np.zeros_like(out)
    for i, f in enumerate(fr):
        out[i * HOP:i * HOP + N_FFT] += f * WIN
        wsum[i * HOP:i * HOP + N_FFT] += WIN ** 2
    out = out / np.maximum(wsum, 1e-9)
    return out[N_FFT // 2:N_FFT // 2 + length]


def db(x):
    return 20.0 * np.log10(np.maximum(x, 1e-12))


def read_wav(path):
    w = wave.open(path)
    n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    dt = {2: "<i2", 4: "<i4"}[sw]
    a = np.frombuffer(w.readframes(n), dtype=dt).astype(np.float64)
    if ch > 1:
        a = a.reshape(-1, ch).mean(axis=1)
    if sr != SR:
        t = np.arange(int(len(a) * SR / sr)) * sr / SR
        a = np.interp(t, np.arange(len(a)), a)
    return a / (np.max(np.abs(a)) + 1e-12)


def synth_noise(kind, n, rng):
    """Ruido sintetico con la forma espectral que interesa (no son grabaciones)."""
    w = rng.standard_normal(n)
    f = np.fft.rfftfreq(n, 1 / SR)
    if kind == "airband":       # hiss de FM estrecha (banda 250-3200 Hz)
        g = 1 / (1 + np.exp(-(f - 250) / 80)) / (1 + np.exp((f - 3200) / 150)) + 0.02
        y = np.fft.irfft(np.fft.rfft(w) * g, n=n)
    elif kind == "engine":      # motor: armonicos 47 Hz con AM
        t = np.arange(n) / SR
        y = sum((1.0 / k) * np.sin(2 * np.pi * k * 47 * t + rng.uniform(0, 6))
                for k in range(1, 18))
        y = y * (1 + 0.35 * np.sin(2 * np.pi * 3.1 * t)) + 0.25 * w
    elif kind == "wind":        # viento: paso bajo modulado
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


def main():
    voice = read_wav("/home/n0p/src/c62/work/c62dig/samples/voice_shihao_8k.wav")
    e = [np.mean(voice[i:i + FRAME] ** 2) for i in range(0, len(voice) - FRAME, 1000)]
    v = voice[1000 * int(np.argmax(e)):][:FRAME]

    # ---- (a) geometria
    mag, pha = stft(v)
    print(f"voz real (voice_shihao_8k.wav): {len(voice)/SR:.2f} s @ {SR} Hz; ventana = {FRAME} muestras "
          f"= {FRAME/SR*1000:.0f} ms")
    print(f"(a) STFT(255, hop 63, center) -> {mag.shape[0]} bins x {mag.shape[1]} tramas"
          f" = la entrada (128,128) del repo")
    print(f"    paso {HOP/SR*1000:.2f} ms/trama, resolucion {SR/(N_FFT+1):.2f} Hz, "
          f"ventana {N_FFT/SR*1000:.1f} ms, Nyquist {SR/2:.0f} Hz")
    rec = istft(mag, pha, FRAME)
    print(f"    ISTFT con la magnitud intacta: error rms {np.sqrt(np.mean((rec-v)**2)):.1e}"
          f" -> OLA hann/hop 63 cierra sin perdidas")

    # ---- (b) techo del algoritmo: |v| + fase del ruidoso, por bloques de 1 s
    rng = np.random.default_rng(0)
    print("\n(b) techo del algoritmo (magnitud perfecta, fase del ruidoso, bloques de 1 s)")
    print("    'suelo' = atenuacion maxima permitida (0 dB = resta sin limite, como el repo)")
    print(f"{'ruido':<9}{'SNRin':>6}" + "".join(f"{s:>9}" for s in
                                                ("sin limite", "suelo -20", "suelo -12", "suelo -6")))
    for kind in ("airband", "engine", "wind"):
        noise = synth_noise(kind, 8 * FRAME, rng)
        nz = noise[FRAME:2 * FRAME]
        for snr_in in (0, 5, 10, 20):
            nz_scaled = nz * np.sqrt(np.mean(v ** 2) / np.mean(nz ** 2) / 10 ** (snr_in / 10))
            mix = v + nz_scaled
            m_mag, m_pha = stft(mix)
            v_mag = stft(v)[0]
            # etiqueta del repo (db_mix - db_clean): perfecta por construccion
            ratio = db(m_mag) - db(v_mag)          # dB que hay que restar por bin
            line = f"{kind:<9}{snr_in:>6}"
            for floor in (None, -20, -12, -6):
                att = ratio if floor is None else np.maximum(ratio, floor)
                y = istft(10 ** ((db(m_mag) - att) / 20), m_pha, FRAME)
                line += f"{snr_db(v, y)-snr_in:>9.1f}"
            print(line)

    # ---- (c) sensibilidad a un error constante del estimador
    print("\n(c) incremento de SNR (dB) con un error constante del estimador en la resta")
    print(f"{'ruido':<9}{'SNRin':>6}" + "".join(f"{d:>8}" for d in (-9, -6, -3, 0, 3, 6, 9)))
    for kind in ("airband", "engine", "wind"):
        noise = synth_noise(kind, 8 * FRAME, rng)
        nz = noise[FRAME:2 * FRAME]
        for snr_in in (0, 10):
            nz_scaled = nz * np.sqrt(np.mean(v ** 2) / np.mean(nz ** 2) / 10 ** (snr_in / 10))
            mix = v + nz_scaled
            m_mag, m_pha = stft(mix)
            v_mag = stft(v)[0]
            line = f"{kind:<9}{snr_in:>6}"
            for d in (-9, -6, -3, 0, 3, 6, 9):
                y = istft(10 ** ((db(v_mag) - d) / 20), m_pha, FRAME)
                line += f"{snr_db(v, y) - snr_in:>8.1f}"
            print(line)


if __name__ == "__main__":
    main()
