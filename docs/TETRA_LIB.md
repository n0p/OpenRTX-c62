# Motor TETRA: libtetra de elkifo

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
describe de dónde sale el motor TETRA del firmware, qué API se usa y cómo se
mapea a nuestras estructuras, qué se gana con él, qué no se usa y por qué, y
cómo se comprueba todo sin radio (tests de la librería, capturas reales, coste
en QEMU). La demodulación propia y la voz están en `docs/DMR_RX.md`,
`TETRA_LIB.md` es el resto de la cadena.

## 1. Qué hay integrado y de dónde viene

El motor de recepción TETRA del firmware **es libtetra**: capa 1 (ráfagas,
AACH/SCH, scrambling), LMAC, MAC de TMO, LLC, MLE, MM, CMCE, SDS-TL, LIP y
decodificación de canal de voz a tramas de 137 bits para OpenACELP. No hay una
segunda implementación de esas capas en el árbol.

Llega como **módulo de west**, no vendorizado:

```yaml
# west.yml
- name: libtetra
  remote: sp5wwp
  revision: 6fc088ce7cbad678772054d5bacbc1163548887b
  path: modules/lib/libtetra
  groups: [csk6]
```

`platform/targets/c62/CMakeLists.txt` lo añade con `add_subdirectory`, le pasa
los flags de Zephyr (CPU, FPU, ABI) y enlaza el target `tetra` al firmware;
`-DC62_TETRA=OFF` lo deja fuera.

### Por qué ese commit y no `05b32aa`

`sp5wwp/libtetra` es un fork de `elkifo/libtetra`: **`05b32aa` es ancestro
directo del commit que usa el firmware** (`git merge-base --is-ancestor 05b32aa
HEAD` → sí, en `modules/lib/libtetra`). Encima hay ocho commits, dos de ellos
PRs del propio elkifo ya fusionados:

| commit | qué aporta |
| --- | --- |
| `c37b4db` | cuenta los bloques de downlink cuyo CRC pasa (`tetra_tmo_crc_ok`) |
| `ed4013e` | mueve la rejilla de slots a una ráfaga de sincronismo cuando hay lock pero no decodifica |
| `cdac2b9` | un *usage marker* pertenece a una llamada por canal |
| `edc3792` | re-alineado más estricto y `tetra_tmo_realigned()` |
| `4b8579a` | mantiene el tráfico cuando la portadora del receptor no se conoce |
| `9551c9e`, `1a763f3`, `2517714` | contadores en `tetra_rx`, reglas de instalación, `tetra_tmo_gen` |
| `6fc088c` | mantiene el tráfico de un slot cuando su AACH no decodifica |

Nuestro `OpMode_TETRA.cpp` y el banco de host **llaman a `tetra_tmo_crc_ok()` y
`tetra_tmo_realigned()`**, que no existen en `05b32aa`: fijar la dependencia en
`05b32aa` rompe la compilación además de perder los arreglos. Por eso la
dependencia se queda donde está y este documento fija la ascendencia; si algún
día hay que volver al upstream puro, primero hay que subir esos dos accesores.

Vendorizar una copia en `lib/tetra` sería duplicar 9,3 kLOC que ya llegan por
west (y que `west update` mantiene al día) sin ganar nada; lo que sí hacía
falta era dejar por escrito la procedencia y la licencia, que es lo que hace
la sección 2.

## 2. Licencia y atribución

| | ficheros | licencia |
| --- | --- | --- |
| librería | 53 | `GPL-3.0-or-later` (cabecera SPDX en cada uno) |
| cripto derivada de osmo-tetra | 6 (`src/crypto/{hurdle,taa1,tb5,tea1,tea2,tea3}.c`) | `AGPL-3.0` |
| sin línea SPDX | 2 (`src/crypto/{hurdle,taa1}.h`) | AGPL-3.0 por herencia de osmo-tetra |

`ta61.c` y `tetra_crypto.h` son de elkifo y van en GPL: solo el cifrador
heredado de osmo-tetra es AGPL. La AGPL y la GPL-3.0 del firmware son
compatibles; el problema práctico es que la AGPL alcanzaría al binario
distribuido, y por eso la cripto **no se compila en el firmware**
(`TETRA_ENABLE_CRYPTO=OFF`, el valor por defecto de la librería): no hay
ninguna clave ni proveedor de claves que la use.

