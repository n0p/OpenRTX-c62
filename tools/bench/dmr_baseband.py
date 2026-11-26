"""Capture the receiver's baseband in DMR and decode it on the host.

Keys nothing: the radio is receiving.  The bench tunes the radio to the
frequency with the DMR receiver running and takes several short captures of the
DSP's baseband; each goes through the host demodulator.  If the host decodes a
capture the firmware is fine and the defect is between the demodulator and the
speaker; if it does not, the signal that reaches the demodulator is already
broken.
"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, "/home/n0p/src/c62/work/merge3/tools/bench")
import benchproto as B

FREQ = 446_112_500
OUT = "/tmp/dmrbb"
DECODER = "/home/n0p/src/c62/work/merge3/tests/host/dmr_tx_host/build/dmr_rx"


def main():
    os.makedirs(OUT, exist_ok=True)
    link = B.open_link("/dev/ttyUSB0")
    link.arm()
    print("tune:", link.tune(FREQ, mode=2, bw=0, power=500), flush=True)
    files = []
    for i in range(5):
        path = os.path.join(OUT, "cap%d.s16" % i)
        try:
            samples, meta = link.capture(500)   # half a second
        except Exception as exc:
            print("capture %d failed: %s" % (i, exc), flush=True)
            continue
        if not samples:
            print("capture %d empty" % i, flush=True)
            continue
        data = struct.pack("<%dh" % len(samples), *samples)
        with open(path, "wb") as f:
            f.write(data)
        files.append(path)
        print("captured %d: %d samples at %d Hz, overflow %s" %
              (i, len(samples), meta.get("rate", 0), meta.get("overflow")),
              flush=True)
    link.close()

    for path in files:
        print("== %s" % path, flush=True)
        try:
            out = subprocess.run([DECODER, path], capture_output=True, text=True,
                                 timeout=120)
            tail = [l for l in (out.stdout or "").splitlines() if l.strip()][-6:]
            print("\n".join(tail) or "(no output)", flush=True)
        except FileNotFoundError:
            print("no host decoder at %s" % DECODER, flush=True)
            break
        except Exception as exc:
            print("decoder failed: %s" % exc, flush=True)


if __name__ == "__main__":
    main()
