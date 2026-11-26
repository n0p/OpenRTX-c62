"""Spectrum shape of one keyed mode, sample by sample of frequency."""
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, "/home/n0p/src/c62/work/merge3/tools/bench")
import benchproto as B

FREQ = 145_500_000
RATE = 500_000
CAPTURE = 3.0
MODE = int(sys.argv[1]) if len(sys.argv) > 1 else 2
GAIN = sys.argv[2] if len(sys.argv) > 2 else "0"
OUT = "/tmp/shape%d.iq" % MODE


def main():
    env = dict(os.environ, UHD_IMAGES_DIR="/tmp/imgs")
    if os.path.exists(OUT):
        os.unlink(OUT)
    cap = subprocess.Popen(["./b200", "rx", str(FREQ), str(RATE),
                            str(CAPTURE), GAIN, OUT],
                           cwd="/home/n0p/src/c62/work/merge3/tools/bench",
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    link = B.open_link("/dev/ttyUSB0")
    link.arm()
    link.tune(FREQ, mode=MODE, bw=1, tx=FREQ, power=500)
    time.sleep(0.6)
    link.ptt(True, tone=1000, level=20)
    time.sleep(1.0)
    try:
        link.ptt(False)
    except Exception:
        pass
    link.close()
    cap.wait()

    x = np.fromfile(OUT, dtype=np.complex64)
    if x.size == 0:
        print("empty")
        return
    n = 32768
    seg = x[:(x.size // n) * n].reshape(-1, n)
    pw = (np.abs(seg) ** 2).sum(axis=1)
    i = int(np.argmax(pw))
    spec = np.fft.fftshift(np.abs(np.fft.fft(seg[i] * np.hanning(n))) ** 2)
    freq = np.fft.fftshift(np.fft.fftfreq(n, 1.0 / RATE))
    peak = int(np.argmax(spec))
    p0 = spec[peak]
    print("mode %d, peak at %+.1f kHz, level %.1f dB" %
          (MODE, freq[peak] / 1000.0, 10 * np.log10(p0 + 1e-20)))
    print("%8s %10s" % ("offset", "rel dB"))
    for off in (-24, -20, -12, -8, -6, -4, -2, -1, 0, 1, 2, 4, 6, 8, 12, 20, 24):
        idx = int(np.argmin(np.abs(freq - (freq[peak] + off * 1000))))
        rel = 10 * np.log10(spec[idx] / (p0 + 1e-20) + 1e-20)
        print("%6d kHz %10.1f" % (off, rel))


if __name__ == "__main__":
    main()
