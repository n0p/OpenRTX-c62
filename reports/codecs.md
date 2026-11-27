<!--
SPDX-FileCopyrightText: 2026 OpenRTX Contributors

SPDX-License-Identifier: GPL-3.0-or-later
-->

# Códecs del C62: qué ofrece el DSP y qué conviene cambiar

Fecha: 2026-11-27 · Rama `codec-eval` (desde `master`) · Árbol:
`/home/n0p/src/c62/work/OpenRTX-integ` · Sin radio, sin `/dev/ttyUSB*`, sin
compilar firmware (todas las medidas son en el host, en QEMU, o en el mapa del
último `build_c62`).

Este informe responde a cuatro preguntas: qué ofrece el SDK del DSP que el
firmware no usa, qué cuesta hoy cada códec, qué se ganaría moviéndolo al HiFi4
o cambiando el algoritmo, y qué no merece la pena.

Convenio de cifras. **medido** = contado con el plugin de QEMU sobre
`mps2-an505` (M33, FPv5-SP, sin modelo de caché) o leído de un mapa/tabla del
repositorio con su fuente citada. **estimado** = lo que no se ha podido medir y
se dice por qué. La conversión instrucciones→ms usa CPI 1,2–1,6, el mismo
intervalo que `docs/DMR_VOCODER.md`; el IPC real de este SoC no está medido con
contador de ciclos. Ciclos: no se cuentan, se derivan (ciclos = instrucciones ×
CPI); por ejemplo el decodificador AMBE+2 son 136 044–181 392 ciclos por trama
de 20 ms.

---

## 0. Resumen

| # | Cambio | Beneficio | Riesgo | Criterio de aceptación |
|---|---|---|---|---|
| 1 | Filtro polifásico 6:1 y 1:6 en `csk6_codec` | los ocho modos de voz de 8 kHz dejan de aliasear el micro y de repetir muestras al altavoz | bajo | 63 k instrucciones/bloque de 10 ms (medido); alias ≥ 60 dB; imágenes ≥ 40 dB |
| 2 | Scratch del vocoder AMBE+2 (4 056 B) fuera de DTCM | estado+pila de voz en DTCM: 95,9 % → 71,1 %; hace posible decodificar los dos slots | bajo-medio | `.dtcm_bss` ≤ 12 000 B (hoy 16 020); 1 000 tramas bit-exactas; instrucciones/trama ≤ 115 000 |
| 3 | Codec2 con `-O2` y pila de 8 KiB en vez de 16 | −2,2 % (deco) y −4,1 % (codi) de instrucciones; 8 KiB de heap | bajo | instrucciones/trama dec ≤ 935 k, enc ≤ 725 k; high-water de pila ≤ 6 KiB en radio |
| 4 | RNNoise: quitar la aritmética `double` del M33 | hasta −21 % de instrucciones (es lo que hoy va a libgcc) | bajo | instrucciones/trama ≤ 1,45 M; PESQ/STOI ±0,02 del actual |
| 5 | RNNoise en el HiFi4 con `ns_service` | 66–73 % del M33 → 4–8 % del DSP (estimado), NS encendible en producción | alto | 7 146 tramas bit-exactas contra la referencia; carga DSP ≤ 25 % escalar; latencia ≤ 30 ms |
| 6 | Codificador AMBE+2 de DMR en el HiFi4 | libera 28–38 % del M33 durante la emisión | alto | instrucciones/trama ≤ 1,5 M en QEMU HiFi4; 1 000 tramas idénticas a `lib/dmr_encoder` |
| 7 | Codificadores ausentes (IMBE de P25, AMBE 3600×2400 de D-STAR) | da voz a dos modos que hoy emiten silencio | medio (sin banco) | una radio real decodifica la llamada |
| 8 | Reproducción de 8 kHz por el mezclador del DSP (resampler lineal) | 0 ciclos de M33 para interpolar | medio (rediseño del transporte) | imágenes ≥ 25 dB; latencia ≤ +10 ms; sin regresión en FM |

Lo que **no** merece la pena está en §4: mover los decodificadores baratos al
DSP, Codec2 al HiFi4, WASM para códecs, la UNet, el NPU para RNNoise y
reemplazar el codificador AMBE+2.

---

## 1. Qué ofrece el SDK del DSP que hoy no se usa

### 1.1 Qué imagen se flashea

Dos imágenes distintas viven en el árbol y conviene no confundirlas:

| Imagen | Bytes | Dónde | Tasa |
|---|---:|---|---|
| A, la que usa el firmware | **466 832** | `build_c62/zephyr/dsp_firmware.bin`, sha256 `9579b617…` | 48 kHz |
| C, la que trae el SDK | 354 824 | `forks/lsf-zephyr-sdk/resources/dsp_firmware.bin` | 16 kHz |

