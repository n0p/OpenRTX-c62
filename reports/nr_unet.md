# UNet-Audio-Denoiser (Vpriyareddy12) para la Retevis C62 — evaluación

**Fecha:** 2026-11-24 · **Rama:** `c62/nr-unet` (worktree desde `master` `1958c7c3`) ·
**Ámbito:** `/home/n0p/src/c62/work/nr-unet` · Sin acceso a radios ni a servidores de producción.

## Resumen ejecutivo

| Pregunta | Respuesta |
|---|---|
| ¿Qué es el repo? | **Copia literal de `vbelz/Speech-enhancement`** (MIT, 2019). El `LICENSE` del fork **borra el copyright original**. El fork **no trae pesos**; el upstream **sí** (`weights/model_unet.h5`, 23 MB) — se han usado para el PoC. |
| ¿Arquitectura? | U-Net 2-D de 5 niveles sobre **espectrograma STFT de 128 bins × 128 tramas = 1,008 s de audio a 8 kHz**. 1,94 M parámetros, **858 MMAC por parche**, salida = **mapa de ruido en dB** (no máscara, no waveform). TF/Keras. |
| ¿Cabe en el M33? | **No.** 17,0 MMAC por trama de 20 ms = **2,8 GHz de M33 a 1 MAC/ciclo**. Es **150× el coste del decodificador AMBE** (113 k instrucciones/trama). |
| ¿Cabe en el HiFi4? | **No.** 833 MMAC/s = **69 % del pico de MACs** del DSP (1,2 GMAC/s); con eficiencia realista de convolución (30–50 %) → **140–230 %** de un núcleo que además lleva la cadena de audio. |
| ¿Cabe en el NPU? | **El tiempo sí, la memoria no.** ~70 ms por segundo de audio (≈7 % del NPU, 10–15 GMAC/s medidos por el vendor), pero **0,9–1,0 MiB de activaciones int8** frente a los ~640 KiB de SRAM que ve el motor, y el tensor mayor (512 KiB) ya no cabe. El de fábrica (`model-cmlp.bin`, 429 KB) es 4,4× más pequeño. |
| ¿Latencia? | **1,03 s de bloque** más el tiempo de inferencia. Estructural: `amplitude_to_db(ref=max)` necesita el parche completo antes de normalizar. Inaceptable para PTT/voz. |
| ¿Calidad (PoC medido)? | Sobre 33 bloques de voz DMR real: **RNNoise gana SNR, segSNR y STOI en las 12 condiciones**; en PESQ la UNet gana 5 de 12 (hiss y viento a 0–10 dB) y pierde las 4 de 20 dB. La UNet **atenúa siempre ~17 dB** (el `*10` del repo es el parche), **rompe la transparencia** (STOI 0,690 vs 0,724 del ruidoso a 20 dB) y sobre voz limpia la deja a **SNR 1,2 dB**. |
| ¿Merece la pena? | **No.** Por 96× los MACs y 22× la memoria de RNNoise no se gana calidad medible, y no cabe en el SoC sin recortarla a una variante que ya no es esta. Lo que sí tiene sentido: la **clase** de modelo (CNN sobre espectrograma) ya está en el chip de fábrica (CMLP/`anker_unet`, 429 KB) y en RNNoise. |

---

## 1. Análisis del repositorio

### 1.1 Procedencia y licencia

| Dato | Valor |
|---|---|
| URL del encargo | `https://github.com/Vpriyareddy12/UNET-Audio-Denoiser` |
| HEAD | `04807d2` (2026-04-08), **un único commit**, 17 ficheros, 612 KB |
| Origen real | **`github.com/vbelz/Speech-enhancement`** (Vincent Belz, 2019, artículo en *Towards Data Science*) |
| Prueba | `data_tools.py` y `model_unet.py` son **byte a byte idénticos** (`diff` vacío); `args.py`, `train_model.py`, `prediction_denoise.py` difieren en 6–20 líneas |
| Licencia del fork | MIT, `Copyright (c) 2026 Vpriyareddy12` |
| Licencia upstream | MIT, `Copyright (c) 2019, vincent belz` → **el fork elimina el aviso de copyright original**, cosa que la MIT prohíbe ("shall be included in all copies") |
| Pesos en el fork | **no hay** (ni `.h5`, ni releases, ni enlace) |
| Pesos en el upstream | **sí**: `weights/model_unet.h5` (23 MB, incluye estado del optimizador) + `model_unet.json` |
| Datos de entrenamiento | LibriSpeech (audiolibros en inglés, 8 kHz) + ruidos ESC-50/Columbia, **según el README del upstream**; no hay script de evaluación ni métricas implementadas |
| Dependencias | TF 1.15 / Keras, librosa 0.7.2, soundfile, streamlit. `librosa.output.write_wav` **fue eliminado en librosa 0.10**: el repo **no corre** con un librosa actual |
| Tests | ninguno en el fork (el upstream tenía uno) |

