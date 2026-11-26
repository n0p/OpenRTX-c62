"""One mode, one keying, with the radio's own view of the transmission.

Keys FM at 145.5 MHz through the bench namespace, captures with the B200mini
(RX2), and prints what the firmware says it is doing while keyed: tx power,
tx reason, tx state.  That separates 'the radio is not emitting' from 'the SDR
path is not seeing it' without guessing.
"""
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, "/home/n0p/src/c62/work/merge3/tools/bench")
import benchproto as B

FREQ = 145_500_000
RATE = 1_000_000
CAPTURE = 4.0
OUT = "/tmp/fmcheck.iq"


def main():
    env = dict(os.environ, UHD_IMAGES_DIR="/tmp/imgs")
    if os.path.exists(OUT):
        os.unlink(OUT)
    cap = subprocess.Popen(["./b200", "rx", str(FREQ), str(RATE),
                            str(CAPTURE), "40", OUT],
                           cwd="/home/n0p/src/c62/work/merge3/tools/bench",
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    link = B.open_link("/dev/ttyUSB0")
    link.arm()
    print("tune:", link.tune(FREQ, mode=1, bw=1, tx=FREQ, power=500), flush=True)
    time.sleep(0.5)
    try:
        print("ptt:", link.ptt(True, tone=1000, level=20), flush=True)
    except Exception as exc:
        print("ptt on failed:", exc, flush=True)
    time.sleep(1.0)
    try:
        d = link.link.request(B.P.MSG_DBG_INFO) if hasattr(link, "link") else None
    except Exception:
        d = None
    try:
        st = link.link.status() if hasattr(link, "link") else None
    except Exception:
        st = None
    print("radio while keyed:", flush=True)
    if d is not None:
        print("  txPower =", d.get(0x008A), "txReason =", d.get(0x00EF), flush=True)
    if st is not None:
        print("  tx field =", st.get(B.P.T_TX_STATE), flush=True)
    time.sleep(1.2)
    try:
        link.ptt(False)
    except Exception as exc:
        print("ptt off failed:", exc, flush=True)
    link.close()
    cap.wait()

    if not os.path.exists(OUT):
        print("no capture file", flush=True)
        return
    x = np.fromfile(OUT, dtype=np.complex64)
    if x.size == 0:
        print("empty capture", flush=True)
        return
    p = np.abs(x) ** 2
    print("samples %d peak %.1f dBFS mean %.1f dBFS" %
          (x.size, 10 * np.log10(p.max() + 1e-20),
           10 * np.log10(p.mean() + 1e-20)), flush=True)
    n = 8192
    seg = x[:(x.size // n) * n].reshape(-1, n)
    pw = (np.abs(seg) ** 2).sum(axis=1)
    i = int(np.argmax(pw))
    sp = np.abs(np.fft.fftshift(np.fft.fft(seg[i] * np.hanning(n)))) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(n, 1.0 / RATE))
    print("strongest block at %.1f s, peak offset %+.1f kHz level %.1f dB" %
          (i * n / RATE, f[int(np.argmax(sp))] / 1000.0,
           10 * np.log10(sp.max() + 1e-20)), flush=True)


if __name__ == "__main__":
    main()