Todo lo que sigue está comprobado sobre **A**, que es la que se flashea en
`0x100000` (partición de 1 MiB: 581 616 B libres). A y C son la misma familia
LSF (AudioFlinger + LSF/URPC), pero A es la variante de 48 kHz que el fork fijó
por hash (`INTEGRATION.md`).

### 1.2 La API que el AP tiene delante

Lo que el cliente (`modules/lib/{af,lsf,urpc}`) expone, con el uso que el
firmware le da hoy:

| API | Qué hace | Uso actual en el C62 |
|---|---|---|
| `AudioRecord_ctor/start/read/stop` | captura, 48 kHz estéreo, bloques de 10 ms | sí (mic + canal de radio) |
| `AudioTrack_ctor/start/write/obtainBuffer` | reproducción, 48 kHz estéreo | sí (altavoz + entrada de micro del BK4819) |
| `AudioSystem_setParameters(ioHandle, "K=V;…")` | 12 claves de ganancia: `ADC_PDM_GAIN_{A,D}_{LEFT,RIGHT}`, las mismas `_AUX_`, y `DAC_GAIN_{A,D}_{LEFT,RIGHT}` | **4** de las 12 (solo ADC principal) |
| `AudioSystem_setMasterVolume` / `AudioTrack_setVolume(l,r)` / `_mute` | volumen digital fraccionario, por canal | no |
| `lsf_vad_enable()` (`services/vad.h`) | VAD por umbral (`amp-l`, `amp-h`, `minlen`), amarra el *wake* del PMU | no |
| `lsf_thinker_set_model/set_input/get_output` | inferencia en el NPU: modelo int8 + tensor de entrada → tensor de salida | no |
| `lsf_wasm_load_app(ptr,size)` | carga una app WASM (contenedor `LSHD`) en el DSP | no |

Esto está en los binarios de A: `VadService`, `ThinkerService`, `WasmService`
aparecen en sus cadenas, y las 12 claves de ganancia son las únicas cadenas
con ese formato (`MAYÚSCULAS_CON_GUIONES`) que aparecen en la imagen
(`strings` de A cruzado con `AudioSystem_setParameters`).

### 1.3 Lo que la imagen **no** ofrece: FFT, filtros, AEC, EQ

No hay API de proceso. Comprobado por dos vías:

* en las cabeceras del cliente no existe ninguna función de FFT, FIR, IIR,
  biquad, remuestreo, AEC, AGC ni EQ; solo streams, volumen, VAD, Thinker y
  WASM;
* en las cadenas de la imagen A no aparece ningún símbolo `fft`, `fftr`,
  `fir`, `iir`, `biquad`, `aec`, `agc` ni `kiss_fft`. Lo que sí aparece es
  `AudioResampler: Create linear Resampler` (interpolación lineal, solo cuando
  la pista pide una tasa distinta de la del dispositivo), `apc_eq_set_coef_array`
  / `apc_eq_enable` (EQ del códec APC *hardware*, sin clave de parámetro que lo
  alcance) y `LunaFftParams_t` (FFT **dentro** del runtime del NPU, no invocable
  desde el AP).

Es decir: **el SDK no expone cómputo**. La imagen A es un puente PCM con códec
hardware (PDM2PCM/DAC), ganancias, VAD por umbral, NPU y WAMR.

La imagen de fábrica sí traía proceso —CAE de iFlytek (NS+AGC+VAD), EQ,
remuestreador con filtro, denoise neuronal `anker_unet` sobre el NPU, todo a
16 kHz— pero no es interoperable con el cliente LSF (`reports/dsp_factory.md`
§2, §3) y no está en el árbol.

### 1.4 Aceleradores: qué hay detrás de la puerta

| Recurso | Dato | Fuente |
|---|---|---|
| HiFi4 | 300 MHz, 4 MAC/ciclo = 1,2 GMAC/s de pico; a 48 kHz, **6 250 ciclos/muestra**; 6,0 M ciclos por trama de 20 ms | `reports/dsp_factory.md` §6; `reports/nr_unet.md` §2 |
| C escalar en el HiFi4 | **8,0 instrucciones/MAC** con `-O2` y 6,5 con `-O3 -funroll-loops` (producto int8×Q15 de 32 768 taps) | medido, `docs/NOISE.md` §"The HiFi4 route" |
| NPU (Thinker/VENUS) | ≈14,6 GMAC/s (MobileNetV2 de 300 MMAC en 20,5 ms, medido por el vendor); soporta `Conv2dInt`, `LinearInt`, **GRUInt** y **LSTMInt**; 96 KiB de memoria del motor, ~640 KiB de SRAM visible | `reports/npu_rnnoise.md` §1, `reports/nr_unet.md` §2.3 |
| WAMR | intérprete WASM con WASI (`wasi_snapshot_preview1`), heap de app, sin acceso a las instrucciones `ae_*` del HiFi4 | cadenas de la imagen A |
| Memoria del CP | arranca en `0x60000000` (PSRAM del CP); banco compartido con el AP en `0x200C0000` (64 KiB, `ICStreamShare` de 42 112 B en la imagen de 48 kHz); partición de firmware de 1 MiB | `reports/dsp_firmware.md` §"Formato" |

