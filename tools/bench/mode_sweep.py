"""Key every TX-capable mode of the C62 and measure what leaves the PA.

For each mode: tune the bench to a frequency with that mode, capture with the
B200mini while the radio is keyed, then report carrier level, occupied
bandwidth (99 %) and whether the emission really only happened while keyed.

Run from tools/bench: python3 mode_sweep.py [outdir]
"""
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchproto as B

FREQ = 145_500_000
RATE = 500_000
MODE_NAMES = {0: "NONE", 1: "FM", 2: "DMR", 3: "M17", 4: "TETRA",
              5: "NXDN", 6: "DPMR", 7: "P25", 8: "YSF", 9: "DSTAR",
              10: "AIRBAND", 11: "WFM"}
# The modes whose transmitter exists in this image (TETRA TX is not viable on
# this hardware, WFM is receive only).
MODES = [1, 2]

CAPTURE_S = 3.0
KEY_AT = 1.0
UNKEY_AT = 2.5


def capture(path):
    env = dict(os.environ, UHD_IMAGES_DIR="/tmp/imgs")
    return subprocess.Popen(["./b200", "rx", str(FREQ), str(RATE),
                             str(CAPTURE_S), "30", path], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def load_iq(path):
    raw = np.fromfile(path, dtype=np.complex64)
    if raw.size == 0:
        data = np.fromfile(path, dtype=np.int16)
        if data.size % 2:
            data = data[:-1]
        raw = (data[0::2] + 1j * data[1::2]).astype(np.complex64) / 32768.0
    return raw


def analyse(x, rate, keyed_span, idle_span):
    """Carrier offset, level and 99 % bandwidth inside a narrow window."""
    k0, k1 = int(keyed_span[0] * rate), int(keyed_span[1] * rate)
    i0, i1 = int(idle_span[0] * rate), int(idle_span[1] * rate)
    keyed = x[k0:k1]
    idle = x[i0:i1]
    n = 8192
    if keyed.size < n:
        return None
    seg = keyed[:(keyed.size // n) * n].reshape(-1, n)
    pw = (np.abs(seg) ** 2).sum(axis=1)
    i = int(np.argmax(pw))
    win = seg[i] * np.hanning(n)
    spec = np.fft.fftshift(np.abs(np.fft.fft(win)) ** 2)
    freq = np.fft.fftshift(np.fft.fftfreq(n, 1.0 / rate))
    peak = int(np.argmax(spec))
    # -20 dB bandwidth around the carrier: with noise inside the window a 99 %
    # figure saturates at the window, and the shape of the emission is what the
    # channel plan cares about.
    near = np.abs(freq - freq[peak]) <= 25000
    band = spec[near]
    bfreq = freq[near]
    thr = spec[peak] * 0.01
    above = np.where(band >= thr)[0]
    if above.size < 2:
        return None
    lo = bfreq[above[0]]
    hi = bfreq[above[-1]]
    level = 10 * np.log10(spec[peak] + 1e-20)
    idle_level = (10 * np.log10(np.abs(idle).max() ** 2 + 1e-20)
                  if idle.size else -200.0)
    return freq[peak], level, hi - lo, level - idle_level


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/modesweep"
    os.makedirs(outdir, exist_ok=True)
    print("%-8s %12s %9s %9s %9s" % ("mode", "offset", "level", "bw20", "on/off"))
    for mode in MODES:
        link = B.open_link("/dev/ttyUSB0")
        try:
            link.arm()
        except Exception as exc:
            print("%-8s arm failed: %s" % (MODE_NAMES.get(mode, mode), exc))
            link.close()
            continue
        path = os.path.join(outdir, "mode%d.iq" % mode)
        cap = capture(path)
        time.sleep(0.2)
        try:
            link.tune(FREQ, mode=mode, bw=0, tx=FREQ, power=500)
        except Exception as exc:
            print("%-8s tune failed: %s" % (MODE_NAMES.get(mode, mode), exc))
            cap.kill(); cap.wait()
            continue
        time.sleep(KEY_AT - 0.2)
        try:
            link.ptt(True, tone=1000, level=20)
        except Exception as exc:
            print("%-8s ptt failed: %s" % (MODE_NAMES.get(mode, mode), exc))
        time.sleep(UNKEY_AT - KEY_AT)
        try:
            link.ptt(False)
        except Exception:
            pass
        cap.wait()
        x = load_iq(path)
        if x.size == 0:
            print("%-8s no capture" % MODE_NAMES.get(mode, mode))
            continue
        r = analyse(x, RATE, (KEY_AT + 0.3, UNKEY_AT - 0.1),
                    (0.15, KEY_AT - 0.15))
        if r is None:
            print("%-8s no carrier found" % MODE_NAMES.get(mode, mode))
            continue
        off, level, bw, delta = r
        print("%-8s %+7.1f kHz %7.1f dB %7.1f kHz %7.1f dB" %
              (MODE_NAMES.get(mode, mode), off / 1000.0, level, bw / 1000.0,
               delta))
        try:
            link.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