**El README miente en varios puntos** (verificado contra el código): dice "squared **mel** spectrograms" (el código hace STFT lineal, sin mel), dice que evalúa "SNR/MSE/MAE" (no hay ni una línea de evaluación, solo `metrics=['mae']` de Keras) y atribuye resultados a un entrenamiento de 10 épocas cuyo PNG está copiado del upstream.

### 1.2 Arquitectura exacta (`model_unet.py`)

U-Net 2-D clásica, `size_filter_in = 16`, kernel 3×3, `padding='same'`, `LeakyReLU` (α = 0,3 por defecto de Keras), MaxPool 2×2, `Dropout(0.5)` en el cuello (no-op en inferencia), upsampling **nearest 2× + Conv2D(k=2)**, salida `Conv2D(1,1,tanh)`.

| Bloque | Operación | Salida (C×H×W) | params | MAC |
|---|---|---|---|---|
| enc1 | conv 3×3 1→16, conv 3×3 16→16 | 16×128×128 | 2 480 | 40,1 M |
| enc2 | pool, conv 16→32, conv 32→32 | 32×64×64 | 13 888 | 56,6 M |
| enc3 | pool, conv 32→64, conv 64→64 | 64×32×32 | 55 424 | 56,6 M |
| enc4 | pool, conv 64→128, conv 128→128 | 128×16×16 | 221 440 | 56,6 M |
| bot | pool, conv 128→256, conv 256→256 | 256×8×8 | 885 248 | 56,6 M |
| dec6 | up2+conv 256→128, cat, conv 256→128, conv 128→128 | 128×16×16 | 573 824 | 146,5 M |
| dec7 | up2+conv 128→64, cat, conv 128→64, conv 64→64 | 64×32×32 | 146 752 | 146,5 M |
| dec8 | up2+conv 64→32, cat, conv 64→32, conv 32→32 | 32×64×64 | 36 656 | 146,5 M |
| dec9 | up2+conv 32→16, cat, conv 32→16, conv 16→16, conv 16→2 | 2×128×128 | 9 298 | 151,0 M |
| out | conv 1×1 2→1 + tanh | 1×128×128 | 3 | 0,03 M |
| **Total** | **24 convs** | | **1 941 093** | **858,6 M** |

Cifras medidas con `scripts/model_cost.py` y verificadas de forma independiente en la reimplementación numpy (`scripts/unet_np_forward.py`, que reconstruye el grafo y suma MACs sobre las formas reales: **858 554 368**, idéntico).

### 1.3 Entrada: qué es exactamente la "imagen" de 128×128

Replicado de `args.py` + `data_tools.py` y **verificado numéricamente** con audio real (`scripts/pipeline_audit.py`):

| Parámetro | Valor | Consecuencia |
|---|---|---|
| `sample_rate` | **8000 Hz** (no 16 kHz) | Nyquist 4 kHz |
| `frame_length` | 8064 muestras = **1,008 s** | es el parche completo, no una trama |
| `n_fft` | **255** | ventana hann de 31,9 ms, resolución **31,25 Hz** |
| `hop_length_fft` | **63** | **7,88 ms/trama**, 128 tramas por parche |
| bins | 128 (0…4 kHz) | ejes = (frecuencia, tiempo) |
| magnitud | `librosa.amplitude_to_db(mag, ref=np.max)` | dB **relativos al máximo del parche** |
| escalado entrada | `(x + 46) / 50` | rango útil ≈ 96 dB |
| escalado salida | `(x − 6) / 82`, inverso `x·82 + 6` | el modelo puede restar de −76 a +88 dB |
| salida | mapa de 128×128 = **el ruido que hay que restar, en dB** | `denoised_dB = noisy_dB − pred` |
| reconstrucción | ISTFT con la **fase del ruidoso**, OLA hann/hop 63 | error de reconstrucción 5·10⁻¹⁷ (sin pérdidas) |

