"""Coste de variantes reducidas de la UNet (canales C, profundidad, n. de tramas).

Objetivo: encontrar que habria que recortar para que quepa en el presupuesto del
CSK6011B (SRAM compartida ~640 KiB para el motor del NPU, HiFi4 1,2 GMAC/s pico)
manteniendo una latencia utilizable.

Uso: python3 scripts/unet_variants.py
"""


def graph(C=16, Hf=128, Wt=128, levels=5):
    """Devuelve (params, macs, lista de tensores (nombre, bytes_int8), pico_act)."""
    L = []          # (tipo, cin, cout, k, h, w)
    tensors = []    # (nombre, bytes) en orden de creacion

    def conv(cin, cout, k, h, w):
        L.append(("conv", cin, cout, k, h, w))
        tensors.append((f"c{len(tensors)}", cout * h * w))

    def pool(c, h, w):
        L.append(("pool", c, c, 0, h // 2, w // 2))
        tensors.append((f"p{len(tensors)}", c * (h // 2) * (w // 2)))

    def upconv(cin, cout, h, w):
        L.append(("upconv", cin, cout, 2, 2 * h, 2 * w))
        tensors.append((f"u{len(tensors)}", cout * 2 * h * 2 * w))

    def cat(c, h, w):
        L.append(("cat", c, c, 0, h, w))
        tensors.append((f"m{len(tensors)}", c * h * w))

    h, w, c = Hf, Wt, C
    skips = []
    conv(1, c, 3, h, w)
    for lv in range(levels):
        conv(c, c, 3, h, w)
        skips.append((c, h, w))
        if lv == levels - 1:
            break
        pool(c, h, w)
        h, w = h // 2, w // 2
        conv(c, 2 * c, 3, h, w)
        c *= 2
    # decoder
    for lv in range(levels - 2, -1, -1):
        sc, sh, sw = skips[lv]
        upconv(c, sc, h, w)
        cat(2 * sc, sh, sw)
        conv(2 * sc, sc, 3, sh, sw)
        conv(sc, sc, 3, sh, sw)
        c, h, w = sc, sh, sw
    conv(c, 2, 3, h, w)
    conv(2, 1, 1, h, w)

    params = macs = 0
    for t, cin, cout, k, hh, ww in L:
        if t in ("conv", "upconv"):
            params += k * k * cin * cout + cout
            macs += hh * ww * k * k * cin * cout

    # planificador greedy sobre la lista de tensores en orden, con vida = hasta
    # su ultimo uso (los skips viven hasta el decoder)
    order = [n for n, _ in tensors]
    sz = dict(tensors)
    n = len(order)
    # vida: los skips son los tensores "conv terminado en nivel lv": aproximacion
    # conservadora -> primer tensor de cada nivel vive hasta su concat
    first = {name: i for i, name in enumerate(order)}
    last = {name: i for i, name in enumerate(order)}
    # skips: conv que cierra cada nivel del encoder
    enc_skips = []
    idx = 0
    # reconstruir indices de skips a partir de la estructura
    i = 0
    per_level = []
    # nivel 0: conv(1->C), conv(C->C) ; niveles siguientes: pool, conv, conv
    i = 2
    enc_skips.append(order[1])
    for lv in range(1, levels):
        enc_skips.append(order[i + 1])
        i += 3
    # posicion del concat de cada nivel en el decoder
    dec = order[i:]
    # el decoder tiene, por nivel, (upconv, cat, conv, conv)
    for lv, sk in enumerate(reversed(enc_skips)):
        cidx = first[dec[lv * 4 + 1]]
        last[sk] = cidx
    items = sorted(sz, key=lambda k: -sz[k])
    offs = {}
    for k in items:
        a, b = first[k], last[k]
        used = sorted((offs[o], offs[o] + sz[o]) for o in offs
                      if not (last[o] < a or first[o] > b))
        base = 0
        for s, e in used:
            if base + sz[k] <= s:
                break
            base = max(base, e)
        offs[k] = base
    peak = max(offs[o] + sz[o] for o in offs)
    return params, macs, peak


def report(name, C, Hf, Wt, levels):
    p, m, a = graph(C, Hf, Wt, levels)
    dur = ((Wt - 1) * 63 + 255) / 8000.0     # segundos de audio por parche
    rate = m / dur
    print(f"{name:<34}{p:>10,}{m:>15,.0f}{rate/1e6:>9.0f} MMAC/s"
          f"{a/1024:>9.0f} KiB{a/1024/640*100:>7.0f}%{dur*1000:>8.0f} ms")


if __name__ == "__main__":
    print(f"{'variante':<34}{'params':>10}{'MAC/parche':>15}{'tasa':>9}{'act pico':>9}{'%640K':>7}{'ventana':>8}")
    report("original C16 128x128 (5 niveles)", 16, 128, 128, 5)
    report("C16 128x64", 16, 128, 64, 5)
    report("C16 128x32", 16, 128, 32, 5)
    report("C8 128x128", 8, 128, 128, 5)
    report("C8 128x64", 8, 128, 64, 5)
    report("C8 128x32", 8, 128, 32, 5)
    report("C8 64x64 (bins 2 kHz)", 8, 64, 64, 5)
    report("C8 64x32 (bins 2 kHz)", 8, 64, 32, 5)
    report("C8 96x16 (freq 96)", 8, 96, 16, 5)
    report("C6 64x16", 6, 64, 16, 5)
    report("C4 64x16", 4, 64, 16, 5)
    report("C4 96x12 (96 ms @ 62Hz)", 4, 96, 12, 5)
