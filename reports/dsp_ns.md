# RNNoise en el HiFi4 del CSK6011B: base medible

Estado: **el procesado de una trama corre en el núcleo DSP, bit-exacto contra el
M33, y cuesta 671 327 instrucciones por trama de 10 ms** (2,32× menos que el M33
con el mismo arnés).  No está integrado en la imagen ni en el servicio LSF: eso
es lo que falta (§6).

Todo lo de aquí se reproduce con `make` en `dsp/ns/` (rama `dsp/ns`, árbol de
trabajo `work/dsp-ns`), sobre el toolchain Xtensa que ya está instalado en
`~/xtensa`.

---

## 1. Resumen

| | instrucciones/trama | ciclos/trama | % de un núcleo a 300 MHz |
|---|---:|---:|---:|
| Cortex-M33, este arnés, `-ffp-contract=off` | 1 556 943 | — | 51,9 % (instr.) |
| Cortex-M33, build publicado del firmware | 1 360 819 | ~1 620 000 | 45 % / 54 % |
| **HiFi4 (`venus_hifi4`), este port** | **671 327** | **934 801** | **31,2 %** |

* Bit-exactitud: **8/8 tramas, 3 840/3 840 muestras idénticas** contra el build
  del M33 (comparación sobre los bits, no sobre el valor).  También el cuerpo de
  servicio en int16: 3 840/3 840.
* El número de instrucciones del ISS es determinista y es el dato duro; los
  ciclos salen del modelo de pipeline del ISS (sin cachés), así que son una
  estimación ideal.  CPI 1,39.
* El 41,5 % del coste está en las tres capas GRU, con pesos `int8` y entrada
  `float`.  El intento de vectorizarlas a 2 carriles empeora el coste (§4).

---

## 2. Fase 1: desbloquear la compilación para `venus_hifi4`

### 2.1 Lo que faltaba, y dónde estaba

El archivo público `XtensaTools_RI_2021_7_linux.tar.zst` (cdn.iflyos.cn, 567 MB)
trae `xt-clang`, `xt-objcopy`, `xt-run` y `xt-gdb`, pero **no el registro del
core**: `XtensaTools/config/` viene vacío y cualquier invocación muere con
`the "venus_hifi4" core is not in the current Xtensa core registry`.

El registro **no está dentro de ese archivo ni en el firmware de fábrica**: es un
paquete aparte, publicado en el mismo CDN y con el mismo prefijo de ruta, que se
extrae *al lado* de `XtensaTools/`:

```
venus_hifi4_linux-2021.7.tar.zst   (21 MB)
  -> RI-2021.7-linux/venus_hifi4/{config,xtensa-elf,lib,misc,...}
```

`RI-2021.7-linux/venus_hifi4/config/` es exactamente lo que el toolchain busca:
`core.yml`, `core.xparm`, `default-params`, `venus_hifi4-params`, `libisa-core.so`,
`libtie-core.so`, `libcas-core.so` y el `llvm/lib/libXtensaCodeGen.so` que
necesita `xt-clang`.  Los dos tarballs comparten el directorio raíz
`RI-2021.7-linux/` justamente para extraerse uno tras otro.

Además hay que hacer dos cosas más, y las dos se descubren fallando:

1. **`venus_hifi4-params` lleva rutas absolutas de la máquina que lo construyó**
   (`/././home/xpgcust/tree/...`).  Hay que reescribirlas al prefijo local (es lo
   mismo que hace el `Dockerfile` de `listenai/hifi4:2021.7`).
2. **Licencia.**  Con el registro en su sitio, `xt-clang` falla con
   `License checkout failed ... Feature: XTENSA_XCC_TIE` (FlexNet).  Y `xt-run`
   también: `Unable to get license`.  Aquí está resuelto con el stub `tenlp.so`
   de `~/xtensa/lic-shim` (`TENLP_DLL`), que concede el checkout.  **Esto no es
   una licencia**: es un sustituto del cliente FlexNet, y conviene tenerlo claro
   para cualquier uso que no sea este banco.  La vía sin licencia es el GCC de
   `$CORE/src/gcc` (FSF, GPL, ya compilado en `~/xtensa/gcc-10.2.0-venus`), que
   no necesita clave pero **no genera intrínsecos TIE** — sirve para el código
   escalar y para medir, no para escribir kernels `ae_`/`xt_`.

Queda así (`~/xtensa/env.sh`):

