# Búsqueda de portadoras en VFO

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre la búsqueda de portadoras por pulsación larga en VFO: la máquina de
estados (`core/vfo_search.*`), el pegamento con el radio
(`core/vfo_search_service.*`), el criterio de parada de cada modo, la
corrección a la frecuencia de la red y los enganches de la interfaz.

No toca ningún modo, ni el scan de memorias. Lo que la interfaz necesita está
en la API de `core/vfo_search_service.h`.

---

## 1. Qué hace y en qué se diferencia del scan

En la pantalla principal y en **modo VFO**, mantener pulsado **ARRIBA o ABAJO
durante tres segundos** arranca un barrido de portadoras desde la frecuencia
actual, con el paso del modo: hacia arriba si es ARRIBA, hacia abajo si es
ABAJO. La fila `VFO scan` del menú de herramientas (P1 mantenido,
`docs/UI.md`) arranca el mismo barrido hacia arriba sin esperar los tres
segundos, y desde la pantalla de memoria devuelve primero la VFO. El barrido se para en la primera portadora que cree, y **se queda ahí**:
no vuelve a barrer, no hay hold con temporizador, no hay reanudación.

Es un barrido puntual del VFO, no un scan. La diferencia con
`core/scanner.*` (`docs/SCAN.md`) no es de detalle:

| | scan (`core/scanner.*`) | búsqueda (`core/vfo_search.*`) |
|---|---|---|
| qué recorre | los canales del banco, o un rango de VFO de los ajustes | el VFO, desde la frecuencia en uso, dentro de su banda |
| qué hace al encontrar | se para y aplica una regla de reanudación (portadora, tiempo, manual) y sigue | se para **para siempre**: el receptor se queda ahí |
| criterio digital | el `rxSquelchOpen()` del modo (un sync) | sync **con tramas detrás** (CRC/paridad), estable |
| frecuencia | la del canal | puede corregirse a la que publica la red |
| fin | vuelta al banco / fin de rango | vuelta al inicio del rango y fin, o sigue con la vuelta |
| quién lo lanza | un menú / una tecla | pulsación larga de 3 s de una flecha (o la fila `VFO scan` del menú de herramientas) |

Se decidió una máquina aparte, y no un tercer modo de reanudación dentro del
scanner, por tres razones: el scanner camina una **lista** (y aquí no hay
lista), su estado `SIGNAL` existe para decidir cuándo *seguir* (y aquí no se
sigue nunca), y el criterio digital y la corrección de frecuencia son estados
nuevos que no tienen sentido en un scan de memorias. Comparte lo que sí es
común: el detector de squelch/sincronismo del modo, y el retune
(`state.channel` + `ui_rtxSyncRequest`).

## 2. Máquina de estados

`core/vfo_search.c`. Es **pura**: no lee reloj, no habla con el radio, no toca
flash. El llamante le pasa el tiempo y una muestra del receptor
(`vfoSearchSample_t`); la máquina devuelve una acción
(`vfoSearchAction_t`) con el evento y, si hay que sintonizar, la frecuencia.

| estado | qué hace | sale con |
|---|---|---|
| `VFOS_STATE_IDLE` | no hay búsqueda | `vfoSearch_start()` |
| `VFOS_STATE_LISTEN` | escucha la frecuencia `dwellMs` (mínimo `syncMs + syncHoldMs` en digital) | portadora → `FOUND`; sincronismo → `FOUND`; temporizador → `TUNE` con la siguiente |
| `VFOS_STATE_TUNE` | acaba de sintonizar; espera `settleMs` antes de creerse al receptor | temporizador → `LISTEN` (y ahí se toma la base del contador de tramas) |
| `VFOS_STATE_FOUND` | parada sobre una portadora: **se queda**, ya no se mueve | nada (el operador sintoniza a mano) |
| `VFOS_STATE_END` | una vuelta completa a la banda sin nada | nada |

Eventos (`vfoSearch_eventName()`):

| evento | significado |
|---|---|
| `VFOS_EV_STARTED` | ha empezado en la frecuencia en uso (sin retune: ya está ahí) |
| `VFOS_EV_TUNED` | ha pasado a otra frecuencia (`target = FREQUENCY`) |
| `VFOS_EV_FOUND` | portadora o sincronismo: se acabó; `result` dice cuál y `corrected` si se ha corregido la frecuencia |
| `VFOS_EV_CANCELLED` | lo ha parado el operador; el receptor se queda donde estaba |
| `VFOS_EV_BAND_END` | vuelta completa sin nada: el receptor vuelve a la frecuencia inicial |