Es decir: **no es una máscara** (nada limita la resta a [0, 1]) y **no es waveform**. La resta en dB es algebraicamente exacta: la etiqueta de entrenamiento es `db_ruidoso − db_limpio`, y restarla del ruidoso devuelve el limpio en la referencia del ruidoso. El problema no es la etiqueta; es que el modelo tiene que *estimar* ese cociente sin conocer el limpio.

### 1.4 Validación de mi reimplementación

Contra la salida del propio autor (`demo_data/save_predictions/denoise_t2.wav`), misma entrada:

* correlación **0,9895**, ganancia relativa **9,93** (el `*10` de `prediction_denoise.py`), SNR del residuo 16,8 dB.
* Es decir: mi port a numpy reproduce el pipeline oficial; las cifras de §4 no son un artefacto mío.

Defectos reales del código (no del README):

1. **`denoise_long = ... * 10`**: +20 dB arbitrarios al final, para que `write_wav` no salga bajo. Esconde que **el modelo atenúa ~17 dB de media** (§4.3).
2. `ref=np.max` sobre el parche completo ⇒ **no causal**: hay que tener el segundo entero antes de normalizar. Es la causa de la latencia de 1 s.
3. Sin suelo de atenuación: en bins donde el estimador se pasa, la resta puede llegar a −76 dB (se come armónicos de la voz).
4. `librosa.output` (eliminado), `sf` importado y sin usar, `sample_rate=8000` mientras la C62 trabaja a 48 kHz en el altavoz.
5. Silencio ⇒ `ref = max = 0` ⇒ división por cero en `amplitude_to_db`.

---

## 2. Coste real por destino

### 2.1 Supuestos (escritos, porque el resultado depende de ellos)

| Supuesto | Valor | Fuente |
|---|---|---|
| Carga a tiempo real | **833 MMAC/s** para audio continuo a 8 kHz | 858,6 MMAC / 1,032 s |
| Por trama DMR de 20 ms | **17,0 MMAC** (1,008 s = 50,4 tramas) | derivado |
| M33 | 300 MHz, **1 MAC/ciclo** (M33 con DSP), C compilado realista 1 MAC / 2–3 ciclos | `docs/DMR_VOCODER.md`, `docs/DMR_TX.md` |
| AMBE+2 decode (referencia de coste del proyecto) | **113 370 instrucciones / trama de 20 ms** = 2,3–3,0 % del M33 | `docs/DMR_VOCODER.md` |
| AMBE+2 encode (referencia de techo) | 1 427 272 instrucciones/trama = **28–38 % del M33** | `docs/DMR_TX.md` |
| HiFi4 | 300 MHz, **4 MAC/ciclo** ⇒ **1,2 GMAC/s pico** | `reports/dsp_factory.md` §6 |
| Eficiencia de convolución en HiFi4 | 30 % (C escalar), 50 % (bucles bien escritos), 100 % = pico teórico inalcanzable | supuesto, se dan los tres |
| NPU (VENUS/CSK6) | 128 GOPS nominales; **medido por el vendor: MobileNetV2 (300 MMAC) en 20,5 ms ⇒ ≈14,6 GMAC/s** con pesos en PSRAM | `npu-rnnoise/docs/model_perf.md` (`renet18_v1_fp32`, 2 864 KB, 20,5 ms) |
| SRAM visible al motor | ~640 KiB (VENUS/CSK6), compartida con el CP y sus buffers | `npu-rnnoise/docs/model_choice.md` |
| SRAM del AP | **256 KiB**, 203,7 KB ya usados (77,7 %) ⇒ ~52 KB libres | `platform/targets/c62/c62.dts` (`reg = <0x80000 DT_SIZE_K(256)>`), `docs/DMR_TX.md` |

### 2.2 Cuenta

**M33 (Cortex-M33 @ 300 MHz)**

```
17,0 MMAC por trama de 20 ms → a 1 MAC/ciclo hacen falta 17,0 M ciclos = 56,8 ms
56,8 ms de un presupuesto de 20 ms = 284 % del núcleo (pico teórico, imposible)
a 1 MAC / 2,5 ciclos (C real sin intrinsics) → 710 %
relativo al decodificador AMBE: 17,0 M / 113 k = 150× su coste
⇒ haría falta un M33 de 2,8 GHz. No es un problema de optimización.
```

**HiFi4 (@ 300 MHz, 1,2 GMAC/s pico)**

```
833 MMAC/s ÷ 1 200 MMAC/s = 69 % del pico de MACs
con 50 % de eficiencia → 139 % del DSP
con 30 % de eficiencia → 231 % del DSP
```
Aun en el caso irreal de 100 % de eficiencia, se queda sin margen para la cadena de audio de fábrica (CAE, EQ, resampler), que ya vive ahí. **No cabe.**