```sh
export XTENSA_ROOT=~/xtensa/RI-2021.7-linux
export XTENSA_TOOLS_ROOT=$XTENSA_ROOT/XtensaTools
export XTENSA_CORE=venus_hifi4
export XTENSA_SYSTEM=$XTENSA_ROOT/venus_hifi4/config     # <- el registro
export TENLP_DLL=~/xtensa/lic-shim/tenlp
```

### 2.2 Cómo se demuestra

Compilar y **ejecutar** un programa para el core, con conteo de instrucciones del
ISS, que es lo que valida el port después:

```console
$ xt-clang --version
XtensaTools-14.07 clang version 10.0.1
Target: xtensa-unknown-unknown

$ cat h2.c
#include <stdio.h>
int main(void){ printf("hello venus_hifi4\n"); return 0; }
$ xt-clang -O2 h2.c -o h2.elf -lsim && xt-run h2.elf
hello venus_hifi4
$ xt-run --client_commands="summary" h2.elf
Committed instructions            6615 ( 100.00 )
Cycles: total = 18155
```

Y sobre el código del port, que es el caso real:

```console
$ make cost
# ISS: 4233468 -> 9606141 instructions, 5995381 -> 13469508 cycles (8 measured frames)
instructions per 10 ms frame: 671327
cycles per 10 ms frame:       934801  (CPI 1.392)
at 300 MHz: 31.2 % of the core, 3.12 ms of CPU per 10 ms of audio
```

El truco de la medida: el mismo binario con 0 y con 8 tramas medidas, y se resta.
Los dos builds hacen las mismas tramas de calentamiento, así que la diferencia
es exactamente N tramas, sin instrumentar nada.  El perfil por función sale de
`xt-run --profile` + `xt-gprof` y se resta igual (`tools/cost.py`).

Para emulación sin licencia hay un QEMU 11.1 con el core importado en
`work/xtensa-tools/qs/build/qemu-system-xtensa` (`-M sim -cpu venus_hifi4`);
el port se midió en el ISS porque es el modelo del fabricante y da conteos
deterministas.

---

## 3. Fase 2: el port, y su coste

### 3.1 Qué se porta y qué no

Al DSP va el **cálculo**, no el modelo: las mismas fuentes de `lib/rnnoise`
(`denoise.c`, `rnn.c`, `rnn_data.c`, `pitch.c`, `celt_lpc.c`, `kiss_fft.c`), con
los pesos v0.1.1 (`87 503` `int8`, que son datos, no código).  La interfaz es
`dsp/ns/ns_dsp.h`: 480 muestras `int16` dentro, 480 fuera, en sitio, y un estado
de **62 312 B** que reserva quien llama (no hay heap).

Lo que hubo que resolver para que además sea *idéntico* al M33:

1. **Precisión.**  El HiFi4 sólo tiene coma flotante simple
   (`XCHAL_HAVE_DFP=0`): cada `double` es una llamada a libgcc.  El árbol ya
   tenía el camino de trama en `float`, pero **el clang de RI-2021.7 acepta
   `-fsingle-precision-constant` y lo ignora**, así que `x*1.3` seguía siendo un
   producto en doble: **101 llamadas a los ayudantes de doble por trama**
   (medido en el ELF).  `tools/fp_literals.py` genera las fuentes con los
   literales ya tipados como `float` para el build del DSP; quedan 0.
2. **`log10f`.**  Está en el camino de trama (energías por banda).  El `log10f`
   de la libm de Xtensa **no está correctamente redondeado** (1414/2000 valores
   distintos del correctamente redondeado), mientras que el M33 ejecuta el
   fdlibm de newlib.  Se vendoriza `ef_log10.c` + `ef_log.c` (Sun/fdlibm, licencia
   permisiva) en `third_party/newlib_fdlibm/` y se compilan para el DSP con
   `-Dlog10f=ns_log10f`: reproduce los valores del M33 en 2000/2000 entradas.
   `sqrtf` y `floorf` son exactos en IEEE y no necesitan nada.
3. **Las tablas.**  `half_window`, `dct_table` y los *twiddles* de la FFT son
   `sin`/`cos` en doble redondeados a `float`, y las dos libm **no coinciden en
   el último bit en un tercio de las entradas** (237/480, 388/484, 683/960,
   medido con `make tables`).  Un ulp en un twiddle cambia todas las muestras:
   `ns_tables.h` se genera del volcado del propio build del M33
   (`make ns_tables.h`) y se usa con `-DRNNOISE_FIXED_TABLES`; la reserva y los
   metadatos siguen viniendo de `kiss_fft`.
