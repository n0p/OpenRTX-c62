# Protocolos y vocoders: qué se ha aplicado

Rama `rev/proto`, worktree `~/src/c62/work/rev-proto`, sobre `fa7183de`.
Un commit por hallazgo, en inglés, con el porqué en el mensaje. No se ha
subido nada a ningún remoto, no se ha tocado la radio ni `/dev/ttyUSB*` y no
se ha compilado firmware.

Los 15 hallazgos se sostienen contra el código. Ninguno se ha rechazado; tres
se han aplicado con una corrección respecto a lo que pedía el informe y dos no
llevan test nuevo (abajo, con el motivo).

## Tabla

| # | Hallazgo | Grav. | Veredicto | Commit |
|---|----------|-------|-----------|--------|
| 1 | `TetraSdsEnc.c`: `put()` sin cota en `buf[400]` de pila | alta | arreglado | `b122ceaf` |
| 2 | `DpmrPhy.c`: ~1 KB de pila por intento de temporización | alta | arreglado | `4ca20a60` |
| 3 | NXDN: mitades robadas cruzadas, FACCH al vocoder | media | arreglado | `c55d6dce` |
| 4 | YSF: FICH fallido se lleva 100 ms de voz | media | arreglado | `4f2c69d5` |
| 5 | TETRA: buffer de bits blandos por debajo del peor caso | media | arreglado | `42a1a16f` |
| 6 | DMR: 624 B de pila por ACK de SMS | media | arreglado | `60293cea` |
| 7 | `dmr_voice.c` sin vocoder no compila | media | arreglado | `1889fccb` |
| 8 | D-STAR: acumulador NMEA en 91 sobre 91 | media | arreglado | `4944baa8` |
| 9 | DMR: cabecero con 0 bloques deja `pending` vivo | media | arreglado | `dae42c09` |
| 10 | `dmr_vuv` sin clamp (margen cero) | media | arreglado (cifra del informe corregida) | `3e5cfef3` |
| 11 | `dmr_sms.c`: `pad` negativo = `memset` gigante | baja | arreglado | `87a9c685` |
| 12 | M17: `getNextBlock()` divide por cero | baja | arreglado | `9c8d28f1` |
| 13 | APRS: símbolo leído del byte 19 con `len == 19` | baja | arreglado (sin rechazar la posición) | `a8baf0d4` |
| 14 | DMR: 160 B de pila por línea y copia del log por trama | baja | arreglado | `6dc6d783` |
| 15 | P25: bucle muerto que fija `j = 12` | baja | arreglado | `245322eb` |

## Las dos altas, medidas

**1. SDS de TETRA.** `put()` comprueba el índice contra `8 * sizeof(buf)` y
marca un `overflow` nuevo; todas las funciones del fichero que devuelven
estado responden -1 cuando está puesto (`put`, `text_payload`, `u_sds_data`,
`u_status`, `wrap_llc`, `mac_access`, `mac_data`). El texto se mide antes de
escribirlo (7·septetos/8 octetos en GSM 7-bit, 16 bits/carácter en UCS-2) y el
empaquetado de septetos va directo al escritor con un acumulador de 32 bits.

- `sept[1024]` + `packed[1024]` desaparecen: **2 048 B menos de pila** por
  llamada y **0 B de estático nuevo** (el `overflow` cae en el relleno de
  `struct sds_tx`, que en el firmware ya vivía en PSRAM).
- `sds_tx_mac_data()` rechaza un TM-SDU mayor de 268 bits en vez de dejar que
  `fill_to()` rebobine el contador y entregue un bloque con cola vieja.

**2. Demodulador dPMR.** Sólo los 12 símbolos que fijan la referencia de nivel
se guardan como muestras; los otros 180 se leen y se cortan uno a uno.

`-fstack-usage`, `-O2`, compilador del host:

| función | antes | después |
|---------|-------|---------|
| `decodePayloadAt()` | 1 040 B | 320 B |
| `processNewSample()` (absorbe `payloadWithRetries()`) | 672 B | 704 B |
| pico del camino | **1 712 B** | **1 024 B** |

Además `payloadWithRetries()` decodifica en un *scratch* y copia sólo el
ganador: dos `struct dpmrFrame` vivos en vez de tres. **Nada pasó a estático y
ningún buffer creció**; lo que se ahorra es el marco de la propia llamada.

## Correcciones al informe

