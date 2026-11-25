# La memoria de los modos: un bloque, un modo

Los once modos del C62 tienen su estado en un solo bloque de SRAM, del tamaño
del modo más grande, y cada uno lo toma al entrar y lo devuelve al salir. Antes
cada modo tenía el suyo en `.bss` desde el arranque, aunque solo uno funcione.

Con todo encendido (once modos, TX de DMR/NXDN/dPMR/P25/YSF, APRS, KISS,
protocolo PC con el espacio de depuración, AM de banda aérea) la RAM pasa de
**209416 B (79,89 %)** a **177192 B (67,59 %)**: 32224 B menos, 6308 B de margen
bajo el 70 %.

El código está en `openrtx/include/rtx/ModeArena.hpp`,
`openrtx/src/rtx/ModeArena.cpp` (el bloque) y `openrtx/src/rtx/rtx.cpp` (el
registro de modos y el cambio de modo). Los tests, en
`tests/unit/mode_arena.cpp` (el bloque, con modos de mentira) y
`tests/unit/mode_switch.cpp` (el cambio real, por `rtx_configure()`/`rtx_task()`).

---

## 1. Inventario

Medido sobre la imagen de todos los modos con TX (build `build_full` de
`c62-ws-mega`, commit `86c3b7dc`), con el mapa del linker
(`zephyr.map`, secciones de entrada de `.bss`/`.data`/`.noinit`/`.psram*`)
cruzado con `arm-none-eabi-nm -S`. Regiones: SRAM `0x80000+256 KB`, DTCM
`0x20000000+16 KB`, PSRAM `0x30200000+1792 KB`.

Las cuatro clases que separa el diseño:

- **muestra**: se toca dentro del lazo de 48 kHz. Se queda en SRAM o DTCM;
  nunca va a PSRAM.
- **trama**: se lee y escribe una vez por bloque de 30 ms, por ráfaga o por
  trama. Puede ir a PSRAM.
- **solo activo**: solo existe mientras el modo está en el aire. Es el
  candidato a la arena.
- **DMA**: lo escribe o lo lee el motor de audio/SPI. No se mueve ni a PSRAM ni
  a otro sitio con otra alineación.

### Estado de cada modo

| modo | objeto (B) | estáticos del modo en SRAM (B) | clase | dónde acaba |
|---|---:|---|---|---|
| FM | 16 | `aprsTxBuf` 3840 | objeto: solo activo; `aprsTxBuf`: **DMA** | objeto a la arena, `aprsTxBuf` se queda |
| AM banda aérea | 28 | `air_tx.c`: `mod` 140 + `ring` 128 | objeto: solo activo; `ring`: **DMA** | objeto a la arena |
| WFM | 16 | `wfm.c` 27 | solo activo | arena |
| M17 | 1864 | tablas RRC 656+336 | objeto: por trama (filtro) | arena |
| TETRA | 28 | `baseband` 5760 + `demod` 4944 + `tmo` 6944 + varios | `baseband`: **DMA**; `demod`: **muestra**; `tmo`: trama; `soft` 1088 ya en PSRAM | **todo al objeto** (arena) |
| DMR | 13112 | `dmr_fec` 512+256, `dmr_demod` 648+328+328+324, `DmrSpeech.pcm` 1920 | `rxSamples`/`txSamples`/`micPcm`: **DMA** y muestra; paquete: trama | arena (es el mayor de los objetos "pequeños") |
| NXDN | 5716 | `baseband` 3840 | **DMA** | objeto a la arena, `baseband` se queda |
| dPMR | 5664 | `DpmrPhy.phy` 752 | trama | arena |
| P25 | 32 | `P25Phy` 716, `P25Tx.mod` 400, `P25Fec` 193 | trama | arena (`P25Phy` tiene 37952 B en PSRAM) |
| D-STAR | 5788 | `DStarSpeech.pcm` 640, `DStarStatus` 309 | `pcm`: DMA; resto trama | arena |
| YSF | 40 | `YsfPhy.phy` 508, `YsfTx.mod` 400 | trama | arena (29476 B de tablas ya en PSRAM) |
| ninguno | 4 | — | — | se queda (nunca se destruye) |

