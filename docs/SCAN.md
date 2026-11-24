# Scan y vigilancia: memorias, VFO y prioridad

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre la exploración de canales y de un rango de frecuencias: la máquina de
estados (`core/scanner.*`), el pegamento con el radio (`core/scan_service.*`),
los ajustes que la configuran y los enganches que la interfaz tiene que usar.

No toca `openrtx/src/ui/**`. Todo lo que la interfaz necesita está en la API de
`core/scan_service.h`, y la sección 9 dice exactamente qué llamar y cuándo.

---

## 1. Qué hace

Dos modos, una sola máquina:

| modo | qué recorre | de dónde salen los canales |
|---|---|---|
| `SCAN_MODE_MEMORY` | los canales del banco, en orden, saltando los marcados SKIP | `chstore_*` (`core/channel_store.h`) |
| `SCAN_MODE_VFO` | un rango de frecuencias, con un paso, una dirección y un límite | los ajustes (`scan_vfoLow`, `scan_vfoHigh`, `scan_step`) |

En los dos: se para al abrirse el squelch (o al detectar señal en digital),
aguanta lo que diga la regla de reanudación, y sigue. Opcionalmente, entre paso
y paso mira un **canal prioritario**.

Cada canal se sintoniza con su modo: un canal DMR se recibe en DMR, uno de FM en
FM, uno de TETRA en TETRA. El modo lo lleva `state.channel.mode` y el salto de
modo lo hace el propio modo del RTX, que se reinicializa al recibir la nueva
configuración (por eso los tiempos de asentamiento de la tabla de abajo).

## 2. Máquina de estados

`core/scanner.h`, implementación en `core/scanner.c`. Es **pura**: no lee reloj,
no habla con el radio y no toca flash. El llamante le pasa el tiempo y una
muestra del squelch; la máquina devuelve una acción (`scanAction_t`) con lo que
ha pasado (evento) y dónde hay que sintonizar (`target`).

| estado | qué hace | sale con |
|---|---|---|
| `IDLE` | no hay scan | `scan_start()` |
| `TUNE` | acaba de sintonizar; espera `settleMs` antes de creerse el squelch | temporizador |
| `LISTEN` | escucha el canal `dwellMs` | actividad → `SIGNAL`; temporizador → siguiente |
| `SIGNAL` | parado sobre una señal | regla de reanudación, PTT, `scan_stop()` |
| `HANG` | el portador ha caído; espera `hangMs` por si vuelve | actividad otra vez → `SIGNAL`; temporizador → siguiente |
| `PRIORITY` | escuchando el canal prioritario | actividad → `SIGNAL` (evento `PRIORITY`); `settleMs + dwellMs` → vuelve al paso interrumpido |
| `END` | fin de rango (VFO sin `loop`, banco vacío, todo SKIP) | `scan_stop()`, `scan_resume()` |

Eventos (`scan_eventName()` los nombra para un log o una pantalla):

| evento | significado |
|---|---|
| `SCAN_EVENT_STARTED` | el scan ha empezado; `target` dice dónde ha sintonizado |
| `SCAN_EVENT_TUNED` | ha pasado a otro canal / frecuencia |
| `SCAN_EVENT_SIGNAL` | squelch abierto (o sincronismo digital): el scan se para |
| `SCAN_EVENT_RESUME` | la señal ha terminado, vuelve a recorrer |
| `SCAN_EVENT_RANGE_END` | vuelta completa al banco, o final del rango de VFO |
| `SCAN_EVENT_PRIORITY` | hay actividad en el canal prioritario |
| `SCAN_EVENT_STOPPED` | el scan se acaba (operador, PTT, fin de rango) |

Un `scan_start()` sobre un banco vacío o todo SKIP devuelve
`SCAN_EVENT_RANGE_END` en vez de `STARTED`: el llamante ya sabe que ha arrancado
(la máquina está corriendo) y lo que necesita saber es por qué no ha sintonizado
nada.

Todas las esperas son `now - inicio`, comparadas en `uint32_t`: el desbordamiento
del contador de milisegundos es inocuo.