### 1.5 Cómo se ejecutaría código propio en el DSP

Tres puertas, y solo una es barata:

| Vía | Qué permite | Coste / límite |
|---|---|---|
| WASM (`lsf_wasm_load_app`) | cargar código sin tocar la imagen; WAMR interpreta | intérprete, sin SIMD del HiFi4, sin control del FPU; un códec en WASM sería más lento que en el M33. No sirve |
| Imagen propia (`work/dsp-openfw`) | servidor LSF/URPC propio, servicios nuevos, acceso directo al HiFi4 | 4–6 semanas según `reports/dsp_firmware.md`; el contrato de cable ya está documentado y el servidor mínimo existe; el registro del core `venus_hifi4` y un `xt-clang` que arranca están en esta máquina (`~/xtensa`, ver la skill `xtensa-hifi4-csk6`), con la licencia de Cadence como asunto aparte |
| Parchear el blob | nada práctico | sin fuente del CP; solo datos, no código |

Consecuencia para todo el informe: **cualquier** propuesta de "llevarlo al
HiFi4" arrastra una imagen DSP propia. Por eso solo se propone donde el M33 no
llega (§3.5, §3.6).

---

## 2. Los códecs, uno por uno

### 2.1 AMBE+2 de DMR, decodificador (`lib/dmr_vocoder`)

* Implementación: reescritura en C del decodificador de mbelib-neo, una
  instancia estática de **8 540 B** (`sizeof`, medido), tablas `const` en flash,
  sin heap ni libm. La usan DMR, NXDN y dPMR; YSF entra por
  `dmr_vocoder_decode_ambe_params`, que salta la FEC porque ese modo protege
  los 49 bits con su propia repetición y blanqueo.
* Coste **medido**: **113 370 instrucciones/trama de 20 ms** = 0,45–0,60 ms =
  **2,3–3,0 % del M33**; flash +38 680 B; pila 1 592 B; estado en DTCM.
* Calidad: bit-exacto contra el decodificador de OpenDMR (803/1 000 tramas
  idénticas, el resto 1 LSB, SNR 106 dB); **no validado en el aire** (faltan
  tramas de repetidora real, `docs/DMR_VOCODER.md`).
* Riesgo: el estado vive en DTCM con la pila del hilo de voz (7 168 B): 15 708 B
  de 16 384 (**95,9 %**). El `.dtcm_bss` completo mide **16 020 B (97,8 %)** en
  el build de TX; el resto son otros objetos `DMR_FAST_BSS` del modo
  (`dmr_voice.c`, `DmrSpeech.c`). Es el recurso crítico del firmware.
* Al HiFi4: no. 2,3–3,0 % del M33 no paga una imagen DSP propia, y el códec es
  en coma flotante: el modelo QEMU del HiFi4 no implementa el FPU y el GCC del
  overlay no genera FP (`docs/NOISE.md`), así que ni se podría medir.
* Cambiando el algoritmo: el techo está en el propio modelo; no hay margen de
  calidad sin cambiar de estándar.

### 2.2 AMBE+2 de DMR, codificador (`lib/dmr_encoder`)

* Implementación: C99 de coma fija (análisis IMBE de OP25 portado), bit-exacto
  contra la referencia, `log2f` una vez por armónico y trama.
* Coste **medido**: **1 427 272 instrucciones/trama** = 5,7–7,6 ms =
  **28–38 % del M33**; flash 54 808 B; `.bss` 4 354 B.
* Calidad: hereda la del codificador de OP25, y es el punto débil de la cadena
  de emisión: correlación de f0 **0,085** y acierto de octava ±10 % en el
  43,8 % de las tramas, sobre voz (`reports/opendmr_port.md` §2). Inteligible y
  áspero. No es un problema de ciclos.
* Al HiFi4: es el mejor candidato de *offload* del árbol (coma fija, 1,43 M
  instrucciones frente a 6,0 M ciclos por trama del DSP), pero arrastra imagen
  propia. Ver §3.6.
* Cambiando el algoritmo: reescribir el codificador es un trabajo de meses y
  choca con la patente de DVSI; no se propone.

### 2.3 IMBE de P25 (`lib/imbe_vocoder`)

* Implementación: C de coma flotante con tablas generadas; estado 11,6 KiB en
  PSRAM; sin codificador en el árbol (la emisión va sin voz).
* Coste **medido**: **195 482 instrucciones/trama de 20 ms** = 0,78–1,04 ms =
  **3,9–5,2 %**; flash +28 288 B.
* Calidad: validado contra 261 tramas de una captura real de P25 (canal de voz
  C4FM), `docs/IMBE.md`.