Suma de los objetos antes (con `noMode`, que no se destruye nunca): **32308 B**. Con los estáticos de TETRA (17691 B entre
`baseband`, `demod` y `tmo`) y el `baseband` de NXDN (3840 B), el estado de los
modos ocupaba **53839 B** de SRAM de forma permanente.

### Subsistemas que no son modos

| subsistema | SRAM | PSRAM | clase |
|---|---:|---:|---|
| APRS (`core/aprs_service.c`) | `svc` 2868 | 5592 (estaciones + buzón) | `svc` lleva el bloque de 20 ms de `audioStream_start()`: **DMA**. El resto, trama |
| KISS / módem de paquetes | pila del hilo 3072 | `svc` 5840 | `svc` ya en PSRAM; la pila no se mueve |
| protocolo PC (`pc_proto_c62.c`) | 4096 + 208 | 33676 (con CONFIG_C62_DEBUG_PROTO) | buffers de transporte y pila: **DMA**/pila |
| analizador de espectro | — | 680 | trama |
| voz / avisos | 272 | 2424 | trama |
| almacenes (canales, contactos, mensajes) | 196 | 4736+1060+4852 | trama |
| `graphics.c` framebuffer | **40960** | (`display_c62.c` tiene otros 40960) | **DMA** del panel ST7735R |
| heap del sistema | 40960 (`noinit`) | — | asignador |
| cliente UART-RPC | 8230 | — | pila + colas de la HAL |

Los dos bloques de 40960 B del framebuffer y del heap son, con diferencia, lo
más grande que queda en SRAM, y ninguno es estado de un modo.

### Lo que dice el inventario

1. Lo que **sí** había que compartir es el objeto de cada modo: 32308 B para uno
   que esté en el aire.
2. TETRA era el caso raro: su objeto son 28 B y tenía 17691 B de estado al lado,
   en `.bss`, para siempre. Un modo de recepción que ocupa 30,6 % de la RAM del
   equipo sin estar escuchándolo.
3. Lo que **no** se puede mover: los buffers que se pasan a
   `audioStream_start()` (`aprsTxBuf`, `baseband` de NXDN y TETRA, `rxSamples` y
   `txSamples` de DMR, `pcm` de las voces) y el framebuffer. Todos son DMA. Esto
   decide dónde va la arena (§2).
4. Lo que **ya** estaba en PSRAM y sigue: las tablas del paquete DMR
   (`.psram_dmr`, 48820 B), las de P25 (75952 B), YSF (29476 + 14720 + 3786), el
   `soft` de TETRA, los almacenes y el buzón de APRS. El patrón de
   `core/psram_state.h` estaba bien aplicado; lo que faltaba era esto.

---

## 2. La arena

Un bloque, `modeArenaStorage`, de **17680 B** en SRAM, que es `sizeof` del modo
más grande que este build tiene (`OpMode_TETRA`, desde que su estado entró en el
objeto). El registro lo calcula el compilador:

```cpp
constexpr modeArenaEntry modeEntries[] = {
    { OPMODE_FM, "FM", sizeof(OpMode_FM), alignof(OpMode_FM) }, ...
};
alignas(strictestModeAlign()) uint8_t modeArenaStorage[largestModeBytes()];
```

Añadir un miembro a un modo agranda el bloque solo; un modo compilado fuera no
está en el registro y no cuenta.

`ModeArena` no sabe nada de los modos: recibe el almacenamiento, su capacidad y
el registro, así que un test lo maneja con su propia tabla. La API del firmware
es la que se pidió, sobre una instancia única en `rtx.cpp`:

- `int modeArena_acquire(uint8_t modeId)`: 0, `-ENOENT` (modo fuera del
  registro), `-EBUSY` (ya hay uno dentro: **el chequeo de solapamiento**),
  `-ENOSPC` (el registro pide más que el almacenamiento), `-EINVAL`.