Resultados (`vfoSearch_resultName()`), que es lo que la pantalla escribe:
`carrier`, `sync`, `cancelled`, `band-end`, y con la corrección aplicada
`net` (ver §4).

Detalles que importan:

- **Arranque**: empieza en `LISTEN` sobre la frecuencia actual, porque el
  receptor ya está asentado ahí; volver a sintonizar la misma frecuencia solo
  reinicializaría el modo. El contador de tramas que traiga la muestra es la
  **base**: solo cuentan las tramas que lleguen a partir de ahora (si no,
  las que decodificó el modo en otra frecuencia pararían el barrido en falso).
- **Vuelta de banda**: al pasar del límite superior (o inferior) vuelve al otro
  extremo y sigue. La vuelta se considera completa cuando el barrido **pasa
  otra vez por la frecuencia en la que empezó**; ahí termina con
  `VFOS_EV_BAND_END` y devuelve el receptor al inicio. Una banda de una sola
  frecuencia termina en el primer paso.
- **Todas las esperas** son `now - inicio` en `uint32_t`: el desbordamiento del
  contador de milisegundos es inocuo.
- **El dwell no puede cortar la prueba**: en digital el dwell efectivo es
  `max(dwellMs, syncMs + syncHoldMs)`. Sin eso, una configuración con dwell
  corto barrería la banda sin poder pararse nunca en ella, que es una forma
  silenciosa de dejar la búsqueda inservible.

## 3. Criterio de parada por modo

| modo | qué se mira | criterio |
|---|---|---|
| FM (`OPMODE_FM`) | squelch analógico (`rtx_rxSquelchOpen()`) | abierto y estable `debounceMs` (20 ms) |
| AM aéreo (`OPMODE_AIRBAND`) | portadora sobre umbral (`rxSquelchOpen()`) | ídem |
| FM comercial (`OPMODE_WFM_BC`) | canal válido del BK1080 (`rxSquelchOpen()`) | ídem (paso 100 kHz) |
| DMR II/III | `dmr_getStatus()`: `sync` + `bursts` | `bursts` crece ≥ 2 con `sync` durante ≥ 100 ms |
| NXDN | `nxdn_getStatus()`: `sync` + `frames - lichErrors` | ≥ 2 tramas con LICH válido, sync ≥ 100 ms |
| P25 fase 1 | `p25Status_get()`: `sync` + `frames` | ≥ 2 tramas, sync ≥ 100 ms |
| TETRA | `tetra_getStatus().sync` + `tetra_getDiag().crcOk` | ≥ 2 bloques con CRC-16 bueno, sync ≥ 100 ms |
| M17 | `rxSquelchOpen()` (`dataValid`) | sync ininterrumpido `syncMs` (400 ms): no publica contador |
| YSF | `ysfPhy_synced()` vía `rxSquelchOpen()` | ídem |
| dPMR, D-STAR | `rxSquelchOpen()` del modo | ídem (sin contador en este árbol) |

Los contadores salen de `rtx/ModeReport.c` (`modeSyncCounters()`), que lee los
módulos de estado de cada protocolo. Son **monótonos** durante todo el
encendido (los módulos los conservan a propósito al re-sintonizar) y dan la
vuelta a 2³²: la máquina compara contra una base tomada al empezar a escuchar
esa frecuencia y, si el valor baja (el modo se reinicializó), vuelve a tomar la
base.

Por qué 400 ms en los modos sin contador: `dmrStatus`, `nxdnStatus` y
`p25Status` mantienen `sync` durante 300 ms después de la última trama, y
`ysfPhy_synced()` durante dos tramas (~200 ms). Con 400 ms, una portadora que
se acaba de dejar atrás **no** puede parar el barrido en la frecuencia
siguiente; un sincronismo real se vuelve a enganchar y sigue ahí.

Por qué dos tramas y 100 ms: un sincronismo suelto en ruido es el falso
positivo típico; dos tramas con su CRC/paridad detrás y 100 ms de estabilidad
es lo que pide el criterio de "no un falso positivo".

## 4. Corrección a la frecuencia real de la red

Al parar sobre una portadora, la máquina pregunta al modo por la frecuencia
real. El hook genérico es `modeReportedFrequency()` (`rtx/ModeReport.h`), y
tiene dos respuestas posibles:

1. **frecuencia absoluta publicada** (`absolute` + `frequency`): se retuna ahí.
   Es lo que hace **TETRA**: el SYSINFO del SCH/F y del SCH-HD trae la portadora
   principal de bajada de la célula (`tetraStatus.dlFreq`), que es exactamente
   la frecuencia en la que emite la red. Se acepta si está a menos de 25 kHz de
   la sintonía actual (si no, es otro canal, no una corrección).