* Al HiFi4: no (3,9–5,2 %, y es flotante).
* Cambiando el algoritmo: el codificador que falta es un paquete de trabajo
  aparte, del orden del de DMR (≈1,4 M instrucciones/trama en M33); los
  candidatos y sus números están en `docs/IMBE.md` §8.

### 2.4 AMBE 3600×2400 de D-STAR (`lib/dstar_vocoder`)

* Implementación: C propio con FEC Golay(24,12) + PN; estado 14 096 B en PSRAM;
  sin codificador (la emisión manda la trama nula).
* Coste **medido**: **138 711 instrucciones/trama** (`-Os`, objetos del
  firmware) = **2,8–3,7 %**; flash +38 636 B; pila 788 B (2 704 B cuando hay
  que repetir el modelo).
* Calidad: parámetros idénticos a mbelib-neo en 20 002 tramas, 9 938 con uno a
  tres errores de canal, 0 bits distintos.
* Al HiFi4: no. Cambiando el algoritmo: el codificador ausente es el mismo caso
  que P25 (mbelib-neo lo tiene en coma flotante).

### 2.5 NXDN, dPMR y YSF

No tienen códec propio: llaman al de §2.1 (NXDN y dPMR, AMBE+2 3600×2450; YSF,
los 49 bits de parámetros directamente). Su coste es el mismo 2,3–3,0 % y su
única diferencia es la FEC, que ya está en el camino de cada protocolo. No hay
nada que optimizar aquí por separado.

### 2.6 ACELP de TETRA (`OpenACELP`)

* Implementación: decodificador de coma fija, 536 B de estado, sin heap; 10 KB
  de código + 9 KB de tablas.
* Coste: el README del proyecto cita **80 k instrucciones/trama de 30 ms en M4**
  (98 k el peor caso). Medido aquí en M33 con el arnés de §5:
  **116 029 instrucciones/trama** (`-O2` = 0,46–0,62 ms = **1,5–2,1 %**) y
  **127 101** (`-Os` = 0,51–0,68 ms = **1,7–2,3 %**). La diferencia con el M4
  es el compilador y el uso de las instrucciones DSP.
* Calidad: bit-exacto contra la referencia ETSI (EN 300 395-4).
* Al HiFi4: no (1,5–2,3 %). Emisión de voz TETRA: bloqueada por el hardware
  (el BK4819 es FM de envolvente constante con PA saturada; la onda TETRA deja
  la señal 37 dB fuera de máscara, `reports/tetra_tx.md`), no por el códec.
  Cambiar el algoritmo no arregla eso.

### 2.7 Codec2 3200 de M17 (`openrtx/src/core/audio_codec.c`)

* Implementación: Codec2 1.x del subproyecto, modo 3200, en coma flotante.
  Hilo propio con **pila de 16 384 B reservada con `malloc`**
  (`CODEC2_THREAD_STKSIZE`), estado del códec en el heap de newlib y
  decimación del micro por media de `CONFIG_MIC_OVERSAMPLE` muestras (en el C62
  no está definido: 1, así que la decimación real es la selección de §2.8).
* Coste **medido aquí** (`scripts/codec_cost.sh`, §5), por trama de 20 ms:

  | build | codifica | decodifica |
  |---|---:|---:|
  | `-O2` | 722 464 instr (14,4–19,3 %) | 930 830 instr (18,6–24,8 %) |
  | `-Os` | 752 120 instr (15,0–20,1 %) | 951 194 instr (19,0–25,4 %) |

  Memoria **medida**: estado 37 452 B (heap, `sbrk` antes/después de
  `codec2_create`); marcos de pila 7 016 B (decodifica) y 1 472 B (codifica)
  con `-fstack-usage`; flash 30 211 B en la imagen actual (mapa de `build_c62`,
  secciones `.text/.rodata/.data` de los objetos `codec2`).
* Calidad: es el Codec2 de referencia; el repositorio no tiene medida de
  calidad de M17 en el aire.
* Al HiFi4: no medible. Codec2 es coma flotante; el modelo QEMU del HiFi4 no
  implementa FP y el GCC del overlay no lo genera (§1.4). Queda como
  estimación no verificable, y por eso no se propone.
* Cambiando el algoritmo: `-O2` por fichero (el firmware compila con `-Os`) ya
  vale −2,2 % en decodificación y −4,1 % en codificación, medido arriba, sin
  tocar código. Es el cambio más barato de todo el informe.

### 2.8 El camino de muestras: 8 kHz ↔ 48 kHz