- `void modeArena_release()`: devuelve el bloque y lo llena de **veneno**
  (`0xA5`).

Detalles que importan:

- **Alineación.** El bloque se declara `alignas(strictestModeAlign())`, el
  máximo del registro (8 en el C62). `acquire()` sube el puntero a la
  alineación del modo pedido y comprueba que el resultado cabe.
- **Veneno.** Se llena al soltar y se mira al adquirir. Si no está intacto,
  alguien escribió después de liberar: se cuenta en `staleDetections()` (y el
  test lo comprueba). No se aborta: el bloque se pone a cero igualmente, porque
  los constructores de los modos (y los que no inicializan todo) esperan el
  `.bss` a cero de siempre.
- **Cero al adquirir.** Reproduce la semántica de `.bss`: ningún modo ve
  basura del anterior.

### ¿Una arena o dos? ¿SRAM o PSRAM?

**Una, en SRAM.** El inventario lo decide:

- lo que va dentro incluye buffers **DMA** (`baseband` de TETRA, `rxSamples` y
  `txSamples` de DMR) y estado que libtetra toca **dentro del lazo de muestra**
  (`demod`); moverlo a PSRAM rompe el DMA o se come el presupuesto de 30 ms;
- una segunda arena en PSRAM para "lo de por trama" no compraría nada: la PSRAM
  va al 35 % y el patrón que ya existe (`PSRAM_STATE`, sección propia por
  familia) resuelve lo mismo sin indirección en tiempo de ejecución;
- compartir en SRAM es lo que da el ahorro: los 17704 B del bloque sustituyen a
  53839 B.

Por eso el trabajo con la PSRAM fue el contrario: **sacar** de SRAM lo que sí
era de por trama y no estaba ya fuera.

---

## 3. Ciclo de vida

En `rtx.cpp`, `switchMode()` sustituye al `switch` que asignaba el puntero a un
objeto estático:

```
radio_setOpmode(nuevo)      el driver retunea el chip primero (contrato de
                            radio_setOpmode, AGENTS.md)
currMode->disable()         el modo cierra sus streams y el PTT
currMode = &noMode          se publica "sin modo" antes de tocar la memoria
obj->~OpMode()              destructor virtual: el del modo concreto
modeArena_release()         veneno
modeArena_acquire(nuevo)    si falla -> camino de error
constructMode(nuevo, blk)   placement new, sin reservar memoria
currMode->enable()
```

- **Sin dejar nada a medias.** El chip se retunea antes de que el modo viejo
  suelte sus streams (orden que ya tenía el código, y que
  `radio_setOpmode()` documenta); `disable()` es idempotente en todos los modos
  y el destructor la vuelve a llamar sin efectos.
- **Camino de error.** Si `acquire()` falla o el objeto construido no devuelve
  el `getID()` pedido (registro y `constructMode()` son dos listas de los mismos
  modos), se destruye, se devuelve el bloque, se deja el driver en
  `OPMODE_NONE`, `rtxStatus.opMode` vuelve a `OPMODE_NONE` (si no, cada
  actualización reintentaría y escribiría una línea de log cada 30 ms) y en el
  C62 se imprime `MODE <n>: no arena block (<errno>)`. La radio queda en reposo,
  la UI sigue viva.
- **Hilos.** `rtx_rxSquelchOpen()` lo llaman la UI y el scanner. `currMode` se
  publica como `&noMode` mientras el bloque está libre, así que esos lectores
  ven "sin portadora" en vez de memoria que se está reutilizando. La ventana
  entre cargar el puntero y llamar al método virtual no está sincronizada (no lo
  estaba antes tampoco), y ahora es más corta.
- `rtx_terminate()` destruye el objeto y suelta el bloque, para que un ciclo
  terminate/init empiece con el veneno intacto.

### TETRA, el modo que cambió de sitio