**NPU (Thinker, VENUS)**

```
833 MMAC/s ÷ 14,6 GMAC/s (medido del vendor) = 57 ms por segundo de audio = 5,7 % de ocupación
con 10 GMAC/s (modelo más profundo, peor eficiencia) = 83 ms/s = 8,3 %
ancho de banda de pesos: 1,9 MB por inferencia ⇒ 1,9 MB/s (techo medido de PSRAM ~140 MB/s) ✔
```
**El tiempo no es el problema en el NPU. La memoria sí** (§2.3) **y la latencia también** (§2.4).

### 2.3 Memoria

| Concepto | Tamaño | ¿Dónde? |
|---|---|---|
| Pesos fp32 | 7,40 MiB | — |
| Pesos **int8** | **1,85 MiB** | PSRAM (8 MB) ✔ |
| Activaciones (planificador greedy, int8) | **0,9–1,0 MiB** | **SM/SRAM del motor** ✘ |
| Suma ingenua sin reutilizar buffers | 3,5 MiB | — |
| Tensor vivo mayor (`cat9`, 32×128×128) | **512 KiB** | ✘ |
| Skips del encoder vivos a la vez | 448 KiB | ✘ |
| Comparación: denoiser de fábrica (`model-cmlp.bin`) | 429 KB | ya corre hoy |
| Comparación: RNNoise v0.1.1 int8 | 85,5 KiB (0,08 MiB) | cabe en SM |
| RAM libre del AP | ~52 KB | ✘ |

El problema no es "va justo": **un solo tensor de la primera convolución (256 KiB int8) es la mitad de la SRAM del motor**, y el planificador de `tpacker` exige que las entradas y salidas de `Conv2dInt` estén en SM (`npu-rnnoise/docs/ops_desc.md`, §Conv2dInt/Conv1dInt: "输入输出必须在 SM"). Para que entrara habría que trocear el modelo en anchos de banda o en tiempo, que es exactamente lo que hace inviable la latencia.

### 2.4 Latencia

```
bloque de análisis           1 008 ms   (necesario: ref=max no es causal)
inferencia en el NPU         ~60-85 ms
ISTFT + solape                ~32 ms
------------------------------------------------
total ≈ 1,10 s de retardo, constante, en el camino de audio
```
Comparación: RNNoise procesa **tramas de 10 ms** con ~10–20 ms de retardo. La cadena DMR de la C62 (vocoder AMBE) trabaja con tramas de 20 ms: un denoiser de 1,1 s de retardo no es usable ni en recepción (el operador oye 1 s tarde) ni, obviamente, en TX.

### 2.5 Encaje de operadores en Thinker (VENUS)

| Op de la UNet | Thinker/VENUS | Comentario |
|---|---|---|
| `Conv2D` 3×3 y 1×1 | `Conv2dInt` ✔ | kernel ≤ 5, pad ≤ 4, batch 1, stride 1 ✔ |
| `MaxPooling2D` 2×2 | `MaxPool` ✔ | |
| `concatenate` | `iqCat` ✔ | |
| `tanh` | `iqTanh` ✔ | |
| `LeakyReLU(0.3)` | **✘** | no está en la lista; se sustituye por `PReLU` (soportado) con pendiente fija 0,3 — **exacto** si se transfiere el peso |
| `UpSampling2D` + `Conv2D(k=2)` | **✘** (`Resize`/`Upsample` no está) | dos salidas: (a) `ConvTranspose2d` k=2 s=2 — soportado, pero **no equivalente** a up+conv (el up+conv mezcla 4 vecinos, el deconv asigna 1 tap) ⇒ requiere reentrenar; (b) trocear en `iqPad`+`iqSlice` manual |
| `Dropout` | n/a | no-op en inferencia, se elimina |
| Formato | **NCHW, batch 1, int8 puro** | la red es TF/Keras ⇒ **hay que reescribirla en PyTorch**: `linger` solo admite PyTorch |

Conclusión de operadores: **no hay bloqueo conceptual**, pero la conversión no es "exportar y empaquetar": hay que (1) reimplementar en PyTorch, (2) cambiar la salida `tanh` (que mapea 164 dB en [-1,1]) por una máscara `sigmoid` acotada, mucho mejor condicionada para int8, (3) reentrenar por el cambio de upsampling, (4) QAT con `linger` (int8 puro en Venus, sin fp16). Es un trabajo de **2–3 días** más la vuelta de tuerca de calibración.