Comprobación, no promesa:

```bash
scripts/tetra_license_check.sh            # inventario + símbolos en la imagen
```

Verifica los SPDX de cada fichero y que la imagen enlazada no lleve símbolos de
cripto (`tetra_tea*`, `tetra_tb5`, `tetra_ta61`, `hurdle`).

## 3. La cadena de recepción

```
audio discriminador 48 kHz (SOURCE_RTX → SINK_MCU, bloques de 30 ms)
   └─ TetraDemod_process()        nuestro demodulador (openrtx/src/protocols/TETRA)
        └─ soft bits (int8, >0 = 1, 1080 por bloque)
             └─ tetra_tmo_push_soft()   libtetra: sincronismo, LMAC, L2/L3
                  └─ tetraStatus_onEvent()   callback (hilo rtx)
                       ├─ tetraSpeech_traffic() → OpenACELP        (voz)
                       ├─ tetraSds_onEvent()   → historial + flash (SDS)
                       └─ struct tetraStatus   → UI y menú rápido
```

- `tetra_tmo_push_soft()` recibe **soft bits**, no dibits: el demodulador
  entrega la decisión con su fiabilidad y el Viterbi de la librería la usa.
- El callback corre en el hilo `rtx_task`, dentro de `push_soft`: solo toca
  RAM y un mutex; nada de flash (el SDS se persiste desde `tetraSds_task()` en
  una tarea normal).
- Cuando el demodulador re-adquiere (salto de temporización, muestras
  perdidas) se reinicia el receptor con `tetra_tmo_init()` y se acumulan los
  contadores de CRC; `tetra_tmo_resync()` es la variante que conserva la
  información de célula.

### Qué demodulador se queda: el nuestro