Todos los códecs de voz entregan y consumen 8 kHz; el transporte del DSP es de
48 kHz estéreo. `csk6_codec.c` convierte **sin filtro**: al capturar se queda
con una de cada `divisor` muestras (línea 693: "keeping every divisor-th
sample") y al reproducir repite cada muestra `divisor` veces. Con `divisor = 6`:

* captura: todo lo que haya entre 4 y 24 kHz se pliega dentro de 0–4 kHz sin
  atenuación. Un tono de 6,5 kHz entra en el codificador de M17/DMR como uno de
  1,5 kHz a 0 dB;
* reproducción: retención de orden cero, primera imagen centrada en 8 kHz y
  atenuada solo por el `sinc` de la retención (−13,5 dB hacia 12 kHz).

Coste de arreglarlo con un polifásico de 48 coeficientes (6 fases × 8), Q15,
medido aquí en M33 por bloque de 10 ms: **24 833** instrucciones el decimador,
**38 009** el interpolador, **63 073** los dos = **2,5 % del M33 a CPI 1,2**
(3,4 % a CPI 1,6). A `-Os` sube a 76 704 (el interpolador, +33 %).

Nota: el decimador del arnés lee con paso 6 sobre un buffer contiguo y sale a
6,5 instrucciones/MAC; una implementación con la historia por fase contigua
baja a ~2 (estimado: ~8 k instrucciones/bloque). El interpolador ya está en
1,65 instrucciones/MAC y es el que manda en el coste.

### 2.9 RNNoise (no es un códec, compite por el mismo presupuesto)

Está para contexto porque es el único proceso de audio que hoy no cabe:

* Coste **medido**: 1,60–1,91 M instrucciones por trama de 10 ms =
  **66,5–73,2 % del M33**; estado 62 312 B en PSRAM; pila 2 856 B; flash
  +103 104 B. Se envía apagado (`docs/NOISE.md`).
* Calidad **medida**: PESQ-WB +0,10 a +0,43 y STOI hasta +0,08 en voz ruidosa;
  no es transparente en voz limpia (PESQ 4,64 → 4,37); 10 ms de latencia.
* El 21 % de esas instrucciones son llamadas `double` de libgcc (biquad,
  `sqrt`, `log10`), y el 38 % es `compute_gru`.

### 2.10 Tabla resumen

| códec | modo | instr/trama | ms | % M33 (CPI 1,2–1,6) | estado | pila | flash | medido en |
|---|---|---:|---:|---:|---|---:|---:|---|
| AMBE+2 dec | DMR/NXDN/dPMR/YSF | 113 370 /20 ms | 0,45–0,60 | 2,3–3,0 | 8 540 B DTCM | 1 592 | +38 680 | `docs/DMR_VOCODER.md` |
| AMBE+2 enc | DMR TX | 1 427 272 /20 ms | 5,7–7,6 | 28–38 | 4 354 B | n.m. | 54 808 | `docs/DMR_TX.md` |
| IMBE dec | P25 | 195 482 /20 ms | 0,78–1,04 | 3,9–5,2 | 11,6 KiB PSRAM | n.m. | +28 288 | `docs/IMBE.md` |
| AMBE 3600×2400 dec | D-STAR | 138 711 /20 ms | 0,55–0,74 | 2,8–3,7 | 14 096 B PSRAM | 788–2 704 | +38 636 | `docs/RX_DSTAR.md` |
| ACELP dec | TETRA | 116 029 /30 ms | 0,46–0,62 | 1,5–2,1 | 536 B | 1 200 | 19 KB | aquí §5 |
| Codec2 3200 dec | M17 | 930 830 /20 ms | 3,7–5,0 | 18,6–24,8 | 37 452 B heap | 7 016 | 30 211 | aquí §5 |
| Codec2 3200 enc | M17 | 722 464 /20 ms | 2,9–3,9 | 14,4–19,3 | ídem | 1 472 | ídem | aquí §5 |
| resampler 6:1+1:6 | todos los de 8 kHz | 63 073 /10 ms | 0,25–0,34 | 2,5–3,4 | 0 | 0 | ≈1 KB (est.) | aquí §5 |
| RNNoise | FM RX | 1,60–1,91 M /10 ms | 6,4–10,2 | 66,5–73,2 | 62 312 B PSRAM | 2 856 | +103 104 | `docs/NOISE.md` |

`n.m.` = no medido en el árbol. Cifras de flash: delta sobre la imagen sin el
códec, salvo Codec2 (30 211 B es lo que el códec ocupa ya en la imagen actual).

---

## 3. Lista ordenada por beneficio/riesgo

Ordenada por (beneficio ÷ riesgo), no por tamaño del beneficio.

### 3.1 Filtro polifásico 6:1 y 1:6 en `csk6_codec` (riesgo bajo)

Beneficio: ataca un defecto que hoy tienen los ocho modos de voz que trabajan a
8 kHz (DMR, NXDN, dPMR, YSF, P25, D-STAR, TETRA y M17). En recepción
quita las imágenes de la retención de orden cero; en transmisión evita que el
codificador de M17 y el de DMR coman ruido plegado de 4–24 kHz, que es
exactamente lo que peor tolera un vocoder de 4 kHz de banda.

Coste medido: 63 073 instrucciones por bloque de 10 ms con ambos sentidos
(2,5 % del M33 a CPI 1,2), 24 833 solo el decimador.

Criterio de aceptación:

* instrucciones por bloque ≤ 70 000 (QEMU, `scripts/codec_cost.sh resample`);
* atenuación de alias ≥ 60 dB: tono de 6 500 Hz a la entrada, medir 1 500 Hz a
  la salida del decimador (hoy: 0 dB);
* atenuación de imágenes ≥ 40 dB: tono de 1 kHz a 8 kHz, medir 7 kHz y 9 kHz
  tras el interpolador (hoy: −13,5 dB en la primera imagen);
* DTCM sin cambios y PESQ del lazo M17 no inferior al actual.

### 3.2 Sacar el scratch del vocoder AMBE+2 de DTCM (riesgo bajo-medio)

El estado de 8 540 B se reparte así (medido, `sizeof`/`offsetof`):

| parte | bytes | uso |
|---|---:|---|
| `cur`, `prev`, `enh` (parámetros) | 3 072 | por trama, predictivo entre tramas |
| `last_uw`, `noise_overlap` (WOLA) | 1 412 | por trama, continuidad entre tramas |
| scratch (`noise`, `uw`, `fft_re/im`, `bin_scale`, `cos_tab`, `a_min/b_max`) | **4 056** | solo dentro de la llamada |

El scratch son 4 056 B (47,5 % de la instancia) que no necesitan cero espera:
se tocan una vez por trama, 50 veces por segundo. En SRAM, los 15 708 B de
estado más pila de voz pasan a 11 652 B (71,1 % de DTCM), lo que además permite
volver a tener dos
instancias (hoy `dmr_voice.c` documenta que dos `vdec_t` de 7,6 KiB no caben y
que por eso solo se decodifica un slot, con una trama corrupta al cambiar).

Riesgo: si la SRAM del CSK6 no es de cero espera, el coste sube; no se puede
medir en QEMU (no modela latencias). Por eso el criterio es doble, en QEMU y en
la radio.

Criterio de aceptación:

* `.dtcm_bss` medido en el mapa ≤ 12 000 B (hoy 16 020 B);
* 1 000 tramas del corpus bit-exactas contra la versión actual;
* instrucciones/trama ≤ 115 000 en QEMU (+1,5 %);
* en la radio, p99 de `s_own_us_total` por trama (`dmr_voice.c` ya lo mide) no
  más de un 10 % por encima del valor actual;
* si se quiere el doble slot: dos instancias enlazan y decodifican 2 × 1 000
  tramas sin error.

### 3.3 Codec2: `-O2` por fichero y pila de 8 KiB (riesgo bajo)

* `-O2` en vez del `-Os` heredado: medido −2,2 % en decodificación (930 830 vs
  951 194) y −4,1 % en codificación (722 464 vs 752 120). El patrón ya existe en
  `lib/dmr_vocoder/CMakeLists.txt` y `lib/imbe_vocoder` (opciones por fuente,
  porque `zephyr_interface` añade `-Os` después).
* Pila: los marcos medidos son 7 016 B (decodifica) y 1 472 B (codifica) con
  una reserva de 16 384 B. Bajarla a 8 192 B libera 8 KiB del heap de newlib.

Criterio de aceptación: instrucciones/trama dec ≤ 935 000 y enc ≤ 725 000
(QEMU); 1 000 tramas de M17 TX y RX sin fallo; high-water de la pila medido en
la radio ≤ 6 KiB (`tools/cps/blackbox.py`, sección de hilos).

### 3.4 RNNoise: quitar los `double` del camino del M33 (riesgo bajo)

El 21 % de las instrucciones de RNNoise son `__aeabi_dadd`, `__muldf3`,
`__divdf3` y `sqrt`/`log10` promocionados, que vienen de `biquad()` (coeficientes
`double` por un cast explícito) y del cálculo de ganancia. Pasarlos a `float`
es una edición local, y el resultado se puede comparar contra la referencia
actual con las mismas métricas.

Criterio de aceptación: instrucciones/trama ≤ 1,45 M (−25 % sobre 1,60–1,91 M);
PESQ-WB y STOI dentro de ±0,02 de la tabla de `docs/NOISE.md`; salida idéntica
en las 7 146 tramas de la prueba de bit-exactitud del parche de `denoise.c`.

No hace que RNNoise quepa en el M33 (seguiría en el 50–60 %), pero abarata la
referencia que el puerto al DSP tiene que igualar.

### 3.5 RNNoise en el HiFi4 con `ns_service` (riesgo alto)

Es el único proceso de audio que **necesita** el DSP: 66,5–73,2 % del M33 hoy,
4–8 % del HiFi4 con intrínsecos `ae_` (estimado; medido en escalar C son 6,5
instrucciones/MAC → ~565 k instrucciones por trama de 10 ms ≈ 19 % del HiFi4).
El contrato de servicio está diseñado en `docs/NOISE.md` (§"Service contract"):
un `lsf_audio` más, un `ICStream` de 480 muestras por sentido, 480 in/480 out
en sitio.

Riesgo: imagen DSP propia (4–6 semanas), reescritura Q15 de `denoise.c`,
`pitch.c` y `kiss_fft.c`, e intrínsecos que hoy solo genera `xt-clang`.

Criterio de aceptación: 7 146 tramas bit-exactas contra la salida del M33;
carga del DSP medida en la radio ≤ 10 % (≤ 25 % en QEMU escalar); latencia
extremo a extremo ≤ 30 ms; PESQ/STOI iguales a la tabla de `docs/NOISE.md`.

### 3.6 Codificador AMBE+2 de DMR en el HiFi4 (riesgo alto)

Único *offload* que paga: 28–38 % del M33 durante la emisión, y el codificador
es de coma fija (`lib/dmr_encoder`, C99), que es lo único que el HiFi4 puede
ejecutar y medir hoy. Presupuesto del DSP: 6,0 M ciclos por trama de 20 ms; el
códec cuesta 1,43 M instrucciones en el M33, así que cabe incluso a 1 IPC sin
contar con los 4 MAC/ciclo. El transporte de datos es trivial (9 bytes por
trama, 50 por segundo, no hace falta `ICStream`).

Riesgo: la misma imagen DSP propia; un servicio nuevo; y la calidad no mejora,
solo se libera el M33 (útil porque el modulador 4FSK y la recepción comparten
hilo).

Criterio de aceptación: instrucciones/trama ≤ 1,5 M en QEMU HiFi4; 1 000
tramas idénticas bit a bit a `lib/dmr_encoder`; en la radio, CPU del M33 durante
TX medida con el *black box* ≤ 15 % de la que hay hoy.

### 3.7 Codificadores que faltan (P25, D-STAR) (riesgo medio, sin banco)

Da voz a dos modos que hoy emiten silencio (la portadora y la señalización ya
están). No es trabajo de DSP: el orden de coste es el del codificador de DMR
(≈1,4 M instrucciones/trama en M33) o mayor. mbelib-neo tiene los dos en coma
flotante. Criterio de aceptación: una radio real del modo decodifica la llamada
con voz inteligible; sin ese banco, el criterio no se puede cumplir y el
trabajo no debe empezar.

### 3.8 Reproducción de 8 kHz por el mezclador del DSP (opción de 3.1)

Si una pista se abre a 8 kHz, `AudioMixer` la remuestrea a 48 kHz con
interpolación lineal (`Create linear Resampler`, §1.3): cuesta 0 ciclos de M33
y mejora las imágenes de −13,5 dB a ~−30 dB. Requiere que el transporte admita
tasas distintas por pista (hoy es un `AudioTrack` estéreo único con el micro en
un canal y la radio en el otro), es decir, rediseñar `csk6_codec`. Se deja como
opción si 3.1 sale caro o si se quiere quitar trabajo al M33.

Criterio de aceptación: imágenes ≥ 25 dB medidas, latencia añadida ≤ 10 ms,
ninguna regresión en la ruta FM (que seguiría a 48 kHz).

---

## 4. Lo que no merece la pena, y por qué

| Idea | Por qué no |
|---|---|
| Mover AMBE+2 dec, IMBE, D-STAR o ACELP al HiFi4 | 1,5–5,2 % del M33 cada uno. El ahorro no paga una imagen DSP propia (4–6 semanas) y dos de ellos son de coma flotante, que el modelo QEMU del HiFi4 no puede ejecutar: no habría ni medida |
| Codec2 al HiFi4 | igual, y además sin poder medir: es coma flotante y el GCC del overlay `venus_hifi4` no genera FP |
| Codec2 en WASM | WAMR interpreta; sin SIMD del HiFi4. Más lento que el M33 |
| Cualquier códec en WASM | ídem; la única puerta barata al DSP no tiene cómputo |
| UNet-audio-denoise (`reports/nr_unet.md`) | 858 MMAC/s = 150× el decodificador AMBE; 69 % del pico del HiFi4; 0,9–1,0 MiB de activaciones frente a los ~640 KiB de SRAM que ve el motor del NPU (el mayor tensor, 512 KiB, no cabe); 1,03 s de latencia estructural |
| RNNoise en el NPU (`reports/npu_rnnoise.md`) | el 80 % de sus MAC son búsqueda de pitch y FFT, que no son operaciones de NPU; con 88 KiB cabe justo y no gana al HiFi4 |
| Reemplazar el codificador AMBE+2 por otro "mejor" | la calidad actual es la del codificador de OP25 (f0 0,085) y cualquier sustituto libre arrastra la patente de DVSI; el trabajo es de meses y el riesgo legal no es de software |
| Optimizar más el decodificador AMBE+2 (113 k, 2,3–3,0 %) | no mueve la aguja: la PHY de DMR cuesta 20,2 M instrucciones por segundo de audio (6,7 %) y el vocoder 5,7 M/s (1,9 % a 1 IPC, 2,3–3,0 % a CPI 1,2–1,6), `docs/DMR_RX.md` §"Cost" |
| EQ o filtros del DSP para el audio de recepción | existen en la imagen (`apc_eq_*`) pero no hay clave de parámetro que los alcance desde el AP; requeriría imagen propia |
| VAD del DSP para DTX | los modos TDMA no pueden dejar el slot vacío: hay que emitir trama de silencio igualmente; en FM el VAD no aporta sobre el squelch |
| AEC / dúplex completo | la radio es semidúplex; no hay referencia de eco que cancelar |
| G.729A o AMR-NB como códecs de modo | no interoperan con ningún estándar que el C62 emita o reciba. (El códec de voz del firmware de fábrica usa el vector de parámetros de AMR-NB MR475, `reports/codec_g729.md`; eso es otro proyecto —hablar con radios de fábrica—, no mejorar los códecs de este firmware) |
| Subir la tasa de los códecs | el aire manda: 4 kHz de banda en todos los modos de 8 kHz |

---

## 5. Cómo se ha medido (reproducible)

Los tres números que no existían en el árbol —Codec2 3200, ACELP en M33 y el
coste del polifásico— salen de:

```sh
scripts/codec_cost.sh O2 all      # o 'Os', o 'codec2|acelp|resample'
```

El script compila cada arnés para el host y para `arm-none-eabi`
(`-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard`), lo enlaza con
`-specs=rdimon.specs` y el `mps2.ld` del probe de `work/dsp-probe`, y ejecuta
QEMU con el plugin contador (`lib/dmr_vocoder/tests/qemu/insn_count.c`). Dos
ejecuciones por arnés, `FRAMES=1` y `FRAMES=21`; la cifra por trama es la
pendiente, así que el arranque y el prólogo del arnés de decodificación se
cancelan.

Antes de creer el número, el resultado del host y del objetivo se comparan por
hash FNV-1a de la última trama: igual para ACELP y el resampler (enteros), y
para Codec2 codificando (bits idénticos); en Codec2 decodificando difiere en
1 LSB (suma −34 121 frente a −34 114 sobre 160 muestras) porque el M33 contrae
en VFMA y el x86-64 no, el mismo efecto que documenta `docs/DMR_VOCODER.md`.

Arnés y límites:

| arnés | qué mide | límite conocido |
|---|---|---|
| `tests/qemu/codec2_cost.c` | Codec2 3200, codifica o decodifica | necesita los dos codebooks `newamp2*` que el cmake de codec2 genera y este árbol no trae: el arnés los define `weak` y vacíos; el modo 3200 no los lee |
| `tests/qemu/acelp_cost.c` | ACELP, una trama de 137 bits | bits pseudoaleatorios con `bfi=0`; el coste del decodificador no depende de los bits |
| `tests/qemu/resample_cost.c` | polifásico 6:1 y 1:6, bloque de 10 ms | coeficientes de ventana de este arnés, no un diseño de filtro final |

Lo demás son cifras del repositorio, citadas en su sitio: `docs/DMR_VOCODER.md`,
`docs/DMR_TX.md`, `docs/IMBE.md`, `docs/RX_DSTAR.md`, `docs/NOISE.md`,
`reports/nr_unet.md`, `reports/npu_rnnoise.md`, `reports/dsp_factory.md`.
Los `sizeof`/`offsetof` del vocoder (8 540 / 4 056 B) y el heap de Codec2
(37 452 B) se han medido en el host con el mismo código y las mismas
estructuras; el estado no depende del compilador más allá del relleno, y el
valor coincide con los 8 540 B que cita `docs/DMR_VOCODER.md`.

---

## 6. Lo que queda abierto

* **Latencia de SRAM frente a DTCM.** La propuesta 3.2 asume que la SRAM del
  CSK6 no es más lenta que el DTCM para el scratch del vocoder. QEMU no modela
  esperas; hay que medirlo en la radio con el contador de ciclos o con
  `s_own_us_total`, que ya existe.
* **FPU del HiFi4 en silicio.** `XCHAL_HAVE_FP=1` en el core, pero ni el modelo
  ni el GCC del overlay lo ejercitan. Mientras eso no cambie, ningún códec de
  coma flotante (Codec2, IMBE, AMBE+2 dec) se puede medir en el DSP.
* **Calidad en el aire.** Todo lo de recepción está validado contra referencias
  de software; nada contra una repetidora real salvo las 261 tramas de IMBE y
  la captura de TETRA.
* **Carga real del DSP hoy.** No hay contadores expuestos por la imagen del CP;
  los 6 250 ciclos/muestra son el techo, no lo que queda libre.