---

## 3. PoC: qué se puede medir y qué no

### 3.1 Pesos

El **fork no trae pesos**; el **upstream sí** (`weights/model_unet.h5`, MIT). Se han usado esos: es el modelo que el README del fork dice haber entrenado (la curva de `training.png` es la del upstream). Con ellos el PoC es real: mis números no son de un modelo aleatorio.

Caveat importante: son pesos entrenados con **audiolibros en inglés a 8 kHz** y ruidos ESC-50. No hay nada en español ni ruido de radio.

### 3.2 Montaje

`scripts/unet_infer.py`: reimplementación numpy del grafo (validada, §1.4) + pipeline completo (bloques de 8064 → STFT → dB → red → resta → ISTFT) sobre **`opendmr-port/audio/osr.wav`**, 33,3 s de voz real a 8 kHz del árbol de OpenDMR (misma tasa y banda que la C62: 0–4 kHz). Ruidos **sintéticos** con la forma espectral del caso (hiss de FM estrecha 250–3200 Hz, motor 47 Hz con AM, viento paso bajo modulado) a 0/5/10/20 dB de SNR. Se comparan: ruidoso, UNet y **RNNoise v0.1.1** (`rnnoise_demo` del árbol de RNNoise, camino 8k→48k→8k con `resample_poly`, cuyo *null test* da 34,7 dB de SNR).

Métricas: SNR, SNR con ganancia óptima (SNR*, para separar error de nivel de distorsión), segSNR 20 ms, **PESQ-NB** (`pesq` 8 kHz) y **STOI**. POLQA no es libre; PESQ-NB es el estándar equivalente a 8 kHz.

### 3.3 Resultados (33 bloques, `work/poc/sweep.txt`)

| ruido | SNR in | proceso | SNR | SNR* | PESQ | STOI | segSNR |
|---|---|---|---|---|---|---|---|
| hiss | 0 dB | ruidoso | 0,0 | 3,0 | 1,44 | 0,460 | −5,1 |
| | | **UNet** | 1,1 | 7,4 | **1,96** | 0,508 | 1,0 |
| | | RNNoise | **7,2** | **8,9** | 1,84 | **0,545** | **2,9** |
| hiss | 10 dB | ruidoso | 10,0 | 10,4 | 1,74 | 0,593 | 1,6 |
| | | UNet | 1,2 | 8,8 | 2,49 | 0,610 | 0,9 |
| | | RNNoise | **6,0** | **12,2** | 2,34 | **0,646** | **2,6** |
| hiss | 20 dB | ruidoso | 20,0 | 20,0 | 2,34 | **0,724** | 11,4 |
| | | UNet | 1,2 | 9,4 | 3,02 | 0,690 | 0,8 |
| | | RNNoise | 6,0 | 13,2 | 3,03 | 0,734 | 3,0 |
| motor | 20 dB | ruidoso | 20,0 | 20,0 | 3,51 | 0,783 | 11,8 |
| | | UNet | 1,2 | 9,5 | 3,22 | 0,711 | 0,8 |
| | | RNNoise | 5,8 | **13,3** | **3,63** | **0,777** | **3,0** |
| viento | 10 dB | ruidoso | 10,0 | 10,4 | 2,47 | 0,666 | 2,0 |
| | | UNet | 1,1 | 8,9 | 2,84 | 0,683 | 0,6 |
| | | RNNoise | **6,4** | **11,7** | 2,87 | **0,706** | **2,8** |

Tabla completa (12 condiciones) en `work/poc/sweep.txt`. Resumen:

* **RNNoise gana SNR, SNR\*, segSNR y STOI en las 12 condiciones** (una, viento a 0 dB, por 0,1 dB). En PESQ la UNet gana 5 de 12 —hiss y viento a 0–10 dB, p. ej. 1,96 vs 1,84 a 0 dB— y pierde las cuatro de 20 dB (3,22 vs 3,63 en motor).
* **La UNet nunca es transparente**: STOI a 20 dB baja de 0,724 (ruidoso) a 0,690. RNNoise lo sube a 0,734.
* A 0 dB de SNR de entrada, la UNet saca 7,4 dB con ganancia corregida y RNNoise 8,9 dB (el ruidoso, con su ganancia óptima, ya da 3,0 dB). Es decir: la UNet mejora +4,4 dB sobre el ruidoso, RNNoise +5,9 dB — y RNNoise hace eso con 96× menos MACs.