## 3. Reanudación

`scan_resume` en los ajustes (3 valores). Un solo número, `scan_hold`, cambia de
significado según la regla:

| regla | qué hace | para qué |
|---|---|---|
| `SCAN_RESUME_CARRIER` (0) | se queda mientras haya portador; cuando cae espera `hold` y sigue | escuchar una conversación entera |
| `SCAN_RESUME_TIMEOUT` (1) | se queda `hold` desde que apareció la señal y sigue aunque siga ahí | no quedarse atrapado en una portadora o un ruido |
| `SCAN_RESUME_HOLD` (2) | se queda hasta que el operador reanude | vigilancia manual |

Con `CARRIER` una conversación con pausas cortas no se corta: si el portador
vuelve antes de que expire el `hang`, la máquina sigue en `SIGNAL` y no cuenta
como una parada nueva (hay un test que lo fija: huecos más cortos que el `hang`
mantienen el scan parado, que es justo para lo que está el `hang`).

Ruido de squelch: un cambio solo se cree si se mantiene `debounceMs` (20 ms en
el firmware). Un chisporroteo de 10 ms no para el scan, y al sintonizar se borra
el detector, así que el squelch del canal anterior no se hereda.

## 4. SKIP por canal

El banco tiene un byte de flags; se añade `CHSTORE_FLAG_SKIP = 0x04`
(`core/channel_store.h`). No cambia el tamaño del registro (37 B) ni la versión
del banco: el byte ya existía.

```c
chstore_setSkipped(idx, true);   /* el editor de canales lo llama */
bool skip = chstore_skipped(idx);
```

Un canal SKIP sigue estando en el banco: se elige a mano, se edita y se borra
como cualquier otro; el scan simplemente lo pasa de largo. Un índice que el
banco rechaza se trata igual que un SKIP, así que un banco que cambie durante el
recorrido no puede atascar la máquina.

## 5. Prioridad

Un canal del banco (no una frecuencia) marcado como prioritario, con un
intervalo entre comprobaciones (`scan_prioInterval`, por defecto 2 s).

- **Cuándo se comprueba:** entre pasos, es decir cuando el scan ha terminado de
  escuchar un canal y va a ir al siguiente. Nunca durante una llamada: si hay
  una llamada en curso la máquina está en `SIGNAL` y ahí no se da ningún paso, y
  además el chequeo exige que el canal actual esté callado.
- **Cómo:** se sintoniza el prioritario, se escucha `settleMs + dwellMs`, y si no
  hay nada se vuelve al canal al que iba el recorrido — el recorrido no pierde el
  sitio (el cursor se guarda antes de desviarse).
- **Si hay actividad:** evento `SCAN_EVENT_PRIORITY` y el scan se queda ahí con
  la misma regla de reanudación. Al reanudar vuelve al canal que iba a
  sintonizar, no al prioritario.
- **Intervalo 0** significa "en cada paso". El primer paso (el que arranca el
  scan) nunca hace chequeo: el operador ha pedido un scan y el scan va al primer
  canal.
- Si el canal al que iba el recorrido es el mismo que el prioritario, no se
  duplica la sintonía.

Un solo receptor, así que la prioridad es **interrumpida**: durante el chequeo no
se oye el canal del recorrido. Es el compromiso de un BK4819, y con el intervalo
por defecto (2 s) el tiempo robado a la escucha es del orden del 10 %.

## 6. Scan de VFO

| paso | Hz | nota |
|---|---|---|
| 5 kHz | 5000 | |
| 6,25 kHz | 6250 | |
| 8,33 kHz | **8330** | 25000/3 no es entero; 8330 Hz es el redondeo habitual (4 Hz de error, por debajo de la precisión de referencia del radio) |
| 10 kHz | 10000 | |
| 12,5 kHz | 12500 | por defecto |
| 25 kHz | 25000 | |

La tabla vive en `settings.c` (`scan_steps[]`), porque su índice se guarda en
flash: solo se puede añadir al final.

