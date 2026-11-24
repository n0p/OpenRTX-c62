#!/usr/bin/env python3
"""Generate src/dmr_tables.h: ROM tables for the CSK6011B AMBE+2 decoder.

All constants are derived from the reference implementation
(OpenDMR/decoder/ambe3600x2450_const.h and mbe_unvoiced_fft.c) and from the
AMBE+2 model equations, evaluated in IEEE-754 double and rounded to single
precision exactly the way the reference does it at run time.  The output is
checked into the repository, so building the decoder needs no Python.

Usage:  tools/gen_tables.py [--src <OpenDMR dir>] > src/dmr_tables.h
"""
import argparse
import math
import re
import sys

import numpy as np

f32 = np.float32


def rf(x):
    """Round a Python float (double) to the nearest float32, print as %a-safe hex."""
    return float(f32(x))


def camf(name, vals, dims, ctype="float", per_line=6, comment=None):
    """Emit a C array definition (flat, row-major)."""
    out = []
    if comment:
        out.append("/* %s */" % comment)
    out.append("static const %s %s%s = {" % (ctype, name,
               "".join("[%d]" % d for d in dims)))
    n = len(vals)
    if len(dims) == 2:                     # emit row braces: [rows][cols]
        cols = dims[1]
        for r in range(dims[0]):
            chunk = vals[r * cols:(r + 1) * cols]
            txt = ", ".join(("%.9ef" % v) if ctype == "float" else ("%d" % v) for v in chunk)
            out.append("    { " + txt + " },")
    else:
        for i in range(0, n, per_line):
            chunk = vals[i:i + per_line]
            if ctype == "float":
                txt = ", ".join("%.9ef" % v for v in chunk)
            else:
                txt = ", ".join("%d" % v for v in chunk)
            out.append("    " + txt + ",")
    out.append("};")
    return "\n".join(out)