4. **Orden de las operaciones.**  `rnn.c` suma con un acumulador por neurona y
   recorrido ascendente de entradas; se extrajo a `ns_matvec()` (misma suma,
   mismo orden) para que un kernel DSP lo pueda sustituir sin tocar nada más.  Y
   el término recurrente de la puerta de salida es `w*state[j]*r[j]`, que en C se
   evalúa `(w*state[j])*r[j]`: hay que mantener ese orden, no `w*(state*r)`,
   porque cambia el último bit (éste fue el único desajuste real del port, se
   encontró con `make trace`).

### 3.2 Coste por trama y reparto

| | instrucciones/trama | ciclos/trama (ISS) | a 300 MHz |
|---|---:|---:|---|
| HiFi4, escalar `float` (lo que se entrega) | **671 327** | 934 801 | 31,2 % |
| HiFi4, kernel SIMD de 2 carriles (§4) | 1 030 213 | 1 301 625 | 43,4 % |
| M33, mismo arnés y mismas banderas | 1 556 943 | — | 51,9 % (instr.) |

Reparto por función en el DSP (perfil muestreado, ±2 %):

```
41,5 %  compute_gru            las tres capas GRU: pesos int8 x entrada float
14,5 %  rnnoise_process_frame  biquad, energías por banda, interpolación de ganancia
13,8 %  rnn_fft_impl           FFT compleja de 960 puntos, dos veces por trama
13,4 %  pitch                  xcorr, búsqueda, decimación, eliminación de doblado
 2,2 %  rnn_fft_c
 1,8 %  compute_dense
```

Contra el M33: **2,32× menos instrucciones** con el mismo arnés y las mismas
banderas.  Contra el número publicado del firmware (1 360 819, GCC con
contracción FMA activada) es 2,03×.

Criterio del informe `reports/codecs.md` §3.5: «≤ 25 % en QEMU escalar».  Con
instrucciones se cumple (22,4 %); con los ciclos del modelo del ISS se queda en
31,2 %.  Con la cuenta del propio informe (6,0 M ciclos por trama de 20 ms de
presupuesto del DSP) serían **2,8 M ciclos por 20 ms ≈ 47 % de ese presupuesto**
medido así, y ~19 % si se acepta la estimación de 6,5 instr/MAC del informe para
el escalar.  Es decir: cabe, pero no sobra; el siguiente escalón es §4.

---

## 4. SIMD: medido, y hoy no paga

`make cost SIMD=1` sustituye `ns_matvec()` por `dsp/ns/rnn_dsp.c`: dos neuronas
por instrucción (`MUL.S`/`ADD.S` sobre `xtfloatx2`, nunca fusionados, para no
cambiar el redondeo).  El resultado es **bit-exacto** (8/8 tramas) y **más
lento**: 1 030 213 instrucciones/trama, 1,53× el escalar.

La razón es el formato de los pesos.  Son `int8`, así que cada producto necesita
una conversión int8→float, y esa conversión cuesta más de lo que ahorran los dos
carriles; encima clang escalariza el *splat*/empaquetado hecho con uniones y el
código generado es `mul.s`/`add.s` de un carril con tráfico de memoria extra (se
comprobó en el desensamblado).

La dirección que sí pagaría, y que no está hecha: camino de 16 bits
(`AE_MUL16X4`, cuatro neuronas por instrucción) con los pesos ensanchados a
`int16` una vez en tiempo de compilación (101 KB más de imagen DSP), cuatro
acumuladores vectoriales y un contrato en coma fija.  Eso **no puede ser
bit-exacto** y habría que gastar el presupuesto de tolerancia de §5.

---

## 5. Fase 3: bit-exactitud, y qué la sostiene

### 5.1 Resultado

`make m33-dump dsp-dump compare` (el M33 en QEMU `mps2-an505`, el DSP en el ISS,
los dos con las mismas fuentes, el mismo arnés y las mismas 8 tramas de voz con
ruido a 10 dB):

```
frames bit-exact: 8/8
samples bit-exact: 3840/3840 (100.0000 %)
max |error|: 0.000000 LSB (full scale 32768)
VERDICT: bit-exact
```

Y el cuerpo de servicio, que añade la conversión `float`→`int16` (redondeo medio
fuera de cero, el mismo de `noise_suppress.c`), `make test`:

```
frames 8, samples 3840: 3840 identical (100.0000 %), max |diff| 0 LSB
```

