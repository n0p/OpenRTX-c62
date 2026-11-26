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
MODES = [1]

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


def occupied_bw(x, rate):
    """99 % occupied bandwidth of the strongest stretch."""
    n = 4096
    if x.size < n:
        return 0.0
    seg = x[: (x.size // n) * n].reshape(-1, n)
    power = np.abs(seg) ** 2
    idx = int(np.argmax(power.sum(axis=1)))
    spec = np.abs(np.fft.fftshift(np.fft.fft(seg[idx] * np.hanning(n)))) ** 2
    total = spec.sum()
    if total <= 0:
        return 0.0
    cum = np.cumsum(spec) / total
    lo = int(np.searchsorted(cum, 0.005))
    hi = int(np.searchsorted(cum, 0.995))
    return (hi - lo) * rate / n


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/modesweep"
    os.makedirs(outdir, exist_ok=True)
    link = B.open_link("/dev/ttyUSB0")
    link.arm()
    print("%-8s %10s %10s %10s" % ("mode", "carrier", "bw99", "idle_delta"))
    for mode in MODES:
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
        rate = RATE
        n = x.size
        keyed = x[int(KEY_AT * rate): int((UNKEY_AT - 0.1) * rate)]
        idle = x[int((UNKEY_AT + 0.2) * rate):]
        peak_k = 20 * np.log10(np.abs(keyed).max() + 1e-12)
        peak_i = 20 * np.log10(np.abs(idle).max() + 1e-12) if idle.size else -200
        bw = occupied_bw(keyed, rate)
        print("%-8s %9.1f dB %9.1f kHz %9.1f dB" %
              (MODE_NAMES.get(mode, mode), peak_k, bw / 1000.0, peak_k - peak_i))
    link.close()


if __name__ == "__main__":
    main()