def parse_reference(path):
    src = open(path).read()
    # strip comments
    out = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = (j + 2) if j >= 0 else n
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = j if j >= 0 else n
        else:
            out.append(src[i])
            i += 1
    src = "".join(out)
    arrs = {}
    for m in re.finditer(r"static const (float|int)\s+(\w+)\s*((?:\[[^\]]*\])+)\s*=\s*\{", src):
        name = m.group(2)
        dims = [int(d) for d in re.findall(r"\[(\d+)\]", m.group(3))]
        start = m.end() - 1
        depth, j = 0, start
        while j < len(src):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = src[start:j + 1]
        nums = [float(x) for x in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", body)]
        arrs[name] = (m.group(1), dims, nums)
    return arrs


def varray_body(path, name):
    """Extract the initialiser numbers of a plain float array from a .c file."""
    src = open(path).read()
    m = re.search(r"%s\s*\[[^\]]*\]\s*=\s*\{" % name, src)
    start = m.end() - 1
    depth, j = 0, start
    while j < len(src):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    body = src[start:j + 1]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [float(x) for x in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", body)]


# ---------------------------------------------------------------- Golay (23,12)
GENPOL = 0x00C75      # g(x) = x^11+x^10+x^6+x^5+x^4+x^2+1
X22 = 0x00400000
X11 = 0x00000800
MASK12 = 0xFFFFF800


def syndrome_23127(pattern):
    """Remainder of pattern (23-bit vector) modulo g(x), split-free version."""
    aux = X22
    if pattern >= X11:
        while pattern & MASK12:
            while not (aux & pattern):
                aux >>= 1
            pattern ^= (aux // X11) * GENPOL
    return pattern


def golay_lut():
    """syndrome -> top 12 bits of the coset leader (Golay(23,12) is perfect:
    weight<=3 patterns cover all 2048 syndromes)."""
    lut = [0] * 2048
    seen = [None] * 2048
    pats = [0]
    for i in range(23):
        pats.append(1 << i)
    for i in range(23):
        for j in range(i + 1, 23):
            pats.append((1 << i) | (1 << j))
    for i in range(23):
        for j in range(i + 1, 23):
            for k in range(j + 1, 23):
                pats.append((1 << i) | (1 << j) | (1 << k))
    for e in pats:
        s = syndrome_23127(e)
        assert seen[s] is None, "collision at syndrome %d" % s
        seen[s] = e
        lut[s] = e >> 11
    assert all(x is not None for x in seen), "not all syndromes covered"
    return lut


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/home/n0p/src/c62/work/OpenDMR/decoder")
    a = ap.parse_args()

    ref = parse_reference(a.src + "/ambe3600x2450_const.h")
    W0 = ref["AmbeW0table"][2]
    LT = [int(x) for x in ref["AmbeLtable"][2]]
    VUV = [int(x) for x in ref["AmbeVuv"][2]]
    LMP = [int(x) for x in ref["AmbeLmprbl"][2]]
    DG = ref["AmbeDg"][2]
    P24 = ref["AmbePRBA24"][2]
    P58 = ref["AmbePRBA58"][2]
    H5 = ref["AmbeHOCb5"][2]
    H6 = ref["AmbeHOCb6"][2]
    H7 = ref["AmbeHOCb7"][2]
    H8 = ref["AmbeHOCb8"][2]
    WS = varray_body(a.src + "/mbe_unvoiced_fft.c", "Ws_synthesis")
    assert len(WS) == 211, len(WS)
    WSV = varray_body(a.src + "/mbelib_const.h", "Ws")   # 321-tap voiced window
    assert len(WSV) == 321, len(WSV)

    pi = math.pi
    # --- W0 in rad/sample: reference does  w0 = f0 * (float)2 * M_PI  (double)
    w0_rad = [rf(float(f32(W0[i])) * 2.0 * pi) for i in range(120)]
    # --- Ri DCT table: cosf((M_PI * (float)(m-1) * ((float)i - 0.5f)) / 8.0f)
    ri = [0.0] * 81
    for m in range(1, 9):
        for i in range(1, 9):
            arg = rf((pi * float(m - 1) * (float(i) - 0.5)) / 8.0)
            ri[m * 9 + i] = rf(math.cos(arg))
    # --- per-block IDCT table: cosf((M_PI * (float)(k-1) * ((float)j - 0.5f)) / (float)ji)
    idct, off = [], [0] * 19
    for ji in range(1, 18):
        off[ji] = len(idct)
        for j in range(1, ji + 1):
            for k in range(1, ji + 1):
                arg = rf((pi * float(k - 1) * (float(j) - 0.5)) / float(ji))
                idct.append(rf(math.cos(arg)))
    # --- log2f of the integer band count L
    log2i = [0.0] * 57
    for l in range(2, 57):
        log2i[l] = rf(math.log2(float(l)))
    # --- unvoiced window, WOLA weights
    win256 = [0.0] * 256
    for i in range(256):
        idx = i - 128
        win256[i] = rf(WS[idx + 105]) if -105 <= idx <= 105 else 0.0
    wp = [0.0] * 160
    wc = [0.0] * 160
    for n in range(160):
        wp[n] = rf(WS[n + 105]) if n - 0 + 105 < 211 else 0.0
        wc[n] = rf(WS[n + 105 - 160]) if (n + 105 - 160) >= 0 else 0.0
    inv = []
    for n in range(160):
        d = f32(f32(wp[n] * wp[n]) + f32(wc[n] * wc[n]))
        inv.append(rf(1.0 / float(d)) if float(d) > 1e-10 else 0.0)
    # --- FFT twiddles (128-point complex, 256-point real)
    tw128 = [rf(math.cos(-2.0 * pi * k / 128.0)) for k in range(128)]
    tw128i = [rf(math.sin(-2.0 * pi * k / 128.0)) for k in range(128)]
    tw256 = [rf(math.cos(-2.0 * pi * k / 256.0)) for k in range(129)]
    tw256i = [rf(math.sin(-2.0 * pi * k / 256.0)) for k in range(129)]
    bitrev = []
    for i in range(128):
        r = 0
        for b in range(7):
            if i & (1 << b):
                r |= 1 << (6 - b)
        bitrev.append(r)
    lut = golay_lut()

    w = sys.stdout.write
    w("/* dmr_tables.h -- GENERATED by tools/gen_tables.py, do not edit.\n"
      " *\n"
      " * ROM (flash) tables for the AMBE+2 (DMR 3600x2450) vocoder: quantiser\n"
      " * codebooks, DCT/HOC pre-computed cosines, unvoiced-synthesis window and\n"
      " * WOLA weights, FFT twiddles and the Golay(23,12) coset-leader table.\n"
      " * Every float is the single-precision value the reference computes at run\n"
      " * time, so no transcendental work is needed on the target.\n"
      " * SPDX-FileCopyrightText: 2026 OpenRTX Contributors\n"
      " *\n"
      " * SPDX-License-Identifier: GPL-2.0-or-later\n"
      " */\n"
      "#ifndef DMR_TABLES_H\n#define DMR_TABLES_H\n#include <stdint.h>\n\n")
    w(camf("dmr_w0", [rf(x) for x in W0], [120], comment="f0 = w0/(2*pi), AMBE 2450 fundamental frequency codebook"))
    w("\n" + camf("dmr_w0_rad", w0_rad, [120], comment="w0 in rad/sample (f0 * 2 * pi, double-rounded like the reference)"))
    w("\n" + camf("dmr_l_table", LT, [120], "uint8_t", 20, "number of harmonics L per b0"))
    w("\n" + camf("dmr_vuv", VUV, [32, 8], "uint8_t", 8, "V/UV codebook"))
    w("\n" + camf("dmr_lmprbl", LMP, [57, 4], "uint8_t", 8, "log2Ml prediction residual block lengths Ji[1..4]"))
    w("\n" + camf("dmr_dg", [rf(x) for x in DG], [32], comment="gain codebook"))
    w("\n" + camf("dmr_prba24", [rf(x) for x in P24], [512, 3], comment="Gm[2..4] codebook"))
    w("\n" + camf("dmr_prba58", [rf(x) for x in P58], [128, 4], comment="Gm[5..8] codebook"))
    w("\n" + camf("dmr_hoc_b5", [rf(x) for x in H5], [32, 4], comment="HOC block 1"))
    w("\n" + camf("dmr_hoc_b6", [rf(x) for x in H6], [16, 4], comment="HOC block 2"))
    w("\n" + camf("dmr_hoc_b7", [rf(x) for x in H7], [16, 4], comment="HOC block 3"))
    w("\n" + camf("dmr_hoc_b8", [rf(x) for x in H8], [8, 4], comment="HOC block 4"))
    w("\n" + camf("dmr_ri_cos", ri, [9, 9], comment="cos(pi*(m-1)*(i-0.5)/8), [m][i]"))
    w("\n" + camf("dmr_idct_off", off, [19], "uint16_t", 19, "offset of the ji x ji block inside dmr_idct_cos"))
    w("\n" + camf("dmr_idct_cos", idct, [len(idct)], comment="cos(pi*(k-1)*(j-0.5)/ji) blocks for ji=1..17"))
    w("\n" + camf("dmr_log2_int", log2i, [57], comment="log2f of the integer L"))
    wsv_prev = [rf(WSV[n + 160]) for n in range(160)]
    wsv_cur = [rf(WSV[n]) for n in range(160)]
    w("\n" + camf("dmr_win256", win256, [256], comment="unvoiced synthesis window Ws_synthesis[i-128]"))
    w("\n" + camf("dmr_ws_prev", wsv_prev, [160], comment="voiced window Ws[n+160] (previous frame, fade out)"))
    w("\n" + camf("dmr_ws_cur", wsv_cur, [160], comment="voiced window Ws[n] (current frame, fade in)"))
    w("\n" + camf("dmr_wola_wprev", wp, [160], comment="WOLA w(n)"))
    w("\n" + camf("dmr_wola_wcurr", wc, [160], comment="WOLA w(n-160)"))
    w("\n" + camf("dmr_wola_inv", inv, [160], comment="1/(wprev^2+wcurr^2), 0 where degenerate"))
    w("\n" + camf("dmr_tw128", tw128, [128], comment="cos(-2*pi*k/128)"))
    w("\n" + camf("dmr_tw128i", tw128i, [128], comment="sin(-2*pi*k/128)"))
    w("\n" + camf("dmr_tw256", tw256, [129], comment="cos(-2*pi*k/256)"))
    w("\n" + camf("dmr_tw256i", tw256i, [129], comment="sin(-2*pi*k/256)"))
    w("\n" + camf("dmr_bitrev128", bitrev, [128], "uint8_t", 16, "bit-reversal permutation for the 128-point FFT"))
    w("\n" + camf("dmr_golay_hi", lut, [2048], "uint16_t", 12,
                 "syndrome -> top 12 bits of the coset leader (Golay(23,12))"))
    w("\n#endif /* DMR_TABLES_H */\n")


if __name__ == "__main__":
    main()
