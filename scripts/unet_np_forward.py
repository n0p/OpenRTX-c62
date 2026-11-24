"""Reimplementacion numpy de la UNet de UNET-Audio-Denoiser + contabilidad de
memoria de activaciones (planificador greedy) y medida de tiempo en host.

No hay pesos: los tensores se inicializan aleatorios. Solo se mide FORMA, COSTE
y MEMORIA; la calidad de salida no significa nada con pesos aleatorios.

Uso: python3 scripts/unet_np_forward.py [--time]
"""
import sys
import time

import numpy as np

# --------------------------------------------------------------- grafo
# cada op: (nombre, tipo, entradas, forma_salida, macs)
# forma = (C,H,W)
C = 16
G = []


def T(c, h, w):
    return (c, h, w)


nodes = {}


def emit(name, kind, ins, out_shape, weight_shape=None, stride=1, pad="same"):
    nodes[name] = dict(name=name, kind=kind, ins=ins, out=out_shape,
                       weight=weight_shape, stride=stride, pad=pad)
    G.append(name)


def macs_conv(cin, cout, k, h, w):
    return h * w * k * k * cin * cout


# encoder
emit("conv1_1", "conv", ["input"], T(C, 128, 128), (C, 1, 3, 3))
emit("conv1_2", "conv", ["conv1_1"], T(C, 128, 128), (C, C, 3, 3))
emit("pool1", "maxpool", ["conv1_2"], T(C, 64, 64), stride=2)
emit("conv2_1", "conv", ["pool1"], T(2 * C, 64, 64), (2 * C, C, 3, 3))
emit("conv2_2", "conv", ["conv2_1"], T(2 * C, 64, 64), (2 * C, 2 * C, 3, 3))
emit("pool2", "maxpool", ["conv2_2"], T(2 * C, 32, 32), stride=2)
emit("conv3_1", "conv", ["pool2"], T(4 * C, 32, 32), (4 * C, 2 * C, 3, 3))
emit("conv3_2", "conv", ["conv3_1"], T(4 * C, 32, 32), (4 * C, 4 * C, 3, 3))
emit("pool3", "maxpool", ["conv3_2"], T(4 * C, 16, 16), stride=2)
emit("conv4_1", "conv", ["pool3"], T(8 * C, 16, 16), (8 * C, 4 * C, 3, 3))
emit("conv4_2", "conv", ["conv4_1"], T(8 * C, 16, 16), (8 * C, 8 * C, 3, 3))
emit("pool4", "maxpool", ["conv4_2"], T(8 * C, 8, 8), stride=2)
emit("conv5_1", "conv", ["pool4"], T(16 * C, 8, 8), (16 * C, 8 * C, 3, 3))
emit("conv5_2", "conv", ["conv5_1"], T(16 * C, 8, 8), (16 * C, 16 * C, 3, 3))
# decoder: upsample nearest x2 + conv k=2 'same'
emit("up6", "upconv", ["conv5_2"], T(8 * C, 16, 16), (8 * C, 16 * C, 2, 2))
emit("cat6", "concat", ["conv4_2", "up6"], T(16 * C, 16, 16))
emit("conv6_1", "conv", ["cat6"], T(8 * C, 16, 16), (8 * C, 16 * C, 3, 3))
emit("conv6_2", "conv", ["conv6_1"], T(8 * C, 16, 16), (8 * C, 8 * C, 3, 3))
emit("up7", "upconv", ["conv6_2"], T(4 * C, 32, 32), (4 * C, 8 * C, 2, 2))
emit("cat7", "concat", ["conv3_2", "up7"], T(8 * C, 32, 32))
emit("conv7_1", "conv", ["cat7"], T(4 * C, 32, 32), (4 * C, 8 * C, 3, 3))
emit("conv7_2", "conv", ["conv7_1"], T(4 * C, 32, 32), (4 * C, 4 * C, 3, 3))
emit("up8", "upconv", ["conv7_2"], T(2 * C, 64, 64), (2 * C, 4 * C, 2, 2))
emit("cat8", "concat", ["conv2_2", "up8"], T(4 * C, 64, 64))
emit("conv8_1", "conv", ["cat8"], T(2 * C, 64, 64), (2 * C, 4 * C, 3, 3))
emit("conv8_2", "conv", ["conv8_1"], T(2 * C, 64, 64), (2 * C, 2 * C, 3, 3))
emit("up9", "upconv", ["conv8_2"], T(C, 128, 128), (C, 2 * C, 2, 2))
emit("cat9", "concat", ["conv1_2", "up9"], T(2 * C, 128, 128))
emit("conv9_1", "conv", ["cat9"], T(C, 128, 128), (C, 2 * C, 3, 3))
emit("conv9_2", "conv", ["conv9_1"], T(C, 128, 128), (C, C, 3, 3))
emit("conv9_3", "conv", ["conv9_2"], T(2, 128, 128), (2, C, 3, 3))
emit("out", "conv", ["conv9_3"], T(1, 128, 128), (1, 2, 1, 1))

# ------------------------------------------------------------ utilidades