### 3.4 Tres defectos medidos, no opinados

**(a) Atenuación fija de −17 dB.** La ganancia óptima UNet→limpio es **g = 6,66–7,47 en las 12 condiciones** (y estable dentro de cada una, 33 bloques de 1 s). El modelo "estima ruido" siempre de más, también sobre voz limpia: pasando **voz limpia** por la red, la salida queda a **SNR 1,2 dB y 0,136 de amplitud** (−17 dB). El `*10` del repo es exactamente ese parche. Un denoiser que necesita un factor 6,9 fijo no es un denoiser: es un compresor con forma espectral.

**(b) No reconoce cualquier voz.** Sobre `voice_shihao_8k.wav` (voz masculina, f0 ≈ 151 Hz) los **11 bloques** colapsan: la red predice **+42…+51 dB** en los bins de voz y la borra. Sobre `osr.wav` (f0 ≈ 211 Hz) funcionan 26/33 bloques y 7 colapsan a +38…+47 dB. Es el comportamiento típico de un modelo de 2019 entrenado con 10 épocas sobre audiolibros ingleses: se sabe la distribución y nada más. Con voz española y ruido de radio no hay ninguna garantía.

**(c) Sensibilidad al error del estimador** (`scripts/pipeline_audit.py`, techo del algoritmo = magnitud perfecta + fase del ruidoso):

| ruido | SNR in | techo (ΔSNR) | error −3 dB | error +3 dB | error +6 dB |
|---|---|---|---|---|---|
| hiss | 0 dB | **+15,2 dB** | +6,7 | −5,0 | −9,5 |
| hiss | 10 dB | +10,8 dB | −13,1 | −10,1 | −14,8 |
| motor | 10 dB | +6,9 dB | −5,5 | −5,6 | −11,1 |
| viento | 10 dB | +5,7 dB | −5,6 | −5,9 | −11,2 |

Es decir: el techo del algoritmo es +15 dB a 0 dB de SNR (hiss) y **+5…+7 dB con ruido correlado** (motor, viento); y **3 dB de error constante cuestan 5–13 dB de mejora**. Para un modelo que debe dar la resta en dB con precisión de ~1 dB, esto fija el presupuesto de cuantización: la salida `tanh` mapea **164 dB en 256 niveles de int8 = 0,64 dB/LSB**, y la entrada **96 dB en 256 = 0,375 dB/LSB**. Ajustado pero viable **solo con QAT**; con PTQ se pierde de golpe parte del margen.

### 3.5 Lo que no se ha podido medir

* **PESQ/POLQA de un modelo entrenado para radio**: no existe. Los pesos que hay son de audiolibros en inglés; no hay pesos españoles, ni de ruido de radio, ni de AMBE. Entrenarlo está valorado en §5.
* **El modelo en el NPU**: no hay toolchain (`linger`/`tpacker`) en este host ni pesos en PyTorch. El veredicto de memoria de §2.3 es aritmético (tamaño de tensores vs SRAM), no una medida de `tpacker`.

---

## 4. Comparación directa con RNNoise

| | **UNet (este repo)** | **RNNoise v0.1.1** |
|---|---|---|
| Entrada | STFT 128 bins × **1,008 s**, 8 kHz | 42 features, **10 ms**, 48 kHz |
| Salida | mapa de ruido en dB (sin límites) | 22 ganancias (sigmoide, ∈[0,1]) + VAD |
| Parámetros | 1 941 093 | 87 503 |
| MAC/s | **833 MMAC/s** | **8,7 MMAC/s** (red) · ~46 MMAC/s con FFT+pitch |
| Peso int8 | **1,85 MiB** | **85,5 KiB** |
| Activaciones | **0,9–1,0 MiB** | ~2,6 KiB + workspace ≤32 KiB |
| Latencia | **~1,1 s** | ~10–20 ms |
| M33 | 284 % (pico) | 2,9 % (red) |
| HiFi4 | 139 % @50 % ef. | ~4–8 % (medido por el otro subagente) |
| NPU | tiempo 6–8 %, **memoria ✘** | **cabe** (88 KiB de 96) |
| Licencia | MIT, **con el copyright upstream borrado** | BSD-3, limpia |
| Pesos | sí, upstream, inglés 8 kHz | sí, en el repo, + conversión int8 de Arm |
| Formato de entrenamiento | TF/Keras (hay que portar a PyTorch) | C con pesos en tablas / TFLite int8 de Arm |
| Calidad medida (mismo arnés, 33 bloques) | peor en SNR/segSNR/STOI (0/12); mejor en PESQ en 5/12 | **mejor** en SNR/segSNR/STOI (12/12) |
| Esfuerzo de integración | reescribir en PyTorch + QAT + rediseño de upsampling + memoria | reexpresar el grafo (ya hecho por Arm) + QAT |