- Límites `scan_vfoLow`/`scan_vfoHigh` en Hz, inclusivos. Si el VFO está fuera
  del rango, el primer paso entra por el borde que toca según la dirección.
- Con `loop` (por defecto) al llegar al límite vuelve al otro extremo y emite
  `RANGE_END`; sin `loop` se queda en el límite en `END` (y el operador puede
  pararlo, o reanudar para reintentar).
- Si el rango es más estrecho que el paso, se usa el paso más pequeño (5 kHz).
- Sintonizar la VFO no cambia el desplazamiento TX/RX que tuviera: si el canal
  VFO tenía −600 kHz, el scan mantiene −600 kHz en todo el recorrido.

## 7. Ajustes persistidos

En `settings_t` (`core/settings.h`), **versión 4** del registro. Son 15 bytes, y
el registro pasa de 175 B a 190 B con APRS (el límite del driver es 256 B).
Las unidades son gruesas a propósito: un menú enseña segundos, y el registro es
el espacio más escaso del firmware.

| campo | unidad | rango | por defecto | significado |
|---|---|---|---|---|
| `scan_resume` | enum | 0..2 | 0 (portador) | regla de reanudación |
| `scan_priority` | índice+1 | 0..128 | 0 (ninguno) | canal prioritario |
| `scan_step` | índice | 0..5 | 4 (12,5 kHz) | paso de VFO |
| `scan_dwell` | 10 ms | 1..255 | 15 (150 ms) | tiempo por canal |
| `scan_hold` | 100 ms | 1..255 | 20 (2 s) | espera antes de reanudar / tiempo en una señal |
| `scan_prioInterval` | 100 ms | 0..255 | 20 (2 s) | entre chequeos de prioridad |
| `scan_flags` | bits | | `VFO_LOOP` | `VFO_LOOP` (0x01), `PRIORITY` (0x02), `DOWN` (0x04) |
| `scan_vfoLow` | Hz | cobertura del BK4819 | 144 MHz | límite inferior |
| `scan_vfoHigh` | Hz | cobertura del BK4819 | 146 MHz | límite superior |

Los valores que llegan de flash o de un menú se sanean al construir la
configuración (`scan_service.c`): un paso o una regla fuera de rango vuelven al
valor por defecto, un canal prioritario que ya no existe se ignora, y un rango
fuera de cobertura o vacío vuelve a 144-146 MHz. Nunca se rechaza el scan por un
ajuste raro.

**Al actualizar el firmware, los ajustes se reinician** (la versión del registro
cambia de 3 a 4). Es lo esperado y lo que ya pasa con cada campo nuevo; ver
`docs/SETTINGS.md`.

## 8. Coste

Medido en el host (`tests/unit/scanner.cpp`, 200 000 muestras): **~17-55 ns por
muestra de 5 ms** con un banco de 128 canales, es decir, escuchar es
prácticamente gratis: en `LISTEN` la llamada solo compara dos tiempos.

En el firmware, cada paso lee el banco dos veces (el candidato y el que decide),
y durante la escucha no lee nada. `scan_service_poll()` se llama 200 veces por
segundo desde `state_task()` y cuesta un test de bandera cuando no hay scan.

Medida en el binario de la C62 (`c62-build -DCONFIG_C62_DMR_RX=y
-DCONFIG_C62_APRS=y`, mapa de regiones del enlazador):

| | master `44ece688` | con scan | diferencia |
|---|---|---|---|
| FLASH | 518 104 B | 520 320 B | **+2 216 B** (0,21 % de 1 MiB) |
| RAM | 196 688 B | 196 984 B | **+296 B** (0,11 % de 256 KiB) |
| PSRAM | 379 064 B | 379 064 B | 0 |

Por fichero (`arm-zephyr-eabi-size`): `scanner.c` 1 552 B de código y datos
(incluidas las tablas de nombres de estado y evento), `scan_service.c` 1 818 B
de código y 271 B de `.bss` (la máquina, la cola de 8 eventos y el estado de
casa). El resto del delta son la tabla de pasos y los nombres en `settings.c`,
los dos ayudantes de SKIP en `channel_store.c` y las llamadas de `state.c`.