`OpMode_TETRA` tenía `baseband`, `demod` y `tmo` como estáticos del fichero.
Ahora son un miembro (`struct tetraModeState state`, `OpMode_TETRA.hpp`) y un
puntero de fichero `st` que se ata al objeto en el constructor, de modo que los
helpers estáticos no cambian. Es lo que hace que el bloque sea el del modo más
grande de verdad y no el del más grande de los "pequeños".

---

## 4. Números

Build de todos los modos, `p4_build.sh` (`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y
-DCONFIG_C62_NXDN_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_P25=y -DCONFIG_C62_DPMR_RX=y
-DCONFIG_C62_DSTAR=y -DCONFIG_DMR_TX=y -DCONFIG_NXDN_TX=y -DCONFIG_DPMR_TX=y
-DCONFIG_P25_TX=y -DCONFIG_YSF_TX=y -DCONFIG_C62_KISS=y -DC62_PC_PROTO=ON
-DC62_DEBUG_PROTO=ON -DC62_AIR_TX=ON`, TETRA por defecto ON).

| región | antes (`86c3b7dc`) | después | |
|---|---:|---:|---|
| RAM | 209416 B (79,89 %) | **177192 B (67,59 %)** | −32224 B |
| ITCM | 8468 B (51,68 %) | 8468 B (51,68 %) | |
| DTCM | 12640 B (77,15 %) | 12640 B (77,15 %) | |
| PSRAMAP | 642278 B (35,00 %) | 642278 B (35,00 %) | igual: no se movió nada allí |
| FLASH | 758948 B (72,38 %) | 759568 B (72,44 %) | +620 B de código |

De dónde sale el ahorro:

| cambio | B |
|---|---:|
| objeto de los once modos → un bloque de 13112 B (el mayor, DMR) | −19168 |
| estado de TETRA (`baseband`+`demod`+`tmo`) al objeto, y el bloque pasa a 17680 B | −13056 |
| **total** | **−32224** (medido: −32224) |

Margen bajo el 70 % (183500 B): **6308 B**.

---

## 5. Verificación

**Host, el bloque** (`tests/unit/mode_arena.cpp`, `meson test`): 7 casos, 1,7 M
aserciones.

- el bloque es el mayor de los once y menor que su suma;
- los once modos reciben el bloque alineado, a cero, y lo devuelven con el
  veneno intacto;
- **121 transiciones** (11×11) con un objeto construido en el bloque en cada
  entrada: mismo puntero, cero en todo el bloque, `getID()` correcto, nada del
  anterior;
- el veneno detecta la escritura después de liberar (`poisonMissing() == 1`,
  `staleDetections() == 1`) y el siguiente modo recibe memoria limpia;
- adquirir con uno dentro se rechaza (`-EBUSY`); modo fuera del registro,
  `-ENOENT`; bloque más corto que el mayor, `-ENOSPC` para todos;
- `modeArena_acquire()`/`modeArena_release()` antes de `rtx_init()` devuelven
  `-ENODEV` en vez de caerse.

**Host, el cambio real** (`tests/unit/mode_switch.cpp`): `rtx_init()`,
`rtx_configure()`, `rtx_task()` — el código del firmware tal cual, con los modos
que el build de Linux tiene (FM, AM de banda aérea, WFM, M17): todos los pares
ordenados, dos vueltas, 32 cambios, `opMode` y dueño de la arena correctos,
mismo puntero, `staleDetections() == 0`; vuelta a `OPMODE_NONE` con el bloque
liberado y envenenado; pedir `OPMODE_DMR` (no compilado en ese build) deja la
radio en `NONE` sin reintentos y sin dejar el bloque tomado; después FM entra
normal.

**Emulador** (`openrtx_linux`, SDL en modo *dummy*): arranca, `rtx_init()`
enlaza la arena, teclas y cambios de configuración sin caídas.

**Otras configuraciones**: el build por defecto del C62 (sin ningún
`-DCONFIG_C62_*`, TETRA por defecto) compila y enlaza con el registro reducido
(RAM 57,42 %, bloque de 17680 B por TETRA); el build de Linux, que solo tiene FM,
AM de banda aérea, WFM y M17, pasa los dos tests nuevos. El bloque se adapta al
registro sin tocar nada.