**La UNet no compra nada**: 96× los MACs de la red, 22× la memoria, 50–100× la latencia, para perder en SNR/segSNR/STOI en las 12 condiciones (PESQ solo lo gana en 5, todas a 0–10 dB). Y no cabe.

---

## 5. Si se quisiera entrenar de verdad: coste

| Concepto | Estimación | Base |
|---|---|---|
| Datos limpios | 100 h de voz española (Common Voice ES, CC0) + 5–10 h de ruidos de radio **grabados en la C62** (hiss, motor, viento, squelch) | los ruidos de radio no están en ningún dataset público; ESC-50/DEMAND son un proxy malo |
| Cómputo por época | 1,72 GFLOP fwd/parche ⇒ ~5,2 GFLOP fwd+bwd; 100 h = 360 k parches ⇒ **1,87 PFLOP/época** | 858 MMAC medidos |
| Tiempo | **~1–3 h de GPU por entrenamiento de 100 épocas** en una RTX 4090/A100 (30 % MFU) | derivado |
| Preparación de datos | STFT + mezcla aleatoria de 360 k parches: **1–2 h de CPU** vectorizado (el `prepare_data.py` del repo es un bucle Python: días) | código actual |
| QAT int8 + toolchain | 2–3 días de ingeniería (port a PyTorch, `linger`, `tpacker`, calibración) | §2.5 |
| **Total realista** | **1–2 semanas de una persona + ~10 h de GPU por iteración** | |

Y aun así el resultado seguiría sin caber (§2.3) y con 1 s de latencia (§2.4). **Entrenar no arregla el problema**: el problema es la forma del modelo.

---

## 6. Qué habría que cambiar para que quepa

`scripts/unet_variants.py` recalcula parámetros, MACs, tasa y pico de activaciones para variantes:

| variante | params | MAC/parche | tasa | act. pico int8 | % de 640 KiB | ventana |
|---|---|---|---|---|---|---|
| **original C16 128×128** | 1 941 093 | 858,6 M | **832 MMAC/s** | 896 KiB | **140 %** | 1 032 ms |
| C16 128×32 | 1 941 093 | 214,6 M | 778 MMAC/s | 224 KiB | 35 % | 276 ms |
| C8 128×128 | 485 813 | 216,4 M | 210 MMAC/s | 448 KiB | 70 % | 1 032 ms |
| C8 128×32 | 485 813 | 54,1 M | 196 MMAC/s | 112 KiB | 18 % | 276 ms |
| C8 64×32 (bins 0–2 kHz) | 485 813 | 27,1 M | 98 MMAC/s | 56 KiB | 9 % | 276 ms |
| C8 96×16 | 485 813 | 20,3 M | 135 MMAC/s | 42 KiB | 7 % | 150 ms |
| C6 64×16 | 273 473 | 7,7 M | 51 MMAC/s | 21 KiB | 3 % | 150 ms |
| C4 64×16 | 121 725 | 3,4 M | 23 MMAC/s | 14 KiB | 2 % | 150 ms |

Dos conclusiones que salen de la tabla y no del gusto:

1. **La tasa de MAC no baja recortando tiempo**: pasar la ventana de 128 a 32 tramas baja memoria y latencia 4× pero **deja 778 MMAC/s** (la misma cuenta por segundo). La tasa solo baja con **canales** (C²: 832 → 210 al pasar de 16 a 8) y con **bins de frecuencia** (128 → 96: 210 → 135).
2. **Para caber hay que irse a C4–C8 y ≤64 bins**: a C8/96×16 queda en 135 MMAC/s (11 % del HiFi4 pico, 35 % con eficiencia del 50 %) con 150 ms de ventana. Eso ya **no es este modelo**: es una CNN de espectrograma pequeña, que es *exactamente lo que el chip ya trae de fábrica* (`model-cmlp.bin`, 429 KB, `anker_unet`/`mtfaa`) y lo que RNNoise hace con 20× menos.

Cambios mínimos si se insistiera:

1. **Salida como máscara acotada** (`sigmoid` × `noisy`, piso −12 dB) en vez de resta libre en dB: mejor condicionada para int8, evita el colapso de armónicos y hace innecesario el `*10`. El techo medido con suelos de −20/−12/−6 dB es el mismo (§3.4), así que la ganancia esperada es de robustez frente al error del estimador, no de techo.
2. **`LeakyReLU` → `PReLU`** y **`UpSampling2D+Conv` → `ConvTranspose2d`** (requiere reentrenar).
3. **Ventana de 16–32 tramas** (125–250 ms) en vez de 128; y normalización por **media móvil** en lugar de `ref=max` del parche, para que sea causal.
4. **C = 4–8**, 5 niveles → 3–4 niveles, bins 64 (0–2 kHz) o 96.
5. **QAT con `linger`**, int8 puro; entrada con escala fija (0,375 dB/LSB) y salida en máscara.

---

## 7. Recomendación

**Descartar esta UNet para la C62.** No es un problema de afinado: 858 MMAC por segundo de audio es 69 % del pico de MACs del HiFi4 y 150× el coste del vocoder AMBE en el M33; en el NPU el tiempo sobra pero las activaciones (0,9–1,0 MiB) no caben en la SRAM del motor, y la latencia de 1,03 s no es negociable sin romper el propio preprocesado (`ref=max`).

**Quedarse con RNNoise v0.1.1** (o con la CNN de fábrica) para el camino de voz:

* **Precisión**: medido en el mismo arnés con voz real de 8 kHz, RNNoise gana en SNR, segSNR y STOI en las 12 condiciones. La UNet solo gana PESQ en 5 de 12 (hiss y viento a 0–10 dB, donde el ruido es más molesto que la distorsión) y además **no es transparente** (baja el STOI de 0,724 a 0,690 a 20 dB) y **no reconoce voces fuera de su distribución** (colapsa en los 11 bloques de una voz masculina de 151 Hz).
* **Latencia**: 10–20 ms frente a 1 100 ms.
* **Memoria**: 85,5 KiB frente a 1,85 MiB de pesos + 1 MiB de activaciones.
* **Esfuerzo**: RNNoise cabe entero en el HiFi4 al 4–8 % (informe del otro subagente) y su conversión a int8 ya existe (Arm ML-zoo, Apache-2.0); la UNet exige port a PyTorch, QAT, cambio de upsampling y rediseño de memoria.
* **Licencia**: BSD-3 limpia frente a una MIT **con el aviso de copyright del upstream eliminado** — irrelevante técnicamente, pero es un repo de terceros sin historial (un solo commit) en el que no conviene basar firmware.

**Lo único aprovechable del repo** es su pipeline de datos (STFT 255/63, mezcla voz+ruido, escalados) como receta para **entrenar una CNN pequeña propia** (C4–C8, ventana ≤250 ms, salida en máscara) si algún día se quiere batir a RNNoise en el NPU. Ese trabajo sí cabe en el chip: 23–51 MMAC/s, 14–21 KiB de activaciones, 0,12–0,46 MB de pesos.

---

## 8. Reproducibilidad

Todo en la rama `c62/nr-unet`, sin tocar otras ramas ni servidores:

| Fichero | Qué hace |
|---|---|
| `scripts/model_cost.py` | params/MACs/MB por capa desde la arquitectura literal |
| `scripts/unet_np_forward.py` | grafo numpy + planificador de memoria greedy + tiempo en host (`--time`: 58–65 ms/parche a 1 hilo según carga del host, ~27–30 GFLOP/s efectivos) |
| `scripts/unet_variants.py` | tabla de variantes (params, MACs, tasa, activaciones, ventana) |
| `scripts/pipeline_audit.py` | geometría del STFT, techo del algoritmo, sensibilidad a errores del estimador |
| `scripts/unet_infer.py` | inferencia real con los pesos del upstream, comparación UNet vs RNNoise (PESQ/STOI/SNR/segSNR), WAVs en `work/poc/` |
| `work/poc/sweep.txt` | salida completa del barrido (12 condiciones × 3 sistemas) |
| `vendor/unet-denoiser/` | clon del fork (sin pesos), ignorado por git |
| `vendor/upstream_weights/` | `model_unet.h5` + `.json` del upstream real, ignorado por git |

Entorno: `python3 -m venv .venv && .venv/bin/pip install h5py scipy pesq pystoi` (Python 3.14). Timing de host en un x86 con OpenBLAS a 1 hilo, no extrapolable al SoC salvo como orden de magnitud.