La RAM es lo que hay que vigilar (el build de DMR-TX está al 75 %): el bloque
de ajustes añade 15 B a `settings_t` y el servicio 271 B, ninguno de los cuales
crece con el número de canales — la cola de eventos está acotada a 8 y descarta
el más viejo.

## 9. Integración: qué llama a qué

Ficheros:

| fichero | papel |
|---|---|
| `core/scanner.h/.c` | la máquina de estados, sin dependencias |
| `core/scan_service.h/.c` | squelch, banco, ajustes, retune, cola de eventos |
| `core/state.c` | `scan_service_init()` en `state_init()`, `scan_service_poll()` al principio de `state_task()` (5 ms), `scan_service_stop()` en `state_terminate()` |
| `core/channel_store.h/.c` | flag SKIP y `chstore_setSkipped()` |
| `core/settings.h/.c` | los ajustes y la tabla de pasos |
| `tests/unit/scanner.cpp` | los tests de host con receptor simulado |

**Cadencia.** `state_task()` corre cada 5 ms desde el hilo principal; el poll va
antes del filtro de 100 ms para que los tiempos del scan sean de milisegundos y
las sintonías caigan entre dos bloques de audio.

**Retune.** La máquina no habla con el RTX: `scan_service.c` escribe
`state.channel` (canal, frecuencia, y el destino DMR si el canal es DMR) y pone
`ui_rtxSyncRequest = true`, que es la puerta que ya usan el menú rápido y el
control remoto. El hilo de la interfaz arma la configuración del RTX desde
`state.channel` en su siguiente vuelta y el modo se reinicializa solo. Así no hay
una segunda copia de las reglas que deciden qué significa un canal.

**PTT.** El servicio mira `rtx_getCurrentStatus().opStatus`; si está en `TX`, el
scan se para en el acto (`SCAN_EVENT_STOPPED`). Si estaba parado sobre una señal,
ese canal pasa a ser el canal de casa (es donde el operador quiere hablar); si
estaba recorriendo, vuelve al canal desde el que arrancó. La vuelta se **difiere**
hasta que termina la transmisión: reconfigurar el RTX en medio de una llamada la
cortaría, y eso es lo único que un scan no puede hacer. `scan_service_poll()`
sigue corriendo y hace la vuelta en el primer tick después del PTT.

**Apagado.** `state_terminate()` para el scan antes de guardar VFO y ajustes, de
modo que no se guarde como canal del operador el canal por el que iba el scan.

**Detección de señal.** `rtx_rxSquelchOpen()`, que es el squelch analógico en FM,
el sincronismo de DMR (`dmrPhy_synced`) y el LSF válido en M17. TETRA no publica
detector de portadora (`rxSquelchOpen()` devuelve siempre `false`), así que en
ese modo se usa el RSSI contra el nivel de squelch del operador: es una
aproximación —no comprueba el canal— y por eso un scan de TETRA puede pararse en
ruido tan fuerte como una llamada. Está dicho aquí y en el código.

### Enganches de la interfaz (lo que falta en `openrtx/src/ui/**`)

El módulo no añade teclas ni pantallas. Lo que la interfaz tiene que hacer:

| gancho | llamada | cuándo |
|---|---|---|
| arrancar el scan de memorias | `scan_service_start(SCAN_MODE_MEMORY)` | tecla del operador (p. ej. P1 largo en la pantalla de canal) |
| arrancar el scan de VFO | `scan_service_start(SCAN_MODE_VFO)` | tecla del operador en la pantalla de VFO |
| parar | `scan_service_stop()` | la misma tecla, `KEY_ENTER`, `KEY_ESC`, o antes de cualquier sintonía manual / cambio de canal / cambio de modo |
| pintar el estado | `scan_service_status(&st)` y `scan_service_stateName()` | en `ui_updateGUI()`: `st.running`, `st.state`, `st.channel`, `st.frequency`, `st.visited`, `st.stops` |
| avisos | `while (scan_service_popEvent(&ev)) ...` | en `ui_updateFSM()`: "parado", "reanudando", "fin de rango", "prioridad" |
| marcar SKIP | `chstore_setSkipped(idx, on)` | en el editor de canales (P2 o el menú del canal) |
| indicar "scan" en pantalla | `state.tuner_mode` (`SCAN`, `CHSCAN`) | el servicio ya lo pone y lo restaura al parar; la UI puede usarlo en vez del nombre del canal |
| silenciar mientras recorre (opcional) | `rtx_cfg.scan = 1` en `threads.c` | el bit existe en `rtxStatus_t` y nadie lo usa: es el sitio natural si se quiere mute durante el recorrido |

