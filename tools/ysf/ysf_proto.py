"""
Yaesu System Fusion (C4FM) receive prototype -- host, Python.

Validates the frame structure, FICH and DCH coding chains and the voice
segmentation against a real discriminator-audio capture.

Reference chain (all cited in docs/RX_FUSION.md):
  4800 sym/s, 2 bits/symbol = 9600 bit/s; 100 ms frame = 960 bits
  [40 sync][200 FICH][720 payload]
  sync = 0xD471C9634D, dibit -> level {0:+1, 1:+3, 2:-1, 3:-3}
"""
import numpy as np

# ---------------------------------------------------------------- constants
SYNC = 0xD471C9634D
SYNC_BITS = np.array([(SYNC >> (39 - i)) & 1 for i in range(40)], dtype=np.uint8)
LEVELS = {0: 1.0, 1: 3.0, 2: -1.0, 3: -3.0}
SYMRATE = 4800

# ------------------------------------------------------------ 4FSK demod
def rrc(beta=0.2, sps=10, span=8):
    n = np.arange(-span * sps, span * sps + 1)
    t = n / sps
    h = np.zeros_like(t, dtype=float)
    for i, ti in enumerate(t):
        if abs(ti) < 1e-9:
            h[i] = 1.0 - beta + 4 * beta / np.pi
        elif beta != 0 and abs(abs(ti) - 1 / (4 * beta)) < 1e-9:
            h[i] = beta / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) +
                                        (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
        else:
            h[i] = (np.sin(np.pi * ti * (1 - beta)) + 4 * beta * ti * np.cos(np.pi * ti * (1 + beta))) / \
                   (np.pi * ti * (1 - (4 * beta * ti) ** 2))
    return h / np.sqrt((h ** 2).sum())


def demod(x, fs=48000, sps=10):
    """Matched-filter and return the oversampled symbol waveform + fractional
    interpolation helper. Returns dict with mf signal."""
    x = x - np.mean(x)
    h = rrc(sps=sps)
    mf = np.convolve(x, h, mode='same')
    return mf


def slice_symbols(y, n):
    """Slice n symbols starting at y[0] with 1 sample/symbol using adaptive
    levels taken from the histogram (median of outer/inner magnitudes)."""
    lev = 0.5 * (np.percentile(np.abs(y), 95))
    # inner/outer from sorted magnitudes: assume 3:1
    outer = np.percentile(np.abs(y), 90)
    inner = outer / 3.0
    if inner <= 0:
        return None
    out = np.empty(n, dtype=np.int8)
    for i, v in enumerate(y[:n]):
        a = abs(v)
        sign = 1 if v >= 0 else -1
        out[i] = sign * (3 if a > 2 * inner else 1)
    return out


def levels_to_dibits(sym):
    """symbol values {-1,+1,-3,+3} -> dibits {0,1,2,3} (level 0=+1,1=+3,2=-1,3=-3)."""
    m = {1.0: 0, 3.0: 1, -1.0: 2, -3.0: 3}
    return np.array([m[float(s)] for s in sym], dtype=np.uint8)


def dibits_to_bits(d):
    b = np.empty(len(d) * 2, dtype=np.uint8)
    b[0::2] = (d >> 1) & 1
    b[1::2] = d & 1
    return b


# ------------------------------------------------------------- FEC / codes
def crc16(bits):
    poly = (1 << 12) | (1 << 5) | 1
    crc = 0
    for bit in bits:
        crc = ((crc << 1) | int(bit)) & 0x1FFFF
        if crc & 0x10000:
            crc = (crc & 0xFFFF) ^ poly
    return crc ^ 0xFFFF


def _golay_tables():
    """(23,12) Golay, generator 0xC75.  The code is perfect: the 2048
    patterns of weight <= 3 map bijectively onto the 2048 syndromes."""
    from itertools import combinations
    tab = np.zeros(2048, dtype=np.uint32)
    tab[_golay_syn(0)] = 0
    for w in (1, 2, 3):
        for combo in combinations(range(23), w):
            pat = 0
            for p in combo:
                pat |= 1 << p
            tab[_golay_syn(pat)] = pat
    return tab


def _golay_syn(code):
    rem = code & 0x7FFFFF
    for i in range(22, 10, -1):
        if rem & (1 << i):
            rem ^= 0xC75 << (i - 11)
    return rem & 0x7FF


_GOLAY = None


def golay24_decode(w24):
    """w24: 24-bit word, 12 data bits MSB-first, 11 parity, 1 overall parity.
    Returns (data12, ok)."""
    global _GOLAY
    if _GOLAY is None:
        _GOLAY = _golay_tables()
    a23 = (w24 >> 1) & 0x7FFFFF
    e = int(_GOLAY[_golay_syn(a23)])
    corr = a23 ^ e
    # overall parity check on the corrected 23-bit word
    ok = (bin(corr).count('1') & 1) == (w24 & 1)
    return corr >> 11, ok


def viterbi(bits, g0=0o23, g1=0o35, k=5):
    """Hard-decision Viterbi, rate 1/2, K=5, MSB-first output 1 bit per step.
    bits: iterable of 0/1 soft or hard. Returns (decoded bits, total metric)."""
    nstates = 1 << (k - 1)
    INF = 1 << 28
    pm = np.full(nstates, INF, dtype=np.int64)
    pm[0] = 0
    hist = np.zeros((len(bits) // 2, nstates), dtype=np.uint8)
    for t in range(len(bits) // 2):
        r0 = bits[2 * t]
        r1 = bits[2 * t + 1]
        new = np.full(nstates, INF, dtype=np.int64)
        for s in range(nstates):
            for b in (0, 1):
                # state holds the previous 4 input bits, s bit0 = most recent
                reg = ((s << 1) | b) & 0x1F
                o0 = bin(reg & g0).count('1') & 1
                o1 = bin(reg & g1).count('1') & 1
                ns = ((s << 1) | b) & (nstates - 1)
                m = pm[s] + (o0 != r0) + (o1 != r1)
                if m < new[ns]:
                    new[ns] = m
                    hist[t, ns] = (s << 1) | b
        pm = new
    s = int(np.argmin(pm))
    out = np.zeros(len(bits) // 2, dtype=np.uint8)
    for t in range(len(bits) // 2 - 1, -1, -1):
        v = int(hist[t, s])
        out[t] = v & 1
        s = v >> 1
    return out, int(pm.min())


def deinterleave(bits, rows, cols):
    """Inverse of write-rows/read-columns."""
    inp = np.asarray(bits, dtype=np.uint8)
    buf = np.zeros(rows * cols, dtype=np.uint8)
    for i in range(rows):
        for j in range(cols):
            buf[j + i * cols] = inp[i + j * rows]
    return buf


def dewhiten(bits, seed=0x1C9, period=512):
    out = np.array(bits, dtype=np.uint8)
    for off in range(0, len(out), period):
        lfsr = seed
        for i in range(min(period, len(out) - off)):
            bit = lfsr & 1
            fb = ((lfsr >> 4) ^ lfsr) & 1
            lfsr = (lfsr >> 1) | (fb << 8)
            out[off + i] ^= bit
    return out


def bits_to_bytes(bits):
    out = bytearray()
    for i in range(0, len(bits) - 7, 8):
        v = 0
        for j in range(8):
            v = (v << 1) | int(bits[i + j])
        out.append(v)
    return bytes(out)


# ------------------------------------------------------------------- FICH
FICH_FIELDS = [('fi', 0, 2), ('cs', 2, 2), ('cm', 4, 2), ('bn', 6, 2),
               ('bt', 8, 2), ('fn', 10, 3), ('ft', 13, 3), ('mr', 18, 3),
               ('vp', 21, 1), ('dt', 22, 2), ('st', 24, 1), ('sc', 25, 7)]


def parse_fich(bits200):
    buf = deinterleave(bits200, 20, 10)
    dec, metric = viterbi(buf)
    info48 = np.zeros(48, dtype=np.uint8)
    ok = True
    for i in range(4):
        w = 0
        for j in range(24):
            w = (w << 1) | int(dec[i * 24 + j])
        data, gok = golay24_decode(w)
        ok = ok and gok
        for j in range(12):
            info48[i * 12 + j] = (data >> (11 - j)) & 1
    crc = crc16(info48)
    f = {}
    for name, off, ln in FICH_FIELDS:
        v = 0
        for j in range(ln):
            v = (v << 1) | int(info48[off + j])
        f[name] = v
    f['golay_ok'] = ok
    f['crc'] = crc
    f['err'] = 0 if (crc == 0 and ok) else (-1 if not ok else -2)
    f['metric'] = metric
    return f, info48


# -------------------------------------------------------------------- DCH
def parse_dch(bits360):
    """V/D1 DCH: 360 bits -> 20 ASCII chars + CRC over the first 176 bits."""
    dew = dewhiten(bits360)
    buf = deinterleave(dew, 20, 18)
    dec, metric = viterbi(buf)
    crc = crc16(dec[:176])
    chars = bits_to_bytes(dec[:160])
    return chars, crc, metric, dec
