"""Parametros y MACs exactos de la UNet de Vpriyareddy12/UNET-Audio-Denoiser.

Arquitectura copiada literalmente de vendor/unet-denoiser/model_unet.py
(TF/Keras). Entrada (128,128,1) = 128 bins de frecuencia x 128 tramas STFT.

Uso: python3 model_cost.py
"""
import json

C = 16  # size_filter_in


def conv(h, w, cin, cout, k):
    return dict(type="conv", h=h, w=w, cin=cin, cout=cout, k=k,
                params=k * k * cin * cout + cout,
                macs=h * w * k * k * cin * cout)


def upconv(h, w, cin, cout, k):
    """Keras: UpSampling2D(2) -> Conv2D(k, stride 1) sobre 2h x 2w."""
    return dict(type="up+conv", h=2 * h, w=2 * w, cin=cin, cout=cout, k=k,
                params=k * k * cin * cout + cout,
                macs=(2 * h) * (2 * w) * k * k * cin * cout)


L = []
# ---- encoder (128x128) ----
L.append(conv(128, 128, 1, C, 3))          # conv1
L.append(conv(128, 128, C, C, 3))
L.append(dict(type="maxpool", h=128, w=128, cin=C))
# ---- level2 (64x64) ----
L.append(conv(64, 64, C, 2 * C, 3))
L.append(conv(64, 64, 2 * C, 2 * C, 3))
L.append(dict(type="maxpool", h=64, w=64, cin=2 * C))
# ---- level3 (32x32) ----
L.append(conv(32, 32, 2 * C, 4 * C, 3))
L.append(conv(32, 32, 4 * C, 4 * C, 3))
L.append(dict(type="maxpool", h=32, w=32, cin=4 * C))
# ---- level4 (16x16) ----
L.append(conv(16, 16, 4 * C, 8 * C, 3))
L.append(conv(16, 16, 8 * C, 8 * C, 3))
L.append(dict(type="maxpool", h=16, w=16, cin=8 * C))
# ---- bottleneck (8x8) ----
L.append(conv(8, 8, 8 * C, 16 * C, 3))
L.append(conv(8, 8, 16 * C, 16 * C, 3))
# ---- decoder ----
L.append(upconv(8, 8, 16 * C, 8 * C, 2))    # up6: 16x16, 256->128
L.append(conv(16, 16, 16 * C, 8 * C, 3))    # conv6 tras concat 128+128
L.append(conv(16, 16, 8 * C, 8 * C, 3))
L.append(upconv(16, 16, 8 * C, 4 * C, 2))   # up7: 32x32 128->64
L.append(conv(32, 32, 8 * C, 4 * C, 3))
L.append(conv(32, 32, 4 * C, 4 * C, 3))
L.append(upconv(32, 32, 4 * C, 2 * C, 2))   # up8: 64x64 64->32
L.append(conv(64, 64, 4 * C, 2 * C, 3))
L.append(conv(64, 64, 2 * C, 2 * C, 3))
L.append(upconv(64, 64, 2 * C, C, 2))       # up9: 128x128 32->16
L.append(conv(128, 128, 2 * C, C, 3))
L.append(conv(128, 128, C, C, 3))
L.append(conv(128, 128, C, 2, 3))           # conv9 borde
L.append(conv(128, 128, 2, 1, 1))           # conv10 1x1 tanh

params = sum(x.get("params", 0) for x in L)
macs = sum(x.get("macs", 0) for x in L)
flops = 2 * macs

# activaciones: pico = mayor tensor vivo (con skip connections hay que
# retener conv1..conv4). Se calcula el pico de bytes float32 e int8.
peak_l1 = 128 * 128 * C          # conv1 vivo durante todo el decoder
peak_l2 = 64 * 64 * 2 * C
peak_l3 = 32 * 32 * 4 * C
peak_skips = peak_l1 + peak_l2 + peak_l3          # skips vivos en el fondo
bottleneck = 8 * 8 * 16 * C
act_f32_peak = 4 * (peak_skips + 2 * bottleneck)  # +-1 buffer doble aprox
act_i8_peak = 1 * (peak_skips + 2 * bottleneck)

print(f"params          : {params:,}  ({params*4/1024:.1f} KB f32, {params/1024:.1f} KB int8)")
print(f"MACs / parche   : {macs:,}  ({macs/1e6:.1f} MMAC)")
print(f"FLOPs / parche  : {flops/1e9:.2f} GFLOP")
print(f"parche cubre    : 128 tramas * hop 63 @8k = {(128-1)*63+255} muestras = {((127*63+255)/8000):.3f} s")
print(f"MAC/s en tiempo real (8 kHz): {macs/(((127*63+255)/8000))/1e9:.2f} GMAC/s")
print(f"act f32 pico    : {act_f32_peak/1024:.0f} KB   act int8 pico: {act_i8_peak/1024:.0f} KB")
print(f"skip tensors    : {peak_skips/1024:.0f} KB f32")
print()
print(f"{'layer':<34}{'shape':>18}{'params':>10}{'MACs':>14}")
for x in L:
    if x["type"] == "maxpool":
        print(f"{x['type']:<34}{f'{x['cin']}x{x['h']}x{x['w']}':>18}{0:>10}{0:>14}")
    else:
        print(f"{x['type']+' '+str(x['k'])+'x'+str(x['k'])+' '+str(x['cin'])+'->'+str(x['cout']):<34}"
              f"{f'{x['cout']}x{x['h']}x{x['w']}':>18}{x['params']:>10,}{x['macs']:>14,}")
json.dump({"params": params, "macs": macs, "layers": L}, open("model_cost.json", "w"), indent=1)