`tools/tetra_demod.c` de la librería es explícitamente **host-only** ("Not part
of the MCU library"): `double`, `complex.h`, filtro RRC de 4 símbolos por lado
evaluado en 16 fases por símbolo y `atan2` por muestra. No entra en un M33 a
30 ms por bloque ni de lejos. Nuestro `TetraDemod` es el que corre en la radio
y estas son las cifras de los dos sobre **la misma captura real** (41,7 s,
228-8889, llamada de grupo 113→100601, `TETRA IQ.wav`):

| | demod de la librería | nuestro demod |
| --- | --- | --- |
| bloques con CRC OK | 4896 (117,4/s) | 4450 (106,7/s) |
| re-alineados de la rejilla | 0 | 0 |
| slots de tráfico | 455 | 455 |
| tramas de voz | 910 | 910 |
| tramas con CRC OK | 908 (2 mal) | 908 (2 mal) |
| llamadas / CELL / NETWORK | 4 / 2 / 7 | 4 / 2 / 7 |
| sincronismo | 100 % tras el primer sync | 92 % de los bloques |
| coste en M33 | no cabe | 1,38 M instr por bloque (4,6 ms) |

Es decir: **misma llamada, mismo hablante, mismas 908 tramas de voz buenas**;
la diferencia está en los slots de señalización (91 % de los bloques que
decodifica el demodulador de host, que trabaja con la señal IQ ideal y sin
límite de tiempo). El demodulador de la captura es además el camino que la
radio ya tiene probado contra hardware (AFC, ecualizador, `CONFIG_TETRA_EQ_INIT`),
así que se queda el nuestro y el de la librería se usa solo como referencia en
el banco de host.

En una captura de aire real no hay bits de referencia, así que el BER solo se
puede medir sobre el corpus sintético (tabla BER/Es-N0 de
`reports/tetra_dmo.md`, BER 0 desde 13 dB); lo que la captura mide es lo de la
tabla: bloques con CRC correcto, FER de voz (2/910 = 0,22 %) y error de fase
RMS del demodulador (10,8 % de π/4, media de la segunda mitad de la captura).

## 4. API usada y mapeo a nuestras estructuras

### Recepción (TMO downlink)

| libtetra | dónde | para qué |
| --- | --- | --- |
| `struct tetra_tmo_rx` | `OpMode_TETRA.cpp` (estático) | contexto del receptor, 6968 B |
| `tetra_tmo_init()` | `OpMode_TETRA::resync()` | arranque y re-sincronización |
| `tetra_tmo_push_soft()` | `OpMode_TETRA::update()` | una llamada por bloque de 30 ms |
| `tetra_tmo_locked()` | `update()`, AFC, LED | hay rejilla TDMA |
| `tetra_tmo_crc_ok()` | `update()`, `tetra_getDiag()` | bloques buenos (mantiene el lock del demodulador) |
| `tetra_tmo_realigned()` | `tetra_getDiag()` | saltos de rejilla de la librería |
| `tetra_tmo_resync()` | `OpMode_TETRA::resync()` | re-sync conservando la célula |
| `tetra_tmo_set_uplink_cell()` | `OpMode_TETRA::resync()` | CC de la célula cuando se escucha su uplink (`TETRA_UPLINK_DECODE`) |
| `tetra_uplink_hz()` / `tetra_cell.ul_freq_hz` | `TetraStatus.c`, `TetraUplink.c` | frecuencia de uplink de la célula |
| `struct tetra_rx_config` | `resync()` | `promiscuous = true`: un monitor no tiene ISSI ni grupos |

No se usan (y por qué): `tetra_tmo_set_key_provider()`,
`tetra_tmo_set_cck_provider()` y todo `TETRA_ENABLE_CRYPTO` (no hay claves),
`tetra_tmo_set_slots()` (decodificamos los cuatro slots: es un monitor, no un
seguidor de una llamada), `tetra_tmo_cell()`/`tetra_tmo_network()` (los datos
llegan por eventos y se copian), `tetra_dmo_*` (no hay modo DMO en la radio) y
`tetra_sds_text_*` fuera del banco (el firmware decodifica con `SdsMsg.c`, que
usa `tetra_sds_parse_text()` en el camino del host).

### Eventos → `struct tetraStatus` (TetraStatus.c)

| evento | campos |
| --- | --- |
| `TETRA_EV_SYNC` | `sync` (y fin de llamada al perderlo) |
| `TETRA_EV_CELL` | `cell`, `mcc`, `mnc`, `colourCode`, `sysinfo`, `la`, `dlFreq`, `airEncryption` |
| `TETRA_EV_CALL` | `call`, `callId`, `callDst`, `callGroup`, `talker`, `callEncrypted` |
| `TETRA_EV_TRAFFIC` | `voice`, `talker`, `callEncrypted` + `tetraSpeech_traffic()` |
| `TETRA_EV_SDS` | `sds`, `sdsSrc`, `sdsTime` + `tetraSds_onEvent()` |
| `TETRA_EV_MM` | `registered`, `mmCause`, `groups`, `detachAll` |
| `TETRA_EV_NETWORK` | `haveNet`, `netLoad`, `neighbours`, `netTime`, `netYear`, `netUtc`, `netOffsetMin` |
| `TETRA_EV_SIGNAL` | `uplink` (ráfaga de uplink sin decodificar) |
| `ev->uplink` en CALL/SDS/TRAFFIC | `uplink`, `uplinkSs` |
| `TETRA_EV_PDU` | se ignora (monitor: no hay nada que hacer con un PDU suelto) |

`tetraStatus_onEvent()` es el único callback: la voz sale antes del mutex, el
resto se copia al estado. La UI lee con `tetra_getStatus()`; el menú rápido
tiene ahora `REG` (registro/attach), `NET` (vecinas + reloj de red) y `UL`
(SSI oído en el uplink) además de `CELL`, `LA/CC` y `CALL`.

### Escuchar el uplink de la celda (MS → BS)

El uplink viene implementado en la librería y **compilado en el firmware**:
`platform/targets/c62/CMakeLists.txt` fija `TETRA_UPLINK_DETECT=ON` y
`TETRA_UPLINK_DECODE=ON` antes de `add_subdirectory(libtetra)` (no se deja al
valor por defecto de la librería), y `cmake/tetra_config.h.in` hace que
`tetra_config.h` y el código compilado no puedan discrepar: un `-D` de fuera
con otro valor es un `#error`.

Qué reconoce, sin sincronismo con la bajada (cláusula 9.4.4 de EN 300 392-2):

| señal | cómo | qué se decodifica |
| --- | --- | --- |
| ráfaga de control de uplink (SCH/HU) | secuencia de entrenamiento **extendida x** (30 bits) | MAC-ACCESS, U-SETUP, U-TX DEMAND, SDS y estado |
| ráfaga normal de uplink (SCH/F, STCH) | secuencias **n** o **p** (22 bits) sin la **q** de un downlink continuo | señalización y tráfico (bloques de 216 bits) |
| cualquier ráfaga, sin código de color | `TETRA_UPLINK_DETECT` | solo `TETRA_EV_SIGNAL` (`uplink`, sin PDUs) |

La celda hace falta para *descodificar*: las ráfagas van aleatorizadas con el
código de color (CC) de la célula. libtetra lo aprende de una bajada recibida
antes o se le da con `tetra_tmo_set_uplink_cell()`; el modo lo siembra en cada
`resync()` con la celda de la sesión, porque al retunear a la frecuencia de
uplink se pierde el downlink del que se aprendió.

#### La sesión: fila `CELL UL` del menú rápido

`protocols/TETRA/TetraUplink.c` (C portátil, sin radio ni libtetra) guarda la
sesión, y el menú rápido del modo TETRA la maneja:

| valor | qué hace |
| --- | --- |
| `OFF` | recepción normal de la bajada; no se toca la frecuencia |
| `ON` | la radio se sintoniza en la frecuencia de uplink de la célula, la sesión queda sembrada con su MCC/MNC/CC y el receptor busca ráfagas de MS |
| `NO UL` | se pidió `ON` y no hay frecuencia de uplink conocida: **no se retunea** y la fila lo dice |

La frecuencia de uplink es la que publica la célula (`tetra_cell.ul_freq_hz`,
de SYSINFO) o, si no la publica, la conversión de duplex de su bajada
(`tetraUl_hzFromDownlink()`, tabla 2 de TS 100 392-15, la misma que aplica la
librería; el arnés de host comprueba que las dos tablas coinciden en las 768
combinaciones de banda, duplex y sentido). Nunca se inventa una: sin dato, la
fila dice `NO UL`.

`OFF` devuelve la radio a la frecuencia de la que vino. La sesión es del modo,
no un ajuste: no está en `settings_t`, no sobrevive al cambio de modo y termina
sola cuando el operador cambia de frecuencia, cuando cambia de modo (el tick
periódico de `ui.c` devuelve entonces rx **y** tx a la bajada, para que el
uplink de una célula no quede como frecuencia de transmisión de un modo que
puede keyar) y cuando el modo ve que la radio ya no está donde la sesión la
puso.

Límites, dichos:

- **Sin downlink no hay rejilla temporal.** Escuchando el uplink no se está
  sincronizado con la célula, así que no hay número de trama ni slot: los
  eventos de uplink llevan `time` a cero (lo pone la librería) y la UI no
  muestra tiempo de red para ellos. Tampoco hay forma de *comprobar* que la
  célula sigue ahí mientras se la escucha: la radio tiene un solo receptor, y
  "perder la celda" solo se detecta al volver a la bajada (Off, cambio de
  frecuencia o de modo) o si aparece un downlink de otra celda en la frecuencia
  de uplink.
- **La frecuencia hay que conocerla.** El uplink de una celda no se busca: o lo
  publica su SYSINFO, o se deduce de la bajada con la tabla de duplex. Las
  frecuencias de uplink de otras celdas no se exploran.
- **Si se apaga la radio con la sesión activa**, el VFO queda en la frecuencia
  de uplink (la sesión no se guarda). Al encender, `CELL UL` está en `OFF` y
  hay que volver a la bajada a mano o poner `ON` otra vez.
- **La voz del uplink** se decodifica igual que la de bajada (los slots de
  tráfico van con `uplink = true` y `talker_ssi` es el MS que transmite), pero
  no se ha probado con una captura real de uplink (ver §5).


### Voz

`tetra_speech_decode()` (libtetra) convierte el slot de tráfico en dos tramas
de 137 bits con BFI → `TetraSpeech.c` → OpenACELP `oa_dec_frame()`. El camino
no cambia: la librería sustituye la decodificación de canal, no el códec.

## 5. Verificación

### Tests de la librería

```bash
scripts/tetra_lib_tests.sh <libtetra-src> <build-dir>
```

Compila las suites contra el Catch2 amalgamado de `work/tetra-dmo/third_party`
(la distro no trae el paquete `Catch2`), sin tocar el `CMakeLists` de la
librería:

| suite | `05b32aa` | `6fc088c` (el del firmware) |
| --- | --- | --- |
| `coding` | 9 casos, 21 125 aserciones | igual |
| `dmo` | 3, 72 | igual |
| `sds` | 10, 76 | igual |
| `speech` | 6, 122 | igual |
| `tmo` | 14, 130 | 17, 204 (re-alineado y usage marker) |
| `tmo_l3` | 5, 82 | 7, 96 |
| `tmo_ul` | 8, 332 | igual |

60 casos, 22 027 aserciones, todas en verde con el commit que usa el firmware.
`crypto` no se compila (AGPL, ver §2); se construyó aparte con
`-DTETRA_ENABLE_CRYPTO=ON` para medir su coste (§6).

### Capturas

| captura | qué se ha hecho | resultado |
| --- | --- | --- |
| `work/tetra-dmo/captures/TETRA IQ.wav` (aire real, 48 kHz I/Q, 41,7 s) | demodulador + librería (tabla §3) y capa de estado del firmware | 4 llamadas, 455 slots, 908/910 tramas OK, red 2010 con 1 vecina, 426,7 MHz |
| `dsd-neo/tests/fixtures/iq/tetra_uplink_bursts_72k.iq` (cu8 72 kHz, 16 ráfagas, 460-4242 CC 17) | `tetra_demod -t cu8 -r 72000` + `tetra_rx -u 460:4242:17` | **16/16 PDUs de uplink**, SSI 2001..2016 en orden |
| `tetra_gen -o dibit -s 15` (escenario DMO de referencia) | `tetra_rx -m dmo` | sincronismo, canal OCCUPIED/RESERVED/FREE/SDS-OCCUPIED, CALL SETUP/INFO/CEASED/RELEASE, SDS de estado, 29 slots de tráfico |

La captura real no lleva SDS ni PDUs de MM (es una llamada de voz), así que
esas dos ramas se cubren con las suites `sds`, `tmo_l3` y `tmo_ul` y con la
prueba sintética de la capa de estado (§5.1).

### 5.1 Capa de estado sin radio

```bash
cc -O2 -I openrtx/include -I <libtetra>/include -I <build>/generated \
   tests/platform/c62_tetra_status_host.c openrtx/src/protocols/TETRA/TetraStatus.c \
   openrtx/src/protocols/TETRA/SdsMsg.c openrtx/src/protocols/TETRA/TetraDemod.c \
   <build>/libtetra.a -lpthread -lm -o c62_tetra_status_host
./c62_tetra_status_host /tmp/disc.s16
```

Es el `TetraStatus.c` del firmware (no una copia) alimentado por la cadena real
sobre la captura; imprime exactamente lo que vería la UI:

```
sync 1 cell 1 228-8889 CC 1 sysinfo 1 LA 1 dl 426700000 Hz enc 0
network 1 load 0 neighbours 1 time 1 2010+120min
network clock 16:33:28 local
call 1 id 113 dst 100601 talker 100701 group 1 enc 0 voice 1
sds 0 traffic 455 speechFrames 910
```

### 5.2 Uplink sin radio, y el demodulador acotado

```bash
scripts/tetra_host_build.sh <libtetra-src> /tmp/lt-host
scripts/tetra_host_build.sh <libtetra-src> /tmp/lt-host captura.wav   # si hay
```

Construye libtetra **con las dos opciones de uplink fijadas** y comprueba en el
`tetra_config.h` generado que están a 1 (si no, falla: un arnés con otro
`layout` que el firmware es peor que ninguno). Después corre
`tests/platform/c62_tetra_uplink_host.cpp`, que usa las unidades del firmware,
no copias:

1. **Tabla de duplex**: `tetraUl_hzFromDownlink()` contra `tetra_uplink_hz()` de
   la librería, 16 bandas × 8 códigos × 2 sentidos × 3 bajadas = **768
   combinaciones, 0 discrepancias**.
2. **Evento de celda → estado → sesión**: una célula con SYSINFO y sin uplink
   publicado se convierte (430,5 MHz → 420,5 MHz con duplex 0), una con duplex
   reservado o sin separación dice `NO UL`, y lo que publica la célula manda
   sobre la conversión.
3. **Ráfaga real de MS**: una SCH/HU de control (MAC-ACCESS + U-SETUP, con la
   secuencia extendida x) construida con `tetra_encode_sch()` +
   `tetra_tmo_build_cb()` de la librería, aleatorizada con el CC de la célula y
   empujada por el mismo receptor que usa el modo. El callback **del firmware**
   (`tetraStatus_onEvent`) acaba con `uplink = 1` y `uplinkSs = 1234567`.

Lo que **no** está verificado: una captura de uplink por el demodulador
completo. El arnés tiene el modo `-u <mcc>:<mnc>:<cc> fichero.wav|.s16` listo
para cuando haya una (48 kHz de audio de discriminador, como
`tests/platform/c62_tetra_status_host.c`), y `--audio salida.s16` sintetiza el
mismo burst como forma de onda (π/4-DQPSK con RRC 0.35, filtro de canal y
discriminador). Con esa señal sintética el demodulador de referencia en Python
(`scripts/c62_tetra_demod.py`) encuentra las secuencias de entrenamiento
(24/24) pero no baja del 17 % de error de fase, y `TetraDemod.c` no engancha:
la forma de onda sintética todavía no es equivalente a una captura de la C62
(falta el filtro de FI real y el ruido del canal), así que el test de audio
completo **queda pendiente de una captura real**. El camino de bits (§5.2.3) sí
prueba la decodificación de un burst de MS de punta a punta.

#### El lazo del demodulador, acotado

`TetraDemod.c` tenía una entrada que lo colgaba: con **silencio digital** al
principio de un bloque (códec muteado, un `underrun` de DMA, los 0,5 s que el
modo tira tras un retune) el ajuste de ganancia dividía por una desviación cero,
la ganancia salía infinita, y `wrapf()` giraba en `while (phi >= PI) phi -=
2π` para siempre — dentro de la hebra de RTX, con la radio sorda y sin que
ningún watchdog pudiera reiniciarla. Demostrado con el código anterior: 0,5 s de
ceros y después una sinusoide dejan el binario girando (`timeout` lo mata) y el
arreglado vuelve.

El arreglo es de dentro afuera, y ninguna secuencia de entrada puede ya no
terminar:

| dónde | qué |
| --- | --- |
| `wrapf()` | no finito → 0; reducción con `floorf` en O(1), sin `while` |
| `advance()` | un paso no finito se descarta (dejaba `nextInt` en `INT_MIN`) |
| ajuste de ganancia | sin desviación no se toca la ganancia (silencio), y la ganancia y el offset quedan acotados y finitos |
| vértice parabólico | acotado a ±medio símbolo y descartado si no es finito (una `den` casi cero daba correcciones de segundos) |
| lazo de símbolos | **un** símbolo por muestra interpolada y la posición nunca queda más de una ventana atrás: ni giro ni escritura fuera del buffer de soft bits |

`tests/unit/tetra_demod.cpp` (meson, `TETRA Demodulator Bounded Test`) mete
silencio, continua a fondo de escala, cuadrada, alternante, rampa, ruido y
silencio+burst, y comprueba en cada bloque que el estado sigue finito, que la
posición no se queda atrás y que no se escribe más de `3/4·n + 8` bits. Antes
del arreglo ese test no termina (meson lo mata por timeout).

**Sin regresión en la bajada**, comprobado con la captura real de §5.1: el
`TetraStatus.c` del firmware, alimentado por el demodulador arreglado, sigue
dando exactamente las cifras de la tabla (`228-8889 CC 1`, 426,7 MHz, red 2010
con 1 vecina, llamada `id 113` de `100701` a `100601`, 455 slots de tráfico,
908/910 tramas). Lo que cambió en el lazo de símbolos fue una condición de
carrera degenerada y las cotas de la ganancia: un valor acotado de más (1e-3
como mínimo) rompía justo esta captura, que necesita ~1e-5 por lo fuerte que
llega; las cotas quedan en 1e-12..1e12, que solo descartan lo que no es un
número.

## 6. Coste

### Por bloque de 30 ms (QEMU, mps2-an505, Cortex-M33, `-O2`)

```bash
scripts/tetra_rx_cost.sh dspO2 && scripts/tetra_rx_cost_parse.py build-qemu/prof-dspO2.txt
```

Dos segundos de la captura real, 60 bloques, `frame_marker()` entre el
demodulador y `push_soft`:

| etapa | mediana | p95 | máx | ms a 300 MHz (1 IPC) |
| --- | --- | --- | --- | --- |
| `TetraDemod_process()` | 1 384 511 | 1 385 590 | 1 385 898 | 4,62 |
| `tetra_tmo_push_soft()` | 175 053 | 239 043 | 239 043 | 0,58 |
| **bloque** | **1 559 564** | | | **5,20** |

**17,3 % del M33** por bloque de 30 ms a 1 IPC (con IPC real de flash/caché,
del orden del 20-25 %; por debajo del 40 % que deja el slot de DMR-TX). De ese
coste, **el 88,8 % es el demodulador** y solo el 11,2 % la librería: la parte
que aporta TMO uplink, SDS-TL, LIP, MM y MLE viene prácticamente gratis.
Antes del lock el bloque cuesta 770 k instrucciones (búsqueda de
sincronismo); la inicialización completa 14 843.

El firmware compila la librería con el `-Os` de `zephyr_interface` (que se
añade **después** de los flags del target y por tanto gana): medido, el mismo
bloque cuesta 1 870 933 instrucciones = **20,8 %** (demodulador 1 675 357,
`push_soft` 195 576). Con `-O2` se ahorran unas 310 k instrucciones por bloque
(3,5 puntos de CPU) si algún día se fijan las opciones por fichero.

Con `TETRA_ENABLE_CRYPTO=ON` (solo para medir): +8 897 B de código en M33 y
**+146 instrucciones en 120 bloques**, porque sin proveedor de claves no se
entra en el descifrado. Es "gratis" en tiempo pero no en tamaño ni en licencia,
y sin claves no descifra nada: se queda fuera.

### FLASH y RAM

`scripts/c62_tetra_sizes.sh <repo> <rama>` construye las dos configuraciones
en el mismo workspace (`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`):

| configuración | FLASH | RAM |
| --- | --- | --- |
| `master` (`44ece688`), `-DC62_TETRA=ON` | 518 104 B | 196 688 B |
| esta rama, `-DC62_TETRA=ON` (por defecto) | 518 836 B | 196 720 B |
| esta rama, `-DC62_TETRA=OFF` | 470 976 B | 170 448 B |

Las cifras de referencia del árbol (518 104 B / 196 688 B) **ya incluyen
libtetra**: es el motor que llevaba el firmware antes de este trabajo. El modo
TETRA completo (libtetra + OpenACELP + demodulador + SDS + capa de estado)
cuesta por tanto **47 860 B de flash y 26 272 B de RAM**; las tres filas nuevas
del menú rápido y los campos de MM/red/uplink añaden **732 B de flash y 32 B de
RAM** sobre `master`.

De paso: hasta esta rama `-DC62_TETRA=OFF` **no enlazaba**
(`undefined reference to _ui_printFit`, porque los ayudantes de dibujo de la UI
estaban dentro del bloque `CONFIG_TETRA` y los usan también DMR y APRS). Ahora
sí, que es lo que permite medir el coste del motor apagándolo.

#### Lo que cuesta el uplink

Medido sobre la librería sola, compilada para Cortex-M33 (`cmake
-DCMAKE_TOOLCHAIN_FILE=cmake/arm-cortex-m33.cmake`, `MinSizeRel` = `-Os`), que
es lo único que cambia con las dos opciones:

| configuración | `.text` de `libtetra.a` | `sizeof(struct tetra_tmo_rx)` |
| --- | --- | --- |
| `TETRA_UPLINK_DETECT=OFF DECODE=OFF` | 26 560 B | 6 912 B |
| `TETRA_UPLINK_DETECT=ON DECODE=ON` | 30 024 B | 6 944 B |
| **diferencia** | **+3 464 B** | **+32 B** |

Es el orden de magnitud que anuncia el README del fork («más con el decodificador
de uplink»), y va con `--gc-sections` en la imagen final, así que es una cota
superior. La pila no se ha medido aquí: el README acota «menos de 1 KB más el
callback» y el callback del firmware (`tetraStatus_onEvent`) es el mismo que ya
estaba en el camino de bajada.

## 7. Lo que se gana, lo que no se usa

Se gana, respecto a lo que había cableado:

- **MM**: registro de los móviles en la célula (`D-LOCATION-UPDATE-ACCEPT`/
  `REJECT` con causa) y attach/detach de grupos, que antes se tiraban.
- **D-NWRK-BROADCAST**: células vecinas, carga de la célula y **hora de red**
  (UTC + offset local), visible en el menú rápido como reloj local.
- **Uplink**: detección de ráfagas de uplink sin sincronizar
  (`TETRA_EV_SIGNAL`) y, con el código de color de la célula, decodificación
  de SCH/HU, SCH/F, STCH, random access, U-SETUP y U-TX DEMAND; los eventos
  llegan con `uplink = true` y la UI muestra el SSI que transmite.
- **SDS-TL y LIP** en la propia librería (texto GSM 7-bit / 8859-1 / UCS-2 y
  mensajes de localización), ya enlazados por `TetraSds.c`/`SdsMsg.c`.

No se usa, y por qué:

- **Cripto TEA1/2/3, TB5 y TA61**: AGPL y sin claves; el firmware no tiene
  dónde provisionarlas. `TETRA_ENABLE_CRYPTO=OFF` y la imagen no lleva ni un
  símbolo de cripto (§2). El coste medido está en §6 por si algún día se
  ofrece un almacén de claves.
- **Retune a otra portadora (`tetra_rx_config.retune`)**: la librería sabe
  pedir el cambio de frecuencia al seguir una llamada, pero `radio.h` no tiene
  una forma de retunear en recepción (`radio_setRxOffset` solo mueve un
  offset; `radio_updateConfiguration()` reconfigura todo el transceptor) y sin
  banco no se puede validar un salto de portadora en caliente. Queda como el
  siguiente paso natural: `radio_setRxFreq(uint32_t)`, implementación en
  `radio_C62.cpp` (`bk4819_setFrequency` + pin de LNA), aplicar el retune
  diferido entre bloques en `OpMode_TETRA::update()` y probarlo en el banco
  con una celda de dos portadoras.
- **Modo DMO**: la librería lo trae completo (verificado con el escenario de
  referencia), pero la radio no tiene modo DMO (menú, frecuencia única y
  estado); es un trabajo de UI y de modo, no del motor.
- **`tetra_tmo_set_slots()`**: decodificar solo los slots de una llamada
  ahorra CPU, pero un monitor quiere los cuatro.

## 8. Reproducir

```bash
# librería y tests
cmake -G Ninja -S modules/lib/libtetra -B build-lt-pin -DTETRA_BUILD_TESTS=OFF
ninja -C build-lt-pin
scripts/tetra_lib_tests.sh modules/lib/libtetra build-lt-pin

# captura real: demodulador de la librería (referencia) y el nuestro
build-lt-pin/tetra_demod -t wav -r 48000 -o soft "TETRA IQ.wav" cap.soft
build-lt-pin/tetra_rx -t soft cap.soft

# coste en QEMU y tamaños
scripts/tetra_rx_cost.sh dspO2
scripts/tetra_rx_cost_parse.py build-qemu/prof-dspO2.txt
scripts/c62_tetra_sizes.sh ~/src/c62/work/tetra-elk c62/tetra-elk
```

Nota sobre la captura: el fichero es I/Q en banda base, así que el audio de
discriminador para nuestro demodulador se obtiene con
`angle(c[n+1] · conj(c[n]))` y convención conjugada (la contraria a la del
demodulador de la librería: por eso `tetra_demod` necesita el símbolo
conjugado para decodificarla).