def im2col(x, k, pad):
    """x: (C,H,W) -> (H*W, C*k*k) con padding 'same'."""
    c, h, w = x.shape
    xp = np.pad(x, ((0, 0), (pad, pad), (pad, pad)))
    cols = np.empty((h * w, c * k * k), dtype=np.float32)
    idx = 0
    for i in range(k):
        for j in range(k):
            patch = xp[:, i:i + h, j:j + w]
            cols[:, idx * c:(idx + 1) * c] = patch.transpose(1, 2, 0).reshape(h * w, c)
        idx += 1
    return cols


def run_timed():
    rng = np.random.default_rng(0)
    vals = {}
    W = {}
    for n in G:
        d = nodes[n]
        ws = d["weight"]
        if ws is not None:
            W[n] = (rng.standard_normal(np.prod(ws), dtype=np.float32) * 0.05).reshape(ws)
    vals["input"] = rng.standard_normal((1, 128, 128), dtype=np.float32)
    per_layer = []
    for n in G:
        d = nodes[n]
        t0 = time.perf_counter()
        if d["kind"] == "conv":
            x = vals[d["ins"][0]]
            cout, cout_, k, _ = d["weight"]
            cin, h, w = x.shape
            kk = d["weight"][2]
            cols = im2col(x, kk, kk // 2)
            wmat = W[n].reshape(cout, -1).T                  # (C*k*k, cout)
            y = (cols @ wmat).T.reshape(cout, h, w)
            if cout == 1:
                y = np.tanh(y)
            else:
                y = np.where(y > 0, y, 0.3 * y)             # LeakyReLU(0.3)
        elif d["kind"] == "maxpool":
            x = vals[d["ins"][0]]
            c, h, w = x.shape
            y = x.reshape(c, h // 2, 2, w // 2, 2).max(axis=(2, 4))
        elif d["kind"] == "upconv":
            x = vals[d["ins"][0]]
            cout, cout_, k, _ = d["weight"]
            cin, h, w = x.shape
            # equivalente exacto a UpSampling2D(2)+Conv2D(k=2,'same') salvo el
            # corrimiento de medio pixel del pad asimetrico: 2x2 stride 2, sin solape
            wm = W[n].reshape(cout * 4, cin)                  # 4 taps (a,b) por canal
            z = (wm @ x.reshape(cin, h * w)).reshape(cout, 2, 2, h, w)
            y = z.transpose(0, 3, 1, 4, 2).reshape(cout, 2 * h, 2 * w)
            y = np.where(y > 0, y, 0.3 * y)
        else:  # concat
            y = np.concatenate([vals[i] for i in d["ins"]], axis=0)
        vals[n] = y
        per_layer.append((n, d, (time.perf_counter() - t0) * 1e3))
    return vals, per_layer


def memory_plan():
    """Planificador greedy: primer hueco libre tras la ultima lectura."""
    first, last = {}, {}
    for i, n in enumerate(G):
        d = nodes[n]
        for s in d["ins"]:
            first.setdefault(s, i)
            last[s] = i
        first.setdefault(n, i)
        last.setdefault(n, i)
    # tamano en bytes (int8) de cada tensor vivo
    sizes = {"input": 128 * 128}
    for n in G:
        c, h, w = nodes[n]["out"]
        sizes[n] = c * h * w
    items = sorted(sizes, key=lambda k: -sizes[k])
    offs = {}
    for k in items:
        a, b = first[k], last[k]
        used = sorted((offs[o] if False else offs[o], offs[o] + sizes[o])
                      for o in offs
                      if not (last[o] < a or first[o] > b or o == k))
        base = 0
        for s, e in used:
            if base + sizes[k] <= s:
                break
            base = max(base, e)
        offs[k] = base
    peak = max(offs[o] + sizes[o] for o in offs)
    naive = sum(sizes.values())
    return peak, naive, sizes, offs, first, last


if __name__ == "__main__":
    macs = 0
    for n in G:
        d = nodes[n]
        if d["kind"] in ("conv", "upconv"):
            cout, cin, k, _ = d["weight"]
            c, h, w = d["out"]
            macs += macs_conv(cin, cout, k, h, w)
    print(f"MACs del grafo (calculados sobre las formas): {macs:,}")
    peak, naive, sizes, offs, first, last = memory_plan()
    print(f"memoria de activaciones, plan greedy int8 : {peak/1024:.1f} KiB "
          f"(suma ingenua {naive/1024:.1f} KiB)")
    top = sorted(sizes, key=lambda k: -sizes[k])[:6]
    print("tensores mayores:", ", ".join(f"{k}={sizes[k]/1024:.0f}K" for k in top))
    if "--time" in sys.argv:
        vals, per = run_timed()
        tot = sum(x[2] for x in per)
        print(f"forward numpy/OpenBLAS en este host: {tot:.1f} ms/parche "
              f"({macs/2/1e9:.2f} GFLOP -> {macs*2/(tot/1e3)/1e9:.1f} GFLOP/s efectivos)")
        print("mas caras:", ", ".join(f"{n} {t:.0f}ms" for n, d, t in sorted(per, key=lambda x: -x[2])[:5]))
        print("salida:", vals["out"].shape, "rango", float(vals["out"].min()), float(vals["out"].max()))
