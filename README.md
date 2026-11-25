# OpenRTX para la Retevis C62

Firmware libre para la **Retevis C62** (también vendida como Chierda UV58N), basado en
[OpenRTX](https://openrtx.org). Sustituye al firmware de fábrica, se flashea por el
conector de accesorios y **no hace falta abrir la radio**.

Hardware: SoC **ListenAI CSK6011B** (Cortex-M33 a 300 MHz + HiFi4 + NPU), transceptor
**BK4819**, receptor de broadcast **BK1080**, flash QSPI de 4 MiB, pantalla ST7735R de
160x128 y códec de audio de 48 kHz servido por el DSP.

```
Estado: uso diario en FM, M17 y DMR; el resto de modos digitales en recepción
Licencia: GPLv3 (ver LICENSE)
```

## Funciones

| Modo | RX | TX | Notas |
|---|---|---|---|
| FM analógica | si | si | 12,5 / 25 / 6,25 kHz, CTCSS y DCS, squelch, VOX, compander |
| M17 | si | si | Codec2, indicativo y destino configurables |
| TETRA TMO | si | no | voz ACELP, AFC, MCC/MNC y celda en pantalla, SDS con historial |
| TETRA DMO | en pruebas | no | llamadas directas entre radios |
| DMR Tier II/III | si | en pruebas | vocoder AMBE+2 propio, CC/slot/tipo de ráfaga en pantalla |
| NXDN | si | no | RAN, 6,25/12,5 kHz, llamada y tipo de mensaje en pantalla |
| dPMR (TS 102 490) | si | no | 4FSK en 6,25 kHz, datos cortos y voz AMBE+2 |
| P25 Fase 1 | si (sin voz) | no | NAC, DUID y llamada en pantalla; falta el decodificador IMBE |
| System Fusion (YSF) | si (sin voz FR) | no | C4FM; la voz de tasa completa es IMBE y no se decodifica |
| D-STAR DV | si | en pruebas | indicativos, datos lentos y voz AMBE 3600x2400; TX con trama nula |
| AM aéreo (118-137 MHz) | si | no | AGC, ráster de 25 / 8,33 kHz, designador de canal |
| FM de radiodifusión (85-108 MHz) | si | no | sintonizador BK1080 propio, estéreo; sin RDS (el chip no lo tiene) |
| APRS sobre FM | si | si | mensajería (SMS con ACK) como función principal |

**Canales y manejo**

- 128 canales guardados en la radio, sin ordenador: crear, duplicar, editar y borrar.
- Por canal: nombre, RX y TX, modo, ancho de banda, potencia, tono y CC/TS de DMR.
- Entrada directa de frecuencia por teclado, resolución de 10 Hz.
- Vuelve al último canal usado al encender; los ajustes se conservan al apagar.
- Rango completo del BK4819: **18-660 MHz y 840-1300 MHz**, sin límites de bandas de
  aficionado. Transmitir fuera de las bandas permitidas es responsabilidad del operador.

**Pantalla**

- Menús Audio, Radio, FM, Display y TETRA, con fuente de 5/6 pt y filas compactas.
- Menú emergente contextual con la pulsación corta de P2: squelch, tono, potencia,
  ancho de banda y los ajustes propios del modo en curso.
- Línea inferior de ayuda que indica qué hace cada botón: verde, P1, P2 y rojo.

**APRS**

Va sobre FM, sin modo aparte. Tres interruptores independientes, todos apagados de
fábrica: *decoding* (escuchar y mostrar paquetes), *beacon* (balizas periódicas) y el
envío de mensajes, que sale de una cola explícita y **nunca del PTT**. La función
principal es la mensajería: recibir, responder y confirmar con ACK, con bandeja
persistente. P2 largo en la pantalla principal abre APRS; en el resto de pantallas
mantiene su función habitual.

## Instalación

1. Apaga la radio.
2. Mantén pulsada la **PTT secundaria** y enciende: entra en modo *burn* (pantalla en
   blanco).
3. Conecta el cable USB-serie y flashea:

```sh
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 0x000000 zephyr.bin
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 0x100000 dsp_firmware.bin
```

4. Apaga y enciende normalmente.

`dsp_firmware.bin` es la imagen del DSP: mientras no haya una libre se usa la de
fábrica, que es la que funciona con el resto del sistema.

**Volver al firmware de fábrica**

```sh
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 0x000000 factory-c62.bin
```

El dump completo de la radio (4 MiB) restaura aplicación, recursos, calibración y
canales. Guárdalo antes de flashear: contiene la calibración de **tu** unidad.

**Aviso sobre los ajustes**: la estructura de ajustes ha cambiado respecto a versiones
anteriores. Si tras flashear algo se comporta de forma extraña, haz un reset de ajustes
desde el menú.

## Compilación

Lo más simple es usar la imagen de Docker que trae Zephyr y el SDK del CSK6:

```sh
docker build -t openrtx-c62 .
docker run --rm -v "$PWD":/work openrtx-c62 c62-build \
    -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y \
    -DCONFIG_C62_NXDN_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_P25=y \
    -DCONFIG_C62_DPMR_RX=y -DCONFIG_C62_DSTAR=y
```

Los binarios quedan en `build_c62/zephyr/`:

| Fichero | Destino | Tamaño actual |
|---|---|---|
| `zephyr.bin` | `0x000000` | 614.740 B (58,6 % de la partición de 1 MiB) |
| `dsp_firmware.bin` | `0x100000` | 466.832 B |

Opciones de compilación relevantes (todas apagadas por defecto salvo que se diga
lo contrario; con las siete de arriba, RAM al 86,5 % — ver
`docs/INTEGRATION.md`):

- `CONFIG_C62_DMR_RX` — receptor DMR con vocoder AMBE+2 propio.
- `CONFIG_C62_APRS` — APRS sobre FM (mensajería, baliza y decoding).
- `CONFIG_C62_NXDN_RX` — NXDN de recepción.
- `CONFIG_C62_DPMR_RX` — dPMR de recepción (TS 102 490).
- `CONFIG_C62_YSF` — System Fusion de recepción.
- `CONFIG_C62_P25` — P25 Fase 1 de recepción (sin decodificador IMBE).
- `CONFIG_C62_DSTAR` — D-STAR DV; arrastra la voz AMBE 3600x2400
  (`CONFIG_C62_DSTAR_VOCODER`) y el transmisor (`CONFIG_DSTAR_TX`).
- `CONFIG_DMR_TX` — cadena de transmisión DMR. **Apagada por defecto**: sin encoder
  AMBE+2 el PTT se niega a transmitir.
- `CONFIG_C62_UNLOCKED_TUNING` — rango completo del BK4819 (activa por defecto).
- `C62_BASEBAND_DUMP` — volcado del audio del discriminador por el puerto serie.
  **Apagada por defecto** (con ella, PSRAM al 77,8 %; sin ella, 20,7 %): se
  enciende con `-DC62_BASEBAND_DUMP=ON` y luego el volcado **solo se arma
  encendiendo con la tecla A/B pulsada**. Es la herramienta para diagnosticar un
  modo digital que no decodifica; ver `docs/DMR_RX.md`.
- `C62_DMR_12K5` — DMR en el modo de canal de 12,5 kHz del BK4819 (filtros de FI de
  4,5 kHz, `REG_43 = 0x7e48`) en vez del de 25 kHz (`0x7e28`). Apagada por defecto.
- `C62_PC_PROTO` — protocolo binario radio-PC por el cable de programación, para
  un CPS y para el banco: canales, contactos, zonas, ajustes, estado en vivo y
  eventos. **Apagada por defecto**; ver `docs/PC_PROTO.md` y el cliente de
  referencia `tools/cps/cps.py`.
- `C62_DEBUG_PROTO` — además, el espacio de nombres de depuración del protocolo
  (log en streaming, diagnóstico, registros del BK4819/BK1080, memoria acotada,
  captura de audio RX, traza de TX). **Apagada por defecto**: una build de
  producción no la lleva ni la anuncia.

## Repositorio

- `master` — lo que se flashea.
- `c62/dmr-tx` — cadena TX de DMR y encoder, en desarrollo.
- `c62/tx-c4fm` — TX de P25 y System Fusion (C4FM) y de D-STAR, en desarrollo.
- `Retevis-c62` — base del port.
- `docs/` — notas de integración: `UI.md`, `CHANNELS.md`, `APRS.md`, `DMR_TX.md`,
  `DMR_VOCODER.md`, `DMR_RX.md`, `TX_P25.md`, `TX_YSF.md`, `TX_DSTAR.md`,
  `PC_PROTO.md`, `openrtx_integration.md`.

## Qué falta

- Encoder AMBE+2 para DMR TX (en curso) y calibración de desviación en banco
  (`REG_40`), que es lo que falta para los tres TX nuevos: `docs/TX_P25.md`,
  `docs/TX_YSF.md`, `docs/TX_DSTAR.md`.
- Encoder IMBE (P25) y AMBE-2020 (D-STAR): no hay ninguno libre, por eso esos
  dos transmiten sin voz.
- TX de TETRA: no es viable con este transmisor, la señal se sale de la máscara.
- Imagen de DSP libre: la de fábrica funciona, pero no es sustituible todavía.
- APRS: digipeater, igate y SmartBeaconing.

## Sobre OpenRTX

Este repositorio es un fork de [OpenRTX](https://github.com/OpenRTX/OpenRTX), el firmware
libre y modular para radioaficionados, y del port de la C62 de **OE3ANC** y **sp5wwp**.
La documentación general del proyecto, el estado de soporte de cada equipo y las
instrucciones de compilación están en [openrtx.org](https://openrtx.org).

Créditos adicionales: **libtetra/OpenACELP** para TETRA, **OpenDMR/mbelib-neo** como
referencia del vocoder AMBE+2 (reescrito en C para este SoC) y **direwolf** como
referencia de validación del módem APRS.

## Disclaimer

Proyecto creado para investigación y uso radioaficionado. No nos hacemos responsables
del uso indebido de este código que pueda dar lugar a transmisiones no autorizadas.

El firmware se distribuye SIN NINGUNA GARANTÍA: quien flashee una imagen construida a
partir de estas fuentes lo hace bajo su propio riesgo.

## Créditos del proyecto original

OpenRTX está hecho por Niccolò Izzo IU2KIN, Silvano Seva IU2KWO, Federico Amedeo Izzo
IU2NUO y Frederik Saraci IU2NRO, con contribuciones de la comunidad (entre ellas los
avisos de voz de Joseph Stephen VK7JS). Todo esto es posible gracias al enorme trabajo
de ingeniería inversa de Travis Goodspeed y los contribuyentes de
[md380tools](https://github.com/travisgoodspeed/md380tools), y a la inspiración y el
código de ejemplo de Roger Clark y su OpenGD77. Gracias también a Wojciech Kaczmarski
SP5WWP y a la comunidad [M17](https://m17project.org) por traer su protocolo libre a
este hardware.

Licencias de terceros del proyecto original: minmea (WTFPL v2),
[QRCode](https://github.com/ricmoo/QRCode) (MIT), driver USB del STM32F405 (MCD-ST
Liberty SW License Agreement V2) y `uf2conf.py` (MIT, Microsoft).

## Contacto y donaciones

- OpenRTX: [M17 Reflector](https://m17.openrtx.org), Matrix
  [#openrtx_general:matrix.org](https://matrix.to/#/#openrtx_general:matrix.org) y
  [Discord](https://discord.gg/TbR2FVtMya).
- Traducciones: [Weblate](https://hosted.weblate.org/projects/openrtx/).
- Donaciones al proyecto original: [Liberapay](https://liberapay.com/OpenRTX/donate).

## Licencia

GNU GPL v3. Ver `LICENSE`.