- **dmr_vuv (#10).** El informe dice que el máximo de `l * 16 * f0` es 7,0 en
  `b0 = 119`. No es así: es **7,40 en `b0 = 113`**, y trunca a 7 (la última
  columna válida). El margen real es 0,6 de columna, no cero exacto. El clamp
  sigue justificado (mbelib lo tiene y el port lo perdió) y no cambia ningún
  valor con las tablas actuales; el test nuevo fija el lado de las tablas
  (`(int)(L·16·f0) < 8` para las 120 filas).
- **APRS (#13).** El informe pedía `len >= 20`, y eso **rechazaría** una
  posición de 19 bytes que hoy entrega lat/lon válidas (sólo el símbolo salía
  del primer byte del FCS). Se mantiene `len >= 19`, el símbolo queda en blanco
  cuando el campo acaba ahí y la longitud del comentario se acota
  (`len > 20 ? len - 20 : 0`) en vez de subdesbordar.
- **D-STAR (#8).** No se añade la llamada a `dstarSlow_sentence()` dentro de
  `blockComplete()`, como sugería la segunda mitad del arreglo: consumiría la
  sentencia por detrás del llamante, que es quien la publica. El invariante
  queda garantizado donde debe, en el propio `append` acotado.

## Sin test nuevo (y por qué)

- **DMR, cabecero con 0 bloques (#9).** `dmr_data_header()` recibe 96 bits con
  una máscara CRC-CCITT privada de `dmr_data.c` y un bloque necesita el
  codificador BPTC y la máscara CRC-9 del mismo fichero: un caso a ese nivel
  tendría que copiar tres constantes privadas al test. Los tests de datos de
  DMR (`dmr_rx_status`, `dmr_glue`, `dmr_sms_format`) siguen en verde.
- **DMR, log por trama (#14, segunda mitad).** No hay ningún test que lea
  `dmr_proto_get_log()`; el cambio se limita a publicar la instantánea cuando
  `s_log_seq` cambia y a vaciarla en `dmr_proto_reset()`, que mueve la marca
  para que la primera trama tras un reset no publique una copia vieja.

Cada arreglo de comportamiento va con un caso que **falla al revertirlo**,
comprobado revirtiendo el arreglo a mano:

| arreglo | al revertirlo |
|---------|---------------|
| SDS de TETRA | el test de texto largo revienta (SIGSEGV, escribe ~700 B fuera) |
| bits blandos de TETRA | `REQUIRE(n <= CAP)` falla con `80 <= 64` |
| NXDN | `first == 2` falla con `0 == 2` |
| YSF | `voiceCount == 5` falla con `0 == 5` |
| D-STAR | `gpsLen <= 90` falla con `'['` (91) |
| M17 | el caso nuevo falla |
| APRS | el símbolo sale `'X'` (el byte del FCS) en vez de blanco |
| `dmr_voice` sin vocoder | el fichero no compila: 3 identificadores sin declarar |

Sin caso rojo posible, y por qué:

- **`dmr_vuv` (#10):** el clamp no es observable con las tablas actuales (el
  índice ya cae en rango). El test fija el otro lado, las tablas generadas.
- **Pila/PSRAM sin cambio de comportamiento (#6, #11, #14):** los tres son
  movimiento de buffers o una copia que ya no se hace; no hay ninguna salida
  que cambie. `blocks()` (#11) además pierde el parámetro redundante, así que
  la firma ya no permite el estado incoherente.


## Suite del host

Mismo conjunto de comandos:

```
meson setup build_linux -Dcpp_args=-fpermissive
ninja -C build_linux -k 0
ninja -C build_linux sim-coverage.txt
meson test -C build_linux --no-rebuild
```

- Estado base en este worktree, **antes** de tocar nada: `Ok: 110 / Fail: 0`.
- Con los 15 arreglos: `Ok: 110 / Fail: 0` (los errores de `ninja -k 0` son
  los targets de miosix/ARM, que fallan por diseño).
- libtetra es un módulo west y no está vendorizado, así que los dos tests de
  TETRA (status y SDS) no se registran sin apuntarlo a mano. Con
  `-Dtetra_lib_dir=~/src/c62/build/c62-ws/modules/lib/libtetra` (rev
  `6fc088c`, la que fija el firmware) la suite es **112/112** — y es donde
  corren los dos casos nuevos del codificador SDS, que hacen el viaje completo
  texto → `TetraSdsEnc.c` → LLC/MAC → receptor de uplink de libtetra.

---

# Rutas de TX y RX de los modos digitales: qué se ha aplicado

Rama `claude/radio-tx`, worktree `~/src/c62/work/cl-radio2`, sobre `a3c9976e`
(master). Un commit por arreglo, en inglés y con el porqué en el mensaje. No se
ha subido nada a ningún remoto, no se ha tocado la radio ni `/dev/ttyUSB*` y no
se ha compilado firmware.

**Nada de esto se ha probado en el aire ni en el banco.** Los cinco modos no se
compilan en el host (`meson.build:483`: `CONFIG_NXDN` arrastraría el demodulador
y el vocoder de AMBE+2 a todos los binarios de test), así que no hay test que
pueda cubrir estas rutas: el diagnóstico es lectura de código —está abajo, con
fichero y línea— y cada arreglo lleva su procedimiento de banco al final de este
documento. Los tests del host se quedan como estaban, **113/113**.

Consecuencia para el operador, la misma que anunciaba `FINDINGS-PROTO.md`: D-STAR
y dPMR pasan a poder seleccionarse, y P25, YSF y D-STAR pasan a emitir RF de
verdad. Es la primera vez que esos cinco caminos clavean la PA, así que la
primera prueba va con carga fantasma, 500 mW y los atenuadores del banco
(`docs/TX_SAFETY.md`, "The second barrier").

## Tabla

| hallazgo | grav. | veredicto | commit |
|---|---|---|---|
| D-STAR no enciende el receptor: el modo no arranca | alta | arreglado | `8a621ce0` |
| dPMR no enciende el receptor, y vuelve de TX con `RX` sin rearmarlo | alta | arreglado | `6bf892aa` |
| YSF no clavea la PA: el PTT pone TX y no sale RF | alta | arreglado | `03bb2d36` |
| P25 no clavea la PA: ídem | alta | arreglado | `2efd3422` |
| D-STAR no clavea la PA: ídem | alta | arreglado | `424222bd` |
| YSF: el flujo lee 9,6 kB más allá de `txSamples[4800]` | alta | arreglado | `03bb2d36` |
| P25: el flujo lee 2,9 kB más allá de `txSamples[1440]` | alta | arreglado | `2efd3422` |
| YSF, P25 y D-STAR: se escribe siempre la mitad que suena y la otra queda con basura | alta | arreglado | `03bb2d36`, `2efd3422`, `424222bd` |
| NXDN y dPMR: el TxGuard aborta la segunda transmisión a los 300 ms | media | arreglado | `2d9fb769` |
| NXDN: el TxGuard corta el TX_REL de **todas** las transmisiones | media | arreglado (se mueve `txg_finishing`, no el presupuesto) | `2d9fb769` |
| NXDN: tras un corte del supervisor el modo no vuelve | alta | arreglado | `40275f36` |
| dPMR: tras un corte del supervisor el modo no vuelve | alta | arreglado | `6bf892aa` |
| dPMR: un `newCfg` durante la TX deja la PA clavada sin dueño | alta | arreglado | `6bf892aa` |
| *no estaba en la lista:* YSF, P25 y D-STAR, tras un corte, dejan los flujos abiertos y el modo en `OFF` | alta | arreglado | `ee4f38da` |
| *no estaba en la lista:* D-STAR genera el patrón de fin y lo tira | media | arreglado | `424222bd` |

Ninguno se ha rechazado: los tres diagnósticos se sostienen contra `master`
(evidencia abajo). Lo que **no** se ha tocado está en "Fuera de alcance", con el
motivo.

## Cómo se sostiene cada diagnóstico

**1. D-STAR y dPMR no llaman a `radio_enableRx()`.** `grep` sobre los dos
ficheros: D-STAR solo tiene `radio_disableRtx()` en las líneas 253, 291 y 342 y
dPMR en 211, 347 y 507; el resto de modos planos lo llaman en su rama de
entrada (YSF:378, P25:288, NXDN:408). `radio_enableRx()`
(`platform/drivers/baseband/radio_C62.cpp:805-886`) es lo único que pone
`radioStatus = RX`, y `switchMode()` espera `RTX_MODE_START_MS` = 300 ms a ese
valor antes de volver al modo anterior (`rtx.cpp:347-372`, `:512-517`,
"MODE n: not receiving after 300 ms, reverting"). Los dos modos, además, tienen
plan de radio plano en RX y TX (`radioPlan.c:312-323`), así que el resto del
camino estaba; faltaba pedir el receptor. dPMR tenía el segundo defecto: al
acabar de transmitir ponía `rtx->opStatus = RX` sin pasar por `OFF`, que es el
único sitio donde se enciende el receptor.

**2. YSF, P25 y D-STAR no clavean.** `grep -n 'radio_enableTx\|txg_' ` sobre los
tres ficheros en `master`: cero. El PTT hacía `radio_disableRtx()`, `startTx()`
y `opStatus = TX`; el driver se quedaba en `OFF` (`radio_disableRtx()` →
`radio_endTx(false)`) y el audio se escribía en una ruta `MCU→SINK_RTX` cuyo
extremo estaba apagado. Ahora la secuencia es la de `OpMode_DMR::startTx()`
(`:442-523`), que es la que el banco ya usa: `txg_enter`, `txg_streamsOpen`,
`radio_enableTx()`, comprobar `radio_getStatus() == TX`, `txg_note(KEYED)`.
Ojo: `radio_enableTx()` rechaza si `rtx_txPermitted()` es falso
(`radio_C62.cpp:891-907`), así que la puerta de clave sigue siendo la que manda;
el modo solo traduce el rechazo a una transmisión fallida en vez de a un TX de
mentira.

**3. El flujo lee el doble del búfer.** `audioStream_start(path, buf, length,
...)` recibe la longitud **total** del búfer y un flujo `BUF_CIRC_DOUBLE` parte
esa longitud por la mitad (`csk6_codec.c:277-282`, `blockSamples()`). En
`master`, YSF declara `static int16_t txSamples[TX_FRAME_SAMPLES]` (4 800
muestras = 9,6 kB) y pasa `2 * TX_FRAME_SAMPLES` (19,2 kB); P25 declara 1 440 y
pasa 2 880. El driver recorre `ctx->bufSize` muestras, o sea 9,6 kB / 2,9 kB más
allá del array, cada 100 ms / 30 ms, en PSRAM. NXDN y dPMR lo hacían bien
(`txSamples[2 * ...]`), que es lo que delataba el error.

**4. La mitad que suena se pisa.** `txSlot()` de los tres hacía
`render(txSamples/audioBuffer, N); outputStream_sync(id, false);`: siempre la
primera mitad, y con `bufChanged = false`. El codec reproduce alternando mitades
(`csk6_codec.c:891-915`: `playStream()` avanza `ep->position` por el búfer), así
que la mitad que sonaba se reescribía y la otra se quedaba con lo que hubiera
—basura de PSRAM, que es `NOLOAD`—. El patrón correcto, ya usado por DMR
(`:584-604`), NXDN (`:272-290`) y dPMR (`:371-389`), es
`outputStream_getIdleBuffer()` + `outputStream_sync(id, true)`. Los tres modos
lo usan ya. De paso, el búfer se pone a cero al clavear: el codec empieza a
reproducir la primera mitad en cuanto el flujo se engancha
(`csk6_codec.c:1331-1345`, `attach()` pone `position = 0`), así que sin el
`memset` los primeros 30–100 ms de la transmisión serían lo que hubiera quedado
en PSRAM o en el búfer de recepción de D-STAR.

**5. `txFirstBlock` no se limpia.** `putHalf()` reporta `txg_block()` si el flag
está puesto y `txg_firstBlock()` si no (NXDN `:283-288`, dPMR `:382-387`), y
`txFirstBlock` solo se inicializaba en el constructor. `txg_block()` se ignora
en `TXG_PHASE_WAIT_FIRST` (`TxGuard.c:103-107`), así que desde el segundo PTT el
guarda no salía nunca de `WAIT_FIRST` y `TXG_FIRST_MS` (300 ms) abortaba. DMR lo
limpia en `startTx()` (`:496`) y en `stopTx()` (`:531`); los dos modos ahora
también.

**6. El presupuesto de drenaje no cabe en el TX_REL.** `TXG_DRAIN_MS` es 160 ms
(`TxGuard.h:86`) y NXDN llamaba a `txg_finishing()` al soltar el PTT
(`:314`), antes del superframe de liberación: 4 tramas de 80 ms
(`NXDN_SUPERFRAME`) más la cola. Es decir, `txg_tick()` devolvía `ABORT` con
motivo `timeout_drain` **en cada transmisión** y el supervisor bajaba la PA a
mitad del TX_REL. Ahora `txg_finishing()` se llama donde acaba el superframe y
empieza la cola real (dos mitades de 40 ms), que sí cabe en el presupuesto.
dPMR ya lo hacía así (`:412`: la END frame, 20 ms, más 40 ms de cola) y no se
toca.

**7. Sin `abortTx()`, el corte no vuelve.** `rtx_task()` pone `opStatus = OFF`
tras un aborto (`rtx.cpp:917-933`) y `abortTransmission()` llama a
`currMode->abortTx()` en cada reintento (`:684-712`). NXDN y dPMR no lo
sobrescribían, y su rama `OFF` solo entra en recepción con `startRx`
(NXDN `:370-377`, dPMR `:476-484`), que la transmisión había puesto a `false` /
no volvía a poner. Resultado: radio muda con el chip apagado y el guarda en
`ABORT` pidiendo el mismo aborto cada 20 ms hasta cambiar de modo. En dPMR era
peor: `reset()` no cerraba `txRunning` ni los flujos, así que quedaban abiertos.
Ahora los dos tienen `abortTx()` (forma de `OpMode_FM::abortTx()`, `:311-337`).

**8. `newCfg` en mitad de la TX (dPMR).** El primer bloque de `update()` es
`if (newCfg || opStatus == OFF) { reset(); if (startRx) { opStatus = RX; ... } }`
y va **antes** de la rama `TX`: un cambio de configuración con la PA arriba
devolvía el modo a recepción sin `stopTx()`. Ahora cierra la transmisión antes
de resetear.

## Una característica que se verá en el banco (no es un defecto nuevo)

Al clavear, el códec reproduce la primera mitad del búfer del flujo en cuanto
se engancha (`attach()` pone `position = 0`) y el modo escribe en la **otra**
mitad (`outputStream_getIdleBuffer()`), así que la primera mitad va con lo que
se dejó en el búfer: por eso los cuatro modos lo ponen a cero al empezar. Lo que
se oye es silencio modulado con la PA ya arriba durante media vuelta de búfer:
100 ms en YSF y 30 ms en P25, D-STAR, NXDN y dPMR (que ya lo hacían así antes de
este cambio). Un receptor no se molesta por eso —es una portadora con silencio,
como el pre-roll de cualquier equipo— pero conviene saberlo al mirar la forma de
onda en el banco: el primer bloque de datos no empieza en el flanco de PTT.

## Las dos que no estaban en la lista (y por qué van igual)

- **YSF/P25/D-STAR sin `abortTx()` útil** (`ee4f38da`). En cuanto estos modos
  pasan a tener guarda (`txg_enter`), el corte del supervisor tiene que tener
  salida o se convierte en un bucle de abortos: es parte del mismo cambio. La
  forma es la de FM y DMR: cerrar sin esperar, cerrar el guarda, bloquear el PTT
  hasta que se suelte y pedir el receptor.
- **D-STAR: el patrón de fin nunca salía** (`424222bd`). `stopTx()` lo generaba
  en el búfer entero y llamaba a `audioStream_terminate()`, que cancela el flujo
  al instante (`csk6_codec.c:1527`: `.terminate = csk6codec_halt` → `detach()`):
  lo que el códec no había reproducido se tira. Se genera en la mitad ociosa y
  se cierra con `audioStream_stop()`, que deja terminar la mitad en curso
  (`csk6_codec.c:1485-1521`, `finishOutputStream()`, con plazo). Mismo patrón
  que DMR/NXDN/dPMR.

## Decisiones de diseño (documentadas, no improvisadas)

1. **`abortTx()` cierra todo con `stopTx(false)`, no solo los flujos.** El
   contrato de `OpMode.hpp:69-82` pide "cancelar los flujos y nada que el modo
   libere por sí mismo", y `FINDINGS-PROTO.md` (menor) señala que YSF/P25
   ejecutaban `stopTx()` entero desde el supervisor. Se ha quitado lo que
   rompía el contrato de verdad —las esperas y la cola—, pero se mantiene la
   liberación de rutas, por dos motivos: (a) `OpMode_FM::abortTx()` ya libera su
   ruta `TX` desde el supervisor, así que es el patrón que este árbol acepta; y
   (b) la alternativa estricta exige que cada modo cierre sus rutas en su propia
   rama `OFF`, y YSF/P25/NXDN no llaman a `stopTx()` ahí, así que si el hilo RTX
   no estaba dentro de un flujo cuando llega el aborto la ruta `SINK_RTX` se
   queda tomada. `audioPath_release()` es idempotente (`routeFind()` devuelve
   `nullptr` y sale, `audio_path.cpp:305-322`), así que un `stopTx()` del hilo
   RTX a la vez no cierra dos veces. Si se quiere la versión estricta, es un
   cambio aparte: `stopTx()` en la rama `OFF` de YSF/P25/NXDN y `abortTx()` sin
   tocar rutas.
2. **Presupuesto de drenaje por modo: no se ha cambiado el de todos.** La cola
   de YSF (hasta 200 ms: `TX_TAIL_SLOTS` 2) y la de P25 (hasta 360 ms: el resto
   de la trama más el TDU de 180 ms) tampoco caben en `TXG_DRAIN_MS` = 160. Por
   eso ninguno de los dos llama a `txg_finishing()`: se quedan en `RUNNING` con
   el tope absoluto (`TXG_MAX_TX_MS`, 5 min) y el reloj de 90 s. Las opciones,
   para decidir con el banco delante: (a) presupuesto por transmisión
   (`txg_finishing(guard, now, ms)`, o un campo que el modo fije tras
   `txg_enter()`), que es lo que el informe de protocolos propone como
   alternativa; (b) subir `TXG_DRAIN_MS` a ~400 ms, que no retrasa ninguna
   transmisión normal y solo tarda 240 ms más en cortar una cola atascada; (c)
   dejar el tope de 5 min como está. No se elige aquí porque cambia un número
   compartido por FM, DMR y M17.
3. **`txBlocked` en NXDN y dPMR.** No existía en esos dos modos. Se añade para
   que un PTT que siga pulsado no vuelva a clavear en la pasada siguiente al
   aborto (contrato de `OpMode.hpp:82`); se limpia al soltar el PTT, igual que
   DMR/YSF/P25.

## La rama `claude/radio`, qué se ha tomado y qué no

Se ha usado como referencia de *qué* tocaba, no como código. Los cuatro
diagnósticos coinciden, pero el parche no se sostiene tal cual:

- **No toca el búfer de audio**: deja YSF y P25 con `txSamples[N]` y el flujo
  abierto con `2 * N`, y deja los tres modos escribiendo la mitad equivocada.
  Los dos defectos de la ruta de audio siguen ahí.
- **Añade `txg_finishing()` al soltar el PTT en YSF y P25**, con un presupuesto
  de 160 ms y colas de hasta 200 y 360 ms: introduciría un aborto por
  `timeout_drain` en **cada** transmisión de los dos modos.
- Mira `guard->phase` directamente en lugar de usar el flag `txFirstBlock` del
  modo, que es la convención del árbol (DMR/NXDN/dPMR).
- En dPMR vuelve a recepción con `opStatus = RX` y enciende el receptor en dos
  sitios; aquí se pasa por `OFF`, que es el único punto de entrada a recepción
  del modo y lo deja con una sola ruta.
- Está sobre un árbol anterior y revierte cosas ya fusionadas, como avisaba el
  encargo.

## Fuera de alcance (visto, no tocado, con el motivo)

- **`OpMode_NXDN.cpp:309-315`, `OpMode_DPMR.cpp:409-413`** (menor de
  `FINDINGS-PROTO.md`): soltar el PTT pone `txHalf = 0` a mitad de trama y la
  trunca antes del TX_REL/END. Es un defecto de temporización de la trama, no
  del guarda ni del receptor; merece su propio commit y su propia prueba de
  banco (¿decodifica el receptor el TX_REL?).
- **`OpMode_M17.cpp:490`** (menor): no comprueba `radio_getStatus() == TX` tras
  `radio_enableTx()`. M17 no estaba en el encargo; el arreglo es de dos líneas y
  ya está el patrón hecho aquí.
- **`cfg.useNullVoice = true` fijo en D-STAR** (`OpMode_DSTAR.cpp:153`): el
  modulador acepta una fuente de voz y el modo nunca la instala, así que D-STAR
  sale con tramas nulas. Es una función que falta (micro → vocoder 3600x2400),
  no un defecto de la ruta.
- **`dmr_demod_slot_start()` sin usar** y la alineación de ráfaga de DMR por
  repetidor (`FINDINGS-PROTO.md`, alta): fuera del encargo y sin relación con
  estos cinco modos.
- **`docs/RX_FUSION.md` sigue diciendo "No transmitter"** en tres sitios (líneas
  28-29, 438 y 443) y describe el modo como receptor sin PTT. No es cierto desde
  antes de este cambio (`docs/TX_YSF.md` y `protocols/YSF/YsfTx.c` están en el
  árbol), pero el documento es largo y su sección 6 es una lista de "no hecho"
  que merece releerse entera, no un cambio de una línea. Se han corregido las
  tres frases equivalentes de `RX_NXDN.md`, `RX_P25.md` y `RX_DPMR.md`, que sí
  eran de una línea.
- **Los `*Speech.c` que ignoran el resultado de `outputStream_sync()`** (menor):
  es la ruta de **recepción** de audio del vocoder, común a seis modos; se toca
  aparte o no se toca.

## Qué hay que medir en banco para dar esto por bueno

Todo con carga fantasma, 500 mW y los atenuadores puestos; el primer PTT de cada
modo es el primero de su historia, así que también se mira que no quede
portadora al soltar.

| modo | qué se mide | qué tiene que pasar |
|---|---|---|
| D-STAR | seleccionar el modo y mirar el puerto serie | no aparece `MODE 3: not receiving after 300 ms, reverting`; el modo se queda, el LED verde sigue al squelch y un equipo D-STAR/DSD decodifica la cabecera |
| dPMR | ídem, y después un PTT | ídem; tras la transmisión vuelve a recibir (se oye al otro equipo) sin cambiar de modo |
| YSF | PTT de 10 s con un FT-70/dsd-neo a la escucha | RF en la frecuencia, 22 tramas: cabecera, V/D1, terminador; `ab` del guarda en 0 y `lat` < 300 ms en la traza `TX n=..` |
| P25 | PTT de 10 s con un receptor P25/dsd | HDU + TSBK + TDU; el TDU (terminador) tiene que oírse **completo**: es la prueba de que el drenaje del flujo funciona |
| D-STAR | PTT de 10 s con un Icom/DSD | cabecera, voz y **patrón de fin** (antes no salía nunca); la desviación según `docs/TX_DSTAR.md` |
| NXDN | dos PTT seguidos sin cambiar de modo | ninguno se corta a los 300 ms (`TX ABORT timeout_first` no aparece, `ab` no sube); al soltar, la PA sigue ~400 ms (TX_REL + cola) y el receptor decodifica el TX_REL |
| dPMR | dos PTT seguidos | ídem; el END frame se decodifica y el modo vuelve a RX |
| los cinco | PTT mantenido hasta que salte el corte de 90 s | la radio **vuelve a recibir sola**, un solo `TX ABORT` en el log, el guarda en `idle` (`ph=exit`) y el siguiente PTT transmite |
| los cinco | `TUNE` del banco de medidas mientras se transmite | no queda portadora clavada: el modo cierra la transmisión y vuelve a RX (dPMR es el que tenía el agujero) |
| YSF/P25 | desviación | la del `docs/TX_*.md`: el nivel es una proporción medida, no una calibración; REG_40 es el número a ajustar |

## Suite del host

Los mismos comandos que la otra revisión:

```
meson setup build_linux -Dcpp_args=-fpermissive -Dtetra_lib_dir=/home/n0p/src/c62/build/c62-ws/modules/lib/libtetra
ninja -C build_linux -k 0
ninja -C build_linux sim-coverage.txt
meson test -C build_linux --no-rebuild
```

- Base en este worktree, antes de tocar nada: `Ok: 112 / Fail: 1`, y el fallo es
  `Simulator Coverage`, que necesita el manifiesto que genera el tercer comando
  (no se había ejecutado todavía).
- Con los ocho commits: **`Ok: 113 / Fail: 0`**.
- `ninja -k 0` sigue devolviendo error: son los targets de miosix/ARM
  (`openrtx_cs7000…rt4d`) y `linux_inputStream_test`, que ya fallaban en
  `master` por falta de toolchain / API retirada (`inputStream_start`); ninguno
  toca los ficheros de este cambio.
- Estas rutas no tienen test de host: los cinco modos no se compilan ahí
  (`meson.build:483`). Lo que sí se ha hecho para no ir a ciegas es compilar los
  cinco ficheros con `g++ -fsyntax-only -Wall -Wextra` en las dos variantes
  (`CONFIG_*_TX` definido y no definido), sin avisos nuevos.