2. **error de portadora medido** (`offsetValid` + `offsetHz`): se retuna a
   `frecuencia + offset`. Es el `portadora %+d Hz` de los logs de TETRA
   (`tetraDemod_carrierOffset()`), el mismo número con el que ya converge el
   AFC del modo. Solo se aplica entre 200 Hz (por debajo es ruido de medida) y
   4 kHz (el rango del AFC), y solo si la frecuencia corregida sigue dentro de
   la banda.

La corrección se aplica **una vez**, con un retune más (`target =
FREQUENCY`), y el mensaje de la pantalla la muestra: `net 145.01250 -250`
(offset en unidades de 10 Hz, como el resto de la pantalla). Después, el AFC
del modo sigue afinando por su cuenta: la corrección de la búsqueda solo le
ahorra el transitorio.

Aviso de escala: en TETRA la lectura del demodulador es una **fracción** del
error real (la cadena de recepción elimina la mayor parte de la continua del
discriminador, por eso el AFC del modo usa ganancia 3 e itera). La búsqueda
aplica el valor tal cual, que es lo que pide el operador y lo que deja el
receptor más cerca; el AFC hace el resto.

**Qué modos no publican su frecuencia** y por qué:

| modo | publica | por qué no |
|---|---|---|
| DMR Tier II / III | no | el split del repetidor y el canal de tráfico son configuración, no interfaz aérea |
| NXDN | no | ídem: la frecuencia del canal no viaja en la trama |
| dPMR | no | ídem |
| P25 fase 1 | no | el NAC y el plan de canales son configuración; el TSBK de voz no lleva la portadora |
| YSF | no | el reflector y el canal son configuración |
| D-STAR | no | el repetidor y el módulo son configuración |
| M17 | no | el LSF lleva indicativo y tipo de paquete, no frecuencia |
| FM / AM / WFM | no | no hay capa de datos que la lleve |

Añadir un modo que sí la publique (TETRA DMO con su propia portadora, un canal
de control troncal que nombre su canal de tráfico) es **un `case` más en
`ModeReport.c`** y nada más: la máquina ya acepta la respuesta.

## 5. Cancelación

Paran la búsqueda y dejan el receptor **en lo último sintonizado**:

- otra pulsación larga de ARRIBA/ABAJO (o de cualquier tecla),
- cualquier tecla corta (incluido ROJO/ESC y MONI),
- el PTT.

Las teclas las consume el enganche de `ui.c`: la tecla que para **no hace nada
más** (no abre el menú, no sintoniza un paso, no cambia de modo). El PTT lo ve
el servicio desde `state_task()`; si hay una transmisión, para el barrido y no
toca la configuración de la llamada en curso.

Al parar, el firmware queda en **RX limpio**: cada frecuencia que visita la
búsqueda es una configuración completa del RTX (`state.channel` +
`ui_rtxSyncRequest`, la misma puerta que el menú rápido y el scan), así que no
hay streams abiertos ni perfiles a medias que recoger. La única excepción es el
retune que se deba en mitad de una transmisión: se aplica al terminar.

## 6. Interfaz

- **Enganche de teclado** (`openrtx/src/ui/default/ui.c`, caso `MAIN_VFO`):
  una pulsación larga de ARRIBA/ABAJO **no** da un paso de sintonía; arma la
  búsqueda. El evento de pulsación larga llega a los 700 ms
  (`input_longPressTimeout`), así que ese evento marca el instante de la
  pulsación y `_ui_vfoSearchTick()` arranca el barrido cuando la tecla lleva
  **3 s** sujeta, comprobando con `kbd_getKeys()` que sigue pulsada. Si se
  suelta antes, el armado se descarta en silencio.
- **Cualquier tecla** con la búsqueda en marcha la para (ver §5).
- **Pantalla** (`ui_main.c`, `_ui_drawMainBottom()`): mientras busca, la fila
  inferior (la del S-meter, la misma que usan los hints en los menús) muestra
  dirección, frecuencia y porcentaje de banda recorrido — `up 145.51250 12%`.
  Al parar, durante 2,5 s (`VFO_SEARCH_MESSAGE_MS`) muestra el motivo:
  `carrier 145.51250`, `sync 145.52500`, `net 145.01250 -250`,
  `cancel 145.48750`, `band end 145.50000`. Después vuelve el S-meter.