Reglas para la UI:

1. Antes de tocar `state.channel` por una acción del operador, `scan_service_stop()`.
   Si no, el scan volverá a sintonizar en el tick siguiente.
2. No hace falta gestionar el PTT: el servicio lo ve y para el scan solo.
3. `scan_service_start()` devuelve `false` si ya hay un scan, si el banco está
   vacío o si el radio está transmitiendo: la UI puede avisar con un pitido.
4. El ajuste de "canal prioritario" se escribe en `state.settings.scan_priority`
   (índice+1) y `SETTINGS_SCAN_PRIORITY` en `scan_flags`: el servicio relee los
   ajustes en cada arranque, así que un cambio se aplica al siguiente scan.

## 10. Tests

`tests/unit/scanner.cpp` (host, Catch2, sin radio): el receptor es un modelo de
señal —una función de (canal, frecuencia, tiempo)— y el banco, un vector con
flags SKIP.

| test | qué fija |
|---|---|
| *visits every channel, in order, and laps* | orden, vuelta completa, `RANGE_END` por vuelta |
| *SKIP is passed over, not deleted* | los canales SKIP no se visitan y siguen en el banco |
| *an index the bank refuses* | un índice que falla no atasca el recorrido |
| *a bank with nothing to walk* | `END` + `RANGE_END`, sin sintonías, y se puede parar |
| *a signal stops the scan…* | parada por squelch, espera, reanudación y "no caza" durante el portador |
| *squelch chatter…* | el rebote de 10 ms no para el scan |
| *carrier vs timeout* | las dos reglas, incluida la de abandonar una llamada larga |
| *hold waits for the operator* | `HOLD` hasta `scan_resume()` |
| *priority checked between steps* | el prioritario aparece entre canales y el recorrido sigue donde iba |
| *a call on the priority channel* | evento `PRIORITY` y reanudación al canal interrumpido |
| *never interrupts a call in progress* | 3 s de llamada con intervalo de 50 ms: cero sintonías |
| *PTT ends the scan from any state* | las cinco estados de la tabla, `HOME` pedido, máquina quieta y reiniciable |
| *VFO walks up and wraps / without loop / down / parked outside* | el recorrido de VFO completo |
| *every step of the table* | los seis pasos producen su rejilla exacta |
| *no state is a trap* | con patrones de portador que se van, siempre hay un paso dentro del presupuesto |
| *gaps shorter than the hang* | el `hang` mantiene el scan parado (comportamiento intencionado) |
| *a stopped machine costs nothing* | cero lecturas del banco parado, una por paso andando |
| *the cost of a sample* | nanosegundos por muestra |

## 11. Lo que no hace

- **No hay doble receptor**: la prioridad es interrumpida (sección 5).
- **TETRA** se detecta por RSSI, no por canal (sección 9).
- **No hay "zona" ni "lista de scan"**: recorre el banco entero. Una lista por
  zona es un cambio de datos en el banco, no de la máquina: `scanList_t` es un
  `count`/`info`, así que otra fuente de canales entra sin tocar `scanner.c`.
- **No hay parada por tono (CTCSS/DCS) como criterio propio**: el squelch del
  modo ya la aplica en FM, así que un canal con tono no se para con otro tono.
- **No hay "scan de canales cercanos" ni barrido espectral**: es un receptor de
  canal, no un analizador.