**No verificado**: no hay hardware en este banco y el CSK6011B no tiene objetivo
de QEMU en este árbol. El lazo TX-RX de DMR y el RX de NXDN/TETRA siguen sin
probarse en el aire; lo que sí está probado es que el camino de cambio de modo no
toca memoria de otro modo, que era el riesgo nuevo. Para el banco: `docs/DMR_TX.md`
y `docs/RX_NXDN.md`/`TETRA_LIB.md` tienen los latidos de recepción que hay que
volver a mirar tras un ciclo de modos.

---

## 6. Riesgos

- **Fragmentación**: no hay. El bloque es fijo, siempre el mismo y siempre en la
  misma dirección; no hay asignador, ni huecos, ni tamaños variables.
- **Alineación**: el almacenamiento se declara con la alineación más estricta del
  registro y `acquire()` sube el puntero; `alignof` viene de cada clase, no de un
  número a mano. El día que un modo necesite 16 B (un `float4` o un buffer DMA
  alineado a caché), basta con que lo declare y el bloque se ajusta.
- **DMA**: los buffers que van al motor de audio están dentro de los objetos, o
  sea dentro del bloque, en SRAM: es la razón de que la arena no esté en PSRAM.
  Si alguien mueve la arena a PSRAM, el DMA de `rxSamples`/`baseband` deja de
  funcionar. Está escrito en el comentario del almacenamiento y aquí.
- **Crecimiento silencioso**: el bloque crece con el modo más grande, que es lo
  correcto, pero un miembro nuevo en TETRA o en DMR se come el margen sin que
  nadie lo note hasta que el mapa lo dice. El margen es de 6308 B; el número que
  hay que vigilar es el `RAM` del build de todos los modos.
- **TETRA y el puntero `st`**: `st` apunta al objeto; el destructor lo pone a
  `nullptr`. Un helper llamado después de destruir el objeto falla en el acto en
  vez de escribir en la memoria reciclada.
- **Modos fuera del registro**: si un modo está en el `enum` y no en el registro,
  `acquire()` devuelve `-ENOENT` y la radio queda en reposo. Es el
  comportamiento buscado, pero es un fallo de configuración, no una función: el
  registro y `constructMode()` tienen que crecer juntos.
- **Hilos**: `rxSquelchOpen()` desde la UI no está sincronizado con el cambio de
  modo (ya no lo estaba); ahora la ventana es más corta porque `currMode` pasa
  por `&noMode`. Si algún día molesta, el arreglo es un id atómico publicado
  antes del objeto, no un mutex.
- **PSRAM**: nada nuevo va allí, así que no hay riesgo nuevo de sección NOLOAD
  sin cero (core/psram_state.h) ni de acceso lento.

## 7. Lo que queda

- **Framebuffer, 40960 B en `.bss`** con otros 40960 B de *shadow* ya en PSRAM:
  es el mayor bloque de SRAM que no es estado de un modo, y es DMA del panel. Si
  se quiere bajar del 60 %, es por ahí (doble buffer en PSRAM + volcado por DMA
  desde ahí, o panel con RAM propia), no por los modos.
- **Heap del sistema, 40960 B** (`kheap__system_heap`): ver si el firmware lo
  usa; si no, es RAM gratis.
- **TETRA `baseband` (5760 B) y NXDN `baseband` (3840 B)**: son DMA y están
  fuera de los objetos. Entrarían en la arena si los modos los pidieran de ella;
  no se hizo porque un buffer DMA tiene que sobrevivir a `enable()`/`disable()`
  del propio modo y ahí el objeto ya está construido: no aporta y complica.
- Las tablas del vocoder AMBE+2 (`dmr_enc_core`, 4354 B en SRAM) son de otro
  dueño (`lib/dmr_encoder`) y se tocan por trama: candidatas a PSRAM si hace
  falta margen.