No es «equivalente»: son los mismos bits.  Sobre 8 tramas, que es todo el
material de prueba disponible en el árbol (`bench/pcm_frames.h`); el criterio de
7 146 tramas del informe pide el corpus largo de 48 kHz que usó el banco del
M33, que no está versionado aquí.

### 5.2 Lo que hay que fijar para que lo siga siendo

La bit-exactitud **depende de dos decisiones de compilación**, y conviene que
quien integre lo sepa:

* **`-ffp-contract=off` en los dos lados.**  Con la contracción por defecto de
  GCC (`fast`, que es la del firmware tal como está), el M33 fusiona
  multiplicación y suma en VFMA donde le toca y el port no puede seguirlo: la
  misma comparación da **error máximo 0,777 LSB, SNR ≈ 72 dB**.  Inaudible, pero
  no es bit-exacto.  O el build del M33 compila `lib/rnnoise` con
  `-ffp-contract=off` cuando la etapa vive en el DSP, o se acepta esa tolerancia
  como criterio.
* **Nada más.**  `-fno-tree-vectorize` no cambia nada (medido: bit-exacto con y
  sin), y las tablas/`log10f`/literales de §3.1 son lo que elimina el resto de
  la deriva (sin ellas: 0,032 LSB y 0,7 % de muestras idénticas; luego
  0,002 LSB; luego 0).

---

## 6. Qué falta para que la radio lo use

1. **Servicio LSF en el DSP.**  El contrato está diseñado en `docs/NOISE.md`
   §«Service contract»: `CMD_NS_OPEN {480, 1 canal, Q15}` + un `ICStream` de
   captura de 480 muestras por sentido, 4 tramas de anillo (7,7 KiB del SRAM
   compartido de `0x200C0000`), en sitio, y `-ENOSYS` si la imagen DSP no lo
   trae.  El cuerpo ya está (`ns_dsp.h`: init + process), lo que falta es el
   pegamento en `dsp-openfw`: registrar el servicio junto a `lsf_audio`, sacar
   punteros de los `ICStream` y llamar a `ns_dsp_process()` por trama.
2. **Drivers de audio del DSP.**  `dsp-openfw/src/main_dsp.c` tiene el bucle y
   los *stubs* del ADC PDM y del DAC: sin ellos el DSP no puede sacar ni meter
   muestras, y el servicio no se puede probar en la radio.  Es el trabajo más
   grande que queda y no es de este port.
3. **Imagen DSP.**  Enlazar el port en la imagen (`link/venus_hifi4.ld`,
   `tools/mkimage.py`) y flashearla en `0x100000`.  Ojo a la memoria: 62 312 B
   de estado + 101 KB si algún día se meten pesos `int16` + el banco compartido
   de 64 KB, sobre la RAM del DSP que ya usa el anillo de audio.
4. **Decisión de banderas del M33** (§5.2) y **medida en la radio**: todo lo de
   aquí está medido en ISS/QEMU.  La cifra que manda («≤ 10 % del DSP en la
   radio») necesita contador de ciclos en el CSK6011B, igual que el M33 la tuvo
   con el *black box*.

---

## 7. Reproducción

```sh
source ~/xtensa/env.sh
cd dsp/ns
make m33-dump dsp-dump compare   # bit-exactitud contra el M33
make cost                        # instrucciones y ciclos por trama en el ISS
make m33-cost                    # lo mismo en el M33 (plugin de QEMU)
make test                        # cuerpo de servicio int16, contra el M33
make tables                      # diferencias de tablas entre las dos libm
make trace                       # traza por etapas de una trama
make cost SIMD=1                 # el kernel de 2 carriles (más lento, ver §4)
```

**Suite del host.**  No se pudo ejecutar en este árbol de trabajo: `meson test`
falla al compilar `tests/unit/ysf_frame.cpp:393` (`taking address of rvalue`,
GCC 16, `-fpermissive`) antes de llegar a ejecutar nada, y ese fichero no está
tocado por estos cambios.  Los cambios son aditivos: `dsp/` no aparece en ningún
`meson.build`/`CMakeLists.txt`, y en `lib/rnnoise` todo lo nuevo está detrás de
`#ifdef` que sólo activa el build del DSP (`RNNOISE_FIXED_TABLES`,
`RNNOISE_DSP_KERNELS`); el camino por defecto de la biblioteca compila y da la
misma salida (medido: el build del M33 con el `rnn.c` reestructurado es
bit-idéntico al de la biblioteca original, 3 840/3 840).