- En **modo no-VFO** (pantalla de canal, menús, entrada de frecuencia) la
  pulsación larga **no arranca nada**: el enganche está dentro del caso
  `MAIN_VFO` y el servicio lo vuelve a comprobar con
  `vfoSearch_mayStart(pantalla VFO, transmitiendo, scan en marcha)`.
- No se arranca una búsqueda con un **scan de memorias** en marcha (los dos
  quieren el receptor; el scan se para con su propia tecla).

## 7. Parámetros

Todo son constantes en `core/vfo_search_service.c` (no hay ajustes nuevos, no
gasta NVS):

| constante | valor | para qué |
|---|---|---|
| `VFO_SEARCH_SETTLE_MS` | 100 | que el hilo de UI recoja el retune y el modo se reinicialice |
| `VFO_SEARCH_DWELL_ANALOG_MS` | 120 | escucha por frecuencia en analógico |
| `VFO_SEARCH_DWELL_DIGITAL_MS` | 600 | escucha por frecuencia en digital |
| `VFO_SEARCH_DWELL_TETRA_MS` | 2500 | TETRA tira 0,5 s de muestras tras un retune y necesita un burst para bloquear |
| `VFO_SEARCH_DEBOUNCE_MS` | 20 | estabilidad del squelch analógico |
| `VFO_SEARCH_SYNC_FRAMES` | 2 | tramas válidas que hacen una portadora digital |
| `VFO_SEARCH_SYNC_HOLD_MS` | 100 | estabilidad exigida a esas tramas |
| `VFO_SEARCH_SYNC_MS` | 400 | sincronismo para un modo sin contador |
| `VFO_SEARCH_OFFSET_MIN/MAX` | 200 / 4000 Hz | corrección por error de portadora |
| `VFO_SEARCH_REPORTED_MAX` | 25 kHz | ventana de la frecuencia publicada |

Bandas que recorre (`bandLimits()`): 144-146 MHz, 430-440 MHz, 446.0-446.2 MHz,
1240-1300 MHz, la banda aérea (118.000-136.975, `core/airband.h`) y la de FM
comercial (85.0-108.0, `core/wfm.h`); cualquier otra frecuencia recorre la
sub-banda del receptor que la contiene (VHF 18-660 MHz o UHF 840-1300 MHz, lo
que abre `CONFIG_C62_UNLOCKED_TUNING`). En el hueco 660-840 MHz no hay barrido
posible y `vfoSearchService_start()` devuelve false.

## 8. Coste

- Máquina: puro C, estado de ~60 B. Medido en el host: **7 ns por muestra**
  (200 muestras/s en el radio, es decir nada). Con la búsqueda parada, el
  servicio es un `if` por tic de 5 ms.
- Imagen, misma receta que la rama (`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y
  -DCONFIG_DMR_TX=y`) sobre `75836b56`:

| | FLASH | RAM | ITCM | PSRAM |
|---|---|---|---|---|
| sin la búsqueda | 590 244 B (56.29 %) | 210 160 B (80.17 %) | 8 468 B | 379 064 B |
| con la búsqueda | 593 984 B (56.65 %) | 210 280 B (80.22 %) | 8 468 B | 379 064 B |
| **coste** | **+3 740 B** | **+120 B** | 0 | 0 |

  Los +120 B son el estado de la máquina y los contadores del servicio; no hay
  buffers nuevos (`ModeReport.c` lee estructuras de estado que ya existen).
  Con todos los modos de RX encendidos además (`CONFIG_C62_NXDN_RX`,
  `CONFIG_C62_P25`, `CONFIG_C62_YSF`): 621 704 B de flash (59.29 %) y
  226 952 B de RAM (86.58 %), que es el presupuesto de esa configuración, no
  el de esta función.

## 9. Pruebas

`tests/unit/vfo_search.cpp` (host, Catch2, `meson test 'VFO Search Test'`),
22 casos: arranque por pantalla/modo, paso arriba y abajo con el paso del modo,
parada por squelch y que **no** se mueve después, chatter que no la para,
parada por sincronismo sintético con tramas, sincronismo solo (sin tramas) que
no la para, modo sin contador que para por tiempo, rebase del contador,
corrección por frecuencia publicada y por offset (y los cuatro rechazos:
umbral, máximo, fuera de ventana, fuera de banda), cancelación en `LISTEN` y en
`TUNE`, doble cancelación, reinicio tras cancelar, vuelta de banda completa con
regreso al inicio, vuelta en los dos extremos, banda de una frecuencia,
configuración inválida y no modificable en marcha, y el coste por muestra.
