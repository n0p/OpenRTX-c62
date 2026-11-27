<!--
SPDX-FileCopyrightText: 2026 OpenRTX Contributors

SPDX-License-Identifier: GPL-3.0-or-later
-->

# Códecs: qué se ha aplicado de `reports/codecs.md`

Fecha: 2026-11-27 · Rama `codec/audio` (desde `codec-eval`, 15186c08) · Árbol:
`~/src/c62/work/codec-audio`. Un commit por propuesta, en inglés, con el porqué
y las cifras. Sin radio, sin `/dev/ttyUSB*`, nada subido a ningún remoto.

De las tres propuestas que toca esta ronda (§3.1, §3.3 y, como comparación
§3.8), **una se sostiene entera, otra a medias y la tercera ha quedado sin
objeto**.

## Tabla

| # | Propuesta | Veredicto | Commit |
|---|---|---|---|
| 1 (§3.1) | Filtro polifásico 6:1 y 1:6 en `csk6_codec` | **hecho**, y más barato de lo presupuestado | `072bc50b` |
| 3 (§3.3) | Codec2 con `-O2` | **hecho** | `4e7984de` |
| 3 (§3.3) | Codec2 con pila de 8 KiB | **rechazado por medida**: el high-water real es 15 484 B | `4e7984de` |
| 8 (§3.8) | Reproducción de 8 kHz por el mezclador del DSP | **sin objeto**: 1 ya deja las imágenes a −88 dB | — |

## Las cifras

Medidas con `scripts/codec_cost.sh` (QEMU mps2-an505, contador de
instrucciones, host y objetivo comparados por hash antes de creer el número):

| | antes | después | criterio del informe |
|---|---:|---:|---|
| alias 6 500 Hz → 1 500 Hz | 0 dB | **−82,1 dB** | ≥ 60 dB |
| imagen 1 kHz en 7 kHz | −13,5 dB | **−87,6 dB** | ≥ 40 dB |
| imagen 1 kHz en 9 kHz | −13,5 dB | **−93,1 dB** | — |
| resampler, 10 ms, ambos sentidos (−O2) | — (antes no había filtro; la referencia del informe medía 63 073) | **47 257** | ≤ 70 000 |
| resampler, 10 ms, ambos sentidos (−Os, el build real) | — | **55 626** | — |
| Codec2 decodifica, trama de 20 ms | 951 194 (−Os) | **930 830** | ≤ 935 000 |
| Codec2 codifica, trama de 20 ms | 752 120 (−Os) | **722 464** | ≤ 725 000 |
| Codec2 decodifica, high-water de pila | — | **15 484 B de 16 384** | ≤ 6 KiB |

Firmware completo (docker, opciones de la imagen de `docs/HACKING.md`), de
`15186c08` a la rama:

| | antes | + propuesta 1 | + propuesta 3 |
|---|---:|---:|---:|
| FLASH | 828 184 | 829 184 | 830 892 |
| RAM | 193 328 | 193 328 | 193 328 |
| ITCM | 8 472 | 8 472 | 8 472 |
| DTCM | 15 712 (95,90 %) | 15 712 | 15 712 |
| PSRAM | 672 046 | 672 534 | 672 534 |

## Por qué la pila se queda en 16 KiB

El informe dedujo 8 KiB de «marcos de pila 7 016 B (decodifica)» con
`-fstack-usage`. 7 016 B es **un** marco (`codec2_decode_3200`), no la cadena:
debajo están `synthesise_one_frame` (1 328 B) y `synthesise` (4 176 B), que con
el buffer de 640 B de `decodeFunc` ya son 13 160 B antes de las llamadas a
`kiss_fft` y a las conversiones de LSP. `tests/qemu/codec2_stack.c` pinta
una pila de `CODEC2_THREAD_STKSIZE` bytes y llama al camino del hilo del códec
por un cambio de pila explícito, así que no cuenta los marcos del arnés:

* decodifica 15 484 B (−O2) y 15 412 B (−Os): `-O2` no la engorda;
* codifica 10 716 B;
* con la pila en 8 192 B el mismo arnés no vuelve nunca.

Es decir: la reserva de 16 KiB no es holgura, son 900 B de margen. Bajarla no
ahorra 8 KiB, cuelga la radio en el instante en que M17 ocupa el PTT.
`openrtx/include/core/threads.h` lleva la medida al lado del número. Lo honesto
para una vuelta siguiente es al revés —bajar el consumo, no la reserva—, o
asumir el coste en el heap, de donde ya salen los 37 KiB de estado del códec.

## Lo que no se ha hecho

* **Los 1 000 tramas de M17 TX/RX sin fallo**: sin radio no hay lazo que
  probar. Lo que sí está medido es que el build `-O2` produce **los mismos
  bytes** que el `-Os`: 1 000 tramas de codifica+decodifica en el host dan el
  mismo hash (4536e455) compiladas de las dos maneras, y el arnés de coste da
  los mismos 77f7d80e / 456ae00c. El cambio es de compilador, no de códec.
* **PESQ del lazo M17**: no hay forma de medirlo sin radio. Lo que sí queda
  medido es lo que lo degradaba: el alias a −82 dB y las imágenes a −88 dB,
  con el paso de 1 kHz a −0,01 dB.
* **16 y 24 kHz**: los moduladores de APRS, NXDN y P25 piden 24 kHz y siguen
  con selección de muestras. No son el camino de voz y filtrarlos pide otro
  prototipo; el comentario de cabecera de `csk6_codec.c` lo dice.

## Suite

`meson test -C build-host`: **114/114**, `Interrupt Context Rules` incluido.
`ninja -k 0` sigue devolviendo error en los targets ARM/miosix (no hay
toolchain en el host) y en los dos de host que ya fallaban antes; ninguno toca
estos ficheros.
