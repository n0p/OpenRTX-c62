# OpenRTX para la Retevis C62

Firmware libre para la **Retevis C62** (también vendida como Chierda UV58N), basado en
[OpenRTX](https://openrtx.org). Sustituye al firmware de fábrica, se flashea por el
conector de accesorios y **no hace falta abrir la radio**.

Hardware: SoC **ListenAI CSK6011B** (Cortex-M33 a 300 MHz + HiFi4 + NPU), transceptor
**BK4819**, receptor de broadcast **BK1080**, flash QSPI de 4 MiB, pantalla ST7735R de
160x128 y códec de audio de 48 kHz servido por el DSP.

```
Estado: uso diario en FM, M17, DMR (RX y TX) y TETRA de recepción; el resto de
        modos digitales recibe, y transmiten los que tienen cadena propia
Imagen: 815.852 B de 1 MiB (77,8 %), RAM al 72,1 %
Pruebas: 112 suites en el host, todas en verde
Licencia: GPLv3 (ver LICENSE)
```

## Funciones

| Modo | RX | TX | Notas |
|---|---|---|---|
| FM analógica | si | si | 12,5 / 25 / 6,25 kHz, CTCSS (50 tonos, TX y RX), squelch con histéresis, VOX, compander |
| M17 | si | si | voz Codec2, indicativo y destino configurables, packet mode |
| TETRA TMO | si | no | voz ACELP, AFC, MCC/MNC y celda en pantalla, SDS con historial, sigue el timeslot de la llamada |
| DMR Tier II/III | si | si | vocoder AMBE+2 y encoder propios; `AUTO`/TS1/TS2, CC, llamada y calidad en pantalla; SMS; troncalizado T3 |
| NXDN | si | si | RAN, 6,25/12,5 kHz, llamada y tipo de mensaje en pantalla |
| dPMR (TS 102 490) | si | si | 4FSK en 6,25 kHz, CCH, voz AMBE+2 |
| P25 Fase 1 | si | si | NAC, DUID y llamada en pantalla; voz con el decodificador IMBE propio |
| System Fusion (YSF) | si | si | C4FM; la voz de tasa completa es AMBE y no se decodifica |
| D-STAR DV | si | si | indicativos, datos lentos y voz AMBE 3600x2400 |
| AM aéreo (118-137 MHz) | si | si | AGC, ráster de 25 / 8,33 kHz, designador de canal, TX con diálogo de confirmación |
| FM de radiodifusión (85-108 MHz) | si | no | sintonizador BK1080 propio, estéreo; sin RDS (el chip no lo tiene) |
| APRS sobre FM | si | si | mensajería (SMS con ACK) como función principal |

Que un modo transmita **en el host** (modulador y demodulador propios, verificado
bit a bit) no es lo mismo que estar medido en el aire: lo que falta por banco está
en *Qué falta*, al final.

**Canales y manejo**

- 128 canales guardados en la radio, sin ordenador: crear, duplicar, editar y borrar.
- Por canal: nombre, RX y TX, modo, ancho de banda, potencia, tono y CC/TS de DMR.
- Entrada directa de frecuencia por teclado, resolución de 10 Hz.
- Vuelve al último canal usado al encender; los ajustes se conservan al apagar.
- Rango completo del BK4819: **18-660 MHz y 840-1300 MHz**, sin límites de bandas de
  aficionado. Transmitir fuera de las bandas permitidas es responsabilidad del operador.

**Pantalla**

- Menú emergente contextual con la pulsación corta de P2: solo lo que se puede
  cambiar en el modo en curso, y nada más.
- El **estado del modo** (CC y timeslot de DMR, RAN de NXDN, NAC de P25, celda y
  llamada de TETRA, canal y llamada de dPMR…) se lee en la **pantalla principal**,
  nunca dentro de un submenú: los menús son para configurar.
- Línea inferior de ayuda que dice qué hace cada botón —verde, P1, P2 y rojo— y que
  se comprueba con un test: si una tecla anunciada no hace nada, la suite falla.
- Ninguna fila muerta: la auditoría de menús recorre las 54 pantallas y falla si
  una fila no hace nada o si un ajuste aparece en dos sitios.

**Timeslots**

DMR tiene dos y TETRA cuatro; el resto de modos son FDMA y no tienen ninguno. En
DMR la fila `TS` del menú rápido ofrece `AUTO`, `TS1` y `TS2`: en `AUTO` la radio
escucha los dos, se queda con el que lleva la llamada y la sigue hasta que acaba
o pasan cinco segundos sin oírla. El porqué y el detalle por modo están en
[`docs/TIMESLOTS.md`](docs/TIMESLOTS.md).

**APRS**

Va sobre FM, sin modo aparte. Tres interruptores independientes, todos apagados de
fábrica: *decoding* (escuchar y mostrar paquetes), *beacon* (balizas periódicas) y el
envío de mensajes, que sale de una cola explícita y **nunca del PTT**. La función
principal es la mensajería: recibir, responder y confirmar con ACK, con bandeja
persistente. P2 largo en la pantalla principal abre APRS; en el resto de pantallas
mantiene su función habitual.

**Seguridad de transmisión**

El puerto de accesorios comparte línea física con el PTT, así que un cable que se
queda pulsado puede parecer un botón: el detector lee el registro de estado del UART,
no solo los caracteres. Un vigilante corta la transmisión a los 90 s sin rekey, la
clave del PTT nunca se arma sola desde el menú y el AM aéreo pide confirmación
explícita. Todo ello está en [`docs/TX_SAFETY.md`](docs/TX_SAFETY.md).

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
fábrica, que es la que funciona con el resto del sistema. Si el flasheo falla con
`E4001: Device did not respond after reset`, la radio no está en modo burn.

**Volver al firmware de fábrica**

```sh
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 0x000000 factory-c62.bin
```

El dump completo de la radio (4 MiB) restaura aplicación, recursos, calibración y
canales. Guárdalo antes de flashear: contiene la calibración de **tu** unidad.

## Compilación

Lo más simple es usar la imagen de Docker que trae Zephyr y el SDK del CSK6:

```sh
docker build -t openrtx-c62 .
docker run --rm -v "$PWD":/work openrtx-c62 c62-build \
    -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y \
    -DCONFIG_C62_NXDN_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_P25=y \
    -DCONFIG_C62_DPMR_RX=y -DCONFIG_C62_DSTAR=y \
    -DCONFIG_DMR_TX=y -DCONFIG_NXDN_TX=y -DCONFIG_DPMR_TX=y \
    -DCONFIG_P25_TX=y -DCONFIG_YSF_TX=y -DCONFIG_C62_KISS=y \
    -DC62_PC_PROTO=ON -DC62_DEBUG_PROTO=ON -DC62_BENCH_PROTO=ON -DC62_AIR_TX=ON
```

Los binarios quedan en `build_c62/zephyr/`:

| Fichero | Destino | Tamaño con las opciones de arriba |
|---|---|---|
| `zephyr.bin` | `0x000000` | 815.852 B (77,8 % de la partición de 1 MiB), RAM 72,1 %, DTCM 95,9 % |
| `dsp_firmware.bin` | `0x100000` | 466.832 B (la de fábrica) |

Opciones de compilación relevantes (todas apagadas por defecto salvo que se diga lo
contrario):

- `CONFIG_C62_DMR_RX` — receptor DMR con vocoder AMBE+2 propio.
- `CONFIG_C62_APRS` — APRS sobre FM (mensajería, baliza y decoding).
- `CONFIG_C62_NXDN_RX`, `CONFIG_C62_DPMR_RX`, `CONFIG_C62_YSF`, `CONFIG_C62_P25`,
  `CONFIG_C62_DSTAR` — un receptor por modo digital.
- `CONFIG_C62_P25_MBELIB` — deja la voz de P25 en mbelib en vez del decodificador
  propio (48 veces más caro).
- `CONFIG_DMR_TX`, `CONFIG_NXDN_TX`, `CONFIG_DPMR_TX`, `CONFIG_P25_TX`,
  `CONFIG_YSF_TX`, `CONFIG_DSTAR_TX` — cadenas de transmisión. DMR TX arrastra el
  encoder AMBE+2 propio; los demás transmiten con su modulador y su plano de
  ráfagas.
- `CONFIG_DMR_SMS` — SMS de DMR (formato, cola, ACK).
- `CONFIG_DMR_TRUNK` — motor troncalizado T3: sigue el grant del sistema, incluido
  el timeslot.
- `CONFIG_C62_KISS` — módem de paquetes (AFSK 1200 y 9600) y KISS sobre el puerto
  serie, para APRS y packet mode de M17.
- `CONFIG_C62_UNLOCKED_TUNING` — rango completo del BK4819 (activada por defecto).
- `C62_PC_PROTO` — protocolo binario radio-PC por el cable de programación, para un
  CPS y para el banco: canales, contactos, zonas, ajustes, estado en vivo y eventos.
  Ver [`docs/PC_PROTO.md`](docs/PC_PROTO.md) y el cliente de referencia
  `tools/cps/cps.py`.
- `C62_DEBUG_PROTO` — además, el espacio de nombres de depuración del protocolo (log
  en streaming, diagnóstico, registros del BK4819/BK1080, memoria acotada, captura de
  audio RX, traza de TX). Una build de producción no la lleva ni la anuncia.
- `C62_BENCH_PROTO` — espacio de nombres del banco de pruebas (tune, PTT con tono,
  captura) usado por `tools/bench/`.
- `C62_AIR_TX` — transmisión de AM en la banda aérea, detrás de un diálogo de
  confirmación ([`docs/TX_AIRBAND.md`](docs/TX_AIRBAND.md)).
- `C62_BASEBAND_DUMP` — volcado del audio del discriminador por el puerto serie, para
  diagnosticar un modo digital que no decodifica; solo se arma encendiendo con la
  tecla A/B pulsada. Ver [`docs/DMR_RX.md`](docs/DMR_RX.md).
- `C62_DMR_12K5` — DMR en el modo de canal de 12,5 kHz del BK4819 (filtros de FI de
  4,5 kHz) en vez del de 25 kHz.
- `C62_TETRA` — modo TETRA de recepción (activado por defecto), con voz OpenACELP
  (`C62_TETRA_SPEECH`).
- `C62_VOICE_STATS` — las cifras de voz que el firmware imprimía cada 10 s por el
  puerto serie. Apagada por defecto: cuestan 1,7 s de planificador cada 10 s, porque
  recorren los hilos con el planificador bloqueado.
- `CONFIG_C62_NOISE_SUPPRESS` — supresión de ruido RNNoise (v0.1.1, BSD-3) en la
  recepción de FM, tras el interruptor *Settings → Audio → RX Noise*. Apagada por
  defecto **y el interruptor también**: cuesta 1,9 M instrucciones por trama de 10 ms
  (66-73 % del M33 a 300 MHz) y 103 KiB de flash, así que en el M33 es una ruta de
  laboratorio; las cifras y el diseño para el DSP están en
  [`docs/NOISE.md`](docs/NOISE.md).

## Pruebas

Hay **112 suites en el host**, y el criterio de cada una es que **falle cuando la
conducta se rompe**, no que pase hoy: cada arreglo de este árbol llegó con el test
que lo sostiene y con la comprobación de que, quitando el arreglo, el test se pone
en rojo.

```sh
meson setup build_linux -Dcpp_args=-fpermissive
ninja -C build_linux -k 0          # los objetivos ARM fallan a propósito aquí
ninja -C build_linux sim-coverage.txt
meson test -C build_linux --no-rebuild
```

Los tests de TETRA necesitan el árbol de libtetra: `-Dtetra_lib_dir=/ruta/a/libtetra`.

Qué cubren, por orden de tamaño:

- **Dobles de receptor**: basebanda sintética construida por el **modulador del
  propio firmware**, metida por el demodulador real, y se afirma lo que publica el
  estado del modo (CC y timeslot de DMR, RAN de NXDN, NAC de P25, canal y llamada de
  dPMR, celda de TETRA) y también lo que **rechaza** (silencios, CRC roto, color code
  mal corregido, trama cortada).
- **Configuración del chip como dato**: el plan `{registro, máscara, valor}` de cada
  modo contra una tabla de oro, acceso por acceso (`tests/unit/regplan.cpp`).
- **UI**: recorrido del árbol de estados del FSM real (20 000 estados), teclas
  anunciadas contra lo que hacen, ajuste del texto a la pantalla, estados vacíos que
  no dicen `CC0`, auditoría de menús sin filas muertas ni ajustes duplicados.
- **Simulador**: recorrido completo de menús con capturas, y persistencia de ajustes
  a través de un reinicio y de un corte de corriente.
- **Protocolo PC/banco**: reglas de token, PTT y eventos sobre un pty.
- **Plataforma**: presupuesto de pila por hilo con `_Static_assert`, reglas del
  vigilante de TX y cableado del supervisor.

El plan completo, con lo que queda sin cubrir y por qué, está en
[`docs/TEST_PLAN.md`](docs/TEST_PLAN.md). El simulador de la interfaz
(`build_linux/openrtx_c62_sim`) permite recorrer menús y pantallas sin la radio:
[`docs/SIMULATOR.md`](docs/SIMULATOR.md).

## Repositorio

`master` es lo que se flashea. La historia del port está en commits por área (placa,
drivers, core, rtx, UI, DMR, NXDN/dPMR, P25/YSF/D-STAR, TETRA, paquete, protocolos,
herramientas, tests, docs, build) sobre el último commit de OpenRTX; el histórico
detallado, commit a commit, se conserva en `backup/master-pre-squash`.

- `docs/` — una nota por tema: `UI.md`, `CHANNELS.md`, `SETTINGS.md`, `ZONES.md`,
  `MESSAGING.md`, `APRS.md`, `KISS.md`, `TIMESLOTS.md`, `TEST_PLAN.md`,
  `SIMULATOR.md`, `MENU_AUDIT.md`, y por modo `RX_NXDN.md`, `RX_DPMR.md`,
  `RX_P25.md`, `RX_FUSION.md`, `RX_DSTAR.md`, `RX_AIRBAND.md`, `RX_WFM.md`,
  `TETRA_LIB.md`, `DMR_TX.md`, `TX_NXDN.md`, `TX_DPMR.md`,
  `TX_P25.md`, `TX_YSF.md`, `TX_DSTAR.md`, `TX_AIRBAND.md`, `TX_SAFETY.md`,
  `DMR_RX.md`, `DMR_VOCODER.md`, `DMR_PKG.md`, `RADIO_CONFIG.md`, `SPECTRUM.md`,
  `SCAN.md`, `VFO_SEARCH.md`, `STATS.md`, `NOISE.md`, `MEMORY.md`,
  `INTEGRATION.md`, `HACKING.md`, `PC_PROTO.md`, `BENCH.md`, `BLACKBOX.md`.
- `tools/cps/` — CPS de referencia por el puerto de programación.
- `tools/bench/` — banco de medidas con un SDR (claveo, barrido de modos, forma de
  espectro, correlación).
- `tools/simulator/` — recorrido de menús, capturas y auditoría.

## Qué falta

- **Medir en banco** lo que el host no puede: la desviación de TX de NXDN, dPMR, P25,
  YSF y D-STAR (cada modo escribe ya su palabra, pero el número no está calibrado), la
  profundidad de AM aérea, la sensibilidad y la máscara de potencia. El DMR ya emite
  con su forma correcta en el host; en el aire queda confirmar la desviación.
- **DMR en RX por aire**: el CC ya no baila y la trama no se corta en falso, pero la
  recepción en condiciones reales está pendiente de una pasada de banco.
- Encoder AMBE-2020 para la voz de tasa completa de YSF y para la de D-STAR (DMR y
  P25 ya tienen el suyo, y el árbol trae un encoder IMBE libre).
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
Liberty SW License Agreement V2) y `uf2conf.py` (MIT, Microsoft). RNNoise es
BSD-3-Clause y conserva sus propios avisos en `lib/rnnoise/`.

## Contacto y donaciones

- OpenRTX: [M17 Reflector](https://m17.openrtx.org), Matrix
  [#openrtx_general:matrix.org](https://matrix.to/#/#openrtx_general:matrix.org) y
  [Discord](https://discord.gg/TbR2FVtMya).
- Traducciones: [Weblate](https://hosted.weblate.org/projects/openrtx/).
- Donaciones al proyecto original: [Liberapay](https://liberapay.com/OpenRTX/donate).

## Licencia

GNU GPL v3. Ver `LICENSE`.
