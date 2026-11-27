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

# Protocolos y vocoders: segunda pasada (rama `claude/proto`)

Worktree `~/src/c62/work/cl-proto`, rama `claude/proto` sobre `a3c9976e`
(master). Un commit por hallazgo, en inglés, con el porqué en el mensaje. Nada
subido a ningún remoto; la radio y `/dev/ttyUSB*` sin tocar; **no se ha
compilado el firmware** (sí los ficheros sueltos con `-fsyntax-only`, ver
abajo).

Alcance: los hallazgos de `FINDINGS-PROTO.md` que no son de rutas de modo. Los
marcados `[=RADIO]` los lleva el agente de radio. El `alta` #3 (P25/YSF/D-STAR
clavean sin RF) tampoco es de esta pasada: su arreglo es la ruta de TX de
`rtx.cpp`/`radio_C62.cpp` y, por decisión del dueño del proyecto, los modos se
arreglan y no se retiran de `opmode_canTransmit()`.

## Tabla

| # | Hallazgo | Grav. | Veredicto | Commit |
|---|----------|-------|-----------|--------|
| 1 | dPMR: `newCfg` en plena TX deja la PA clavada | alta | **arreglado** | `bce502ce` |
| 2 | D-STAR y dPMR no encienden el receptor | alta | `[=RADIO]`, fuera de alcance | — |
| 3 | P25/YSF/D-STAR: PTT sin RF, flujo al doble, mitad pisada, patrón de fin | alta | fuera de alcance (ruta de modo) | — |
| 4 | NXDN/dPMR: el TxGuard aborta y el modo no vuelve | alta | `[=RADIO, ampliado]`, fuera de alcance | — |
| 5 | DMR: TX de datos desatendida reintentada sin límite | alta | **arreglado** | `cb5afd81` |
| 6 | Demoduladores con posición absoluta (YSF 46,6 min; D-STAR 5:50) | alta | ya estaba en master (`45e0bffd`); la parte de YSF que faltaba es la #9 | — |
| 7 | DMR por repetidor: ráfaga sin referencia de slot y portadora en el slot ajeno | alta | **diagnóstico, no forzado** (abajo) | — |
| 8 | Voz D-STAR: hilo sin pila, vocoder sin inicializar, bucle de reproducción | alta | **arreglado, no verificado en el aire** | `d1f42582` |
| 9 | YSF: bloques de 4800 muestras, 47 % de fases de sincronismo perdidas | alta | **arreglado** (con test que falla sin el arreglo) | `118dc7ea` |
| 10 | Tras cada paquete APRS, FM se queda sin receptor | alta | `[=RADIO]`; ya estaba en master (`ae3c5dff`) | — |

Verificación de lo que ya estaba: `YsfPhy.c:581-601` (`rebase()`) y
`DStarPhy.c:406-421` (rebase de `symPosQ8`/`yTotal`) son el arreglo del #6;
`OpMode_FM.cpp:398-432` devuelve el receptor por sus tres salidas, que es el
#10. NXDN/P25/dPMR siguen con la posición en `float` absoluto
(`NxdnPhy.c:297-301`), que es la parte no hecha del #6 y está marcada
`[=RADIO]`.

---

## #1 dPMR: `newCfg` con la PA arriba — arreglado

Se sostiene. `rtx.cpp:939-941` conserva `opStatus` al copiar una configuración
nueva, y `OpMode_DPMR::update()` empieza con `if (newCfg || OFF) { reset(); ...
opStatus = RX; return; }` antes del bloque de TX: un `newCfg` durante una
transmisión dejaba la PA keyed, el flujo abierto con el último medio búfer en
bucle, `txRunning` en true y nadie llamando a `txSlot()`; el TxGuard se quedaba
en RUNNING, sin fase de cierre y por tanto sin plazo. `startRx` no vale de
guarda: sólo lo pone a `false` `disable()` (`:205`), así que esa rama siempre
entra.

Arreglo: `if (txRunning) stopTx();` como primera línea de la rama, antes de
`reset()`. `stopTx()` para el flujo, termina el micro, libera las dos rutas,
baja la PA y saca el guard a idle; el pase siguiente vuelve a muestrear porque
`stopSampling()` dejó `basebandId` en -1.

Sin test: no hay camino de `switchMode`/`newCfg` para dPMR en la suite del
host. Qué medir en banco: clave dPMR, cambie de canal o de potencia con el PTT
mantenido y compruebe en REG_33 (línea de nota de la PA) que cae en el mismo
pase.

## #5 DMR: reintento sin límite de la TX de datos — arreglado

Se sostiene, y era peor de lo que decía el informe: el bloqueo de `txRefuse()`
dura una pasada porque `update()` hace `if (!ptt) txBlocked = false;` y en una
TX desatendida el PTT nunca está pulsado. Además, ni el fallo al abrir ni una
llamada cortada a media lista pedían al paquete que soltara la lista, así que
`needDataTx()` seguía contestando que sí con las ráfagas que quedaban en cola.

Arreglo: `dataRetryAt`/`dataTries`; tres intentos como máximo, 2 s entre ellos;
`txPower == 0` no se intenta; el tercer fallo llama a `dataGiveUp()`
(`dmrSms_txDone(false)` + `messaging_done(..., false)`, que es lo que vacía la
cola y lo que ve la UI); una llamada que acaba con la lista a medias hace lo
mismo en vez de re-clavear por el resto; una que llega al final limpia el
contador; `reset()` limpia el estado de reintento (canal nuevo = intento
nuevo).

`dmrSms_tick()` **no** se ha conectado: sólo caduca listas que nunca
empezaron y las tira sin `messaging_done`, lo que dejaría la entrada de
`core/messaging` en cola para siempre (la desincronización del #14 del
informe). Con el límite de reintentos ya no hace falta para acotar el bucle.

Sin test: `OpMode_DMR::update()` no es alcanzable desde la suite
(`tests/unit/dmr_rx_status.cpp` lo dice en su cabecera: necesita el kernel de
Zephyr, los flujos y la radio). Verificado compilando el fichero en las dos
configuraciones que importan (`DMR_TX`, `DMR_TX+DMR_SMS`) y por lectura.

## #7 DMR por repetidor — diagnóstico, no forzado

Se sostiene en los dos puntos, y el arreglo es grande y de banco:

- **La alineación es un retardo fijo, no una referencia.** `OpMode_DMR.cpp`
  pone `txAligned = (txFrequency != rxFrequency)` y `txSilence =
  txPreRoll()`, y `dmr_glue.c:99-107` devuelve el retardo de aire (40 ms
  medidos) módulo una trama TDMA: 1 920 muestras. Eso no dice nada del TS1 del
  repetidor: cada PTT cae en una fase aleatoria de su trama de 60 ms.
- **La referencia existe pero no tiene quien la use.** `dmr_demod_slot_start()`
  (`dmr_demod.c:339`, alimentada desde `:329-337` con el `t0` de cada ráfaga de
  repetidor y su `seq`) sólo la llama `OpMode_DMR_RX.cpp:718`, que **no está en
  ningún build** (`meson.build`/`CMakeLists.txt` no lo listan).
- **La portadora en el slot ajeno es real.** El modo clavea una vez y mantiene
  la PA arriba toda la llamada; el slot que no es nuestro se rellena con el
  símbolo nulo (`renderHalf()` con `carry = false`), que es portadora sin
  desviación, no silencio de RF. La puerta por slot que usaba el fichero muerto
  (`radio_c62_paGate()`) tampoco existe: en `platform/` no hay definición, sólo
  el uso en `OpMode_DMR_RX.cpp` (y `csk6codec_captureTime()`, que el mismo
  fichero declara localmente, no está en el driver del códec).

Lo que haría falta, y por eso no se ha hecho aquí: (a) una API nueva en
`csk6codec` que relacione un índice de captura con una hora de reloj (o
cualquier otra forma de conocer la fase del repetidor, que hoy no existe); (b)
portar del fichero muerto el seguimiento de referencia completo —`rptTiming()`,
`rptPeriod()`, PPM, compensación del hueco al reiniciar la captura, `txFreezeTs1()`:
unas 400 líneas con estado, y con él la forma de decidir cuándo la referencia es
vieja; (c) una puerta de PA por slot en `radio_C62.cpp`, hoy inexistente, que
además tiene que convivir con `radio_txPathOff()`; (d) medida en banco contra un
repetidor real (error de fase aceptable, rampa de subida/bajada por slot). Sin
(a)-(c) lo único que se puede hacer es elegir entre "no transmitir por
repetidor" (cambio de comportamiento que nadie ha pedido y que hoy funciona a
ojo con el pre-roll) o seguir como está. Se deja como está y documentado.

En la misma función hay un defecto **medio** del informe (#11) que tampoco se
ha forzado: el pre-roll enmascara símbolos en vez de desplazar la ráfaga, así
que con 1 920 muestras la primera cabecera LC pierde sus dibits 0-41 de 132
(`renderHalf()`, `OpMode_DMR.cpp:610-625`). Arreglarlo bien exige que la ráfaga
pueda empezar a caballo entre dos mitades —el modelo actual renderiza una
ráfaga por mitad anclada en el símbolo 6— y eso es rehacer el camino de
render de TX de DMR, con el test `dmr_tx_host` como contrato. Va con el #7.

## #8 Voz D-STAR — arreglado, no verificado en el aire

Los tres puntos se sostienen:

- `pthread_attr_setstacksize()` sin `pthread_attr_setstack()`: el
  `pthread_create()` de Zephyr 3.4 del workspace
  (`lib/posix/pthread.c:205-215`, `:303`) devuelve `EINVAL` cuando
  `attr->stack == NULL`, así que el hilo no existía nunca y el fallo no se
  registraba.
- `dstarVocoder_reset()` no tenía ningún llamador en el firmware (sólo los
  tests de `lib/dstar_vocoder` y `tests/host/dstar`), y el estado vive en
  `.psram_section` (NOLOAD, sin limpiar:
  `platform/targets/c62/CMakeLists.txt:552`).
- El bucle decodificaba en `pcm[0..159]` y sincronizaba con `sync(id, false)`
  en un flujo `BUF_CIRC_DOUBLE`: la mitad que suena es la que se acaba de
  escribir, así que sonaba 20 ms sí y 20 ms no; y `path`, local, no se liberaba
  nunca.

Arreglo: el hilo y el bucle son ahora los de `NxdnSpeech.c` (malloc +
`pthread_attr_setstack` con `free` al parar y si el `create` falla,
`outputStream_getIdleBuffer()` + `sync(..., true)`, `audioStream_stop()` +
`audioPath_release()`), y `dstarVocoder_reset()` se llama al abrir el modo y al
empezar cada burst. El `reset` se declara como hook débil en
`protocols/DSTAR/DStarVocoder.h` (junto a `decode`/`available`) para que
`DStarSpeech.c` no dependa de `lib/dstar_vocoder` en el include path; el núcleo
2400 lo redefine, igual que ya hacía con `decode`.

**No verificado en el aire**: `DStarSpeech.c` no está en la suite del host
(sólo el PHY, el vocoder y la capa de trama), así que esto está comprobado
compilando el fichero en las dos ramas del bloque de atributos (Zephyr y host,
con `-Wall -Wextra -Wpedantic` limpio) y por lectura. Qué medir en banco:

1. una fuente DV de D-STAR dentro: el altavoz debe sonar seguido, sin huecos de
   20 ms, y el contador de tramas reproducidas de la pantalla de estado tiene
   que subir;
2. la primera transmisión tras encender no debe empezar con un golpe de ruido
   (eso es el vocoder sin inicializar);
3. entrar y salir de D-STAR 40 veces y comprobar que DMR/NXDN siguen abriendo
   el altavoz (ruta de audio no agotada) y que el heap no baja 8 KiB por ciclo;
4. `dstarSpeech_stats()`: relación reproducidas/encoladas en 10 s de
   transmisión.

## #9 YSF: el `hunt()` sin trocear — arreglado

Se sostiene y el cálculo es exacto. El modo entrega 4800 muestras por bloque
(`OpMode_YSF.cpp:65`, `BLOCK_SAMPLES`) y `ysfPhy_push()` añadía el bloque
entero antes de cada `hunt()`; `hunt()` sólo puntúa posiciones con la trama
completa dentro del anillo (`phy.ringIndex - FRAME_SAMPLES`), así que el anillo
de 7 360 muestras ya había pisado las 2 240 posiciones más antiguas de las
4 800 que tocaba explorar. Como una trama dura exactamente un bloque, la fase
del sincronismo respecto de la frontera de bloque no cambia en toda la
transmisión: si cae en esas 2 240, la llamada entera no engancha (47 %).

Arreglo: añadir 2 400 muestras por vuelta y explorar después de cada trozo,
con un `_Static_assert` de que el trozo cabe en el margen del anillo.

Test nuevo (`tests/unit/ysf_phy.cpp`, "a frame handed over in one-frame blocks
is still found"): la misma trama de cabecera entregada en bloques de 4 800 con
cuatro adelantos, uno por cuarto del bloque. **Falla con el código anterior**
(`lead 100`, `REQUIRE(s.frames == 1)` con `0 == 1`) y pasa con el arreglo; los
otros 7 casos del PHY no cambian.

---

## Suite del host

Los mismos comandos del encargo, en este worktree:

```
meson setup build_linux -Dcpp_args=-fpermissive -Dtetra_lib_dir=~/src/c62/forks/libtetra
ninja -C build_linux -k 0            # fallan sólo los targets ARM/miosix, a propósito
ninja -C build_linux sim-coverage.txt
meson test -C build_linux --no-rebuild
```

- `ninja -k 0` termina con errores en los `openrtx_<radio>` ARM (md3x0, gd77,
  dm1801, md9600, cs7000, mduv3x0, rt4d, dm1701, mod17), como se espera, más
  dos de host **que ya fallaban en master y no están en la suite**:
  `linux_inputStream_test` (usa `inputStream_start/stop`, que ya no existen; el
  `test()` está comentado en `meson.build:2445`) y el enlace de
  `openrtx_linux_mod17` (`ui_rtxSyncRequest` sin definir).
- `meson test` **antes** de `sim-coverage.txt` da 112/113: el que falla es
  "Simulator Coverage" (exit 42) porque el manifiesto no existe todavía. Con el
  paso de `ninja` delante, **113/113**.
- El "Simulator Menu Audit" tarda 659 s de reloj; es el test lento de la suite,
  no un cuelgue.

## Cómo se ha comprobado lo que la suite no compila

`OpMode_DMR.cpp`, `OpMode_DPMR.cpp` y `DStarSpeech.c` **no están en
`build_linux`** (la suite del host sólo compila `OpMode_FM`, `OpMode_M17`,
`OpMode_Airband` y `OpMode_WFM`). Para no quedarse en "compila en mi cabeza" se
han compilado sueltos, sin firmware y sin imagen:

```
# C++: los flags del target de simulación + los -DCONFIG_ que hacen falta
c++ <flags de build_linux/compile_commands.json> -fsyntax-only \
    -include <shim con printk> -DCONFIG_DMR_TX -DCONFIG_DMR_SMS \
    openrtx/src/rtx/OpMode_DMR.cpp     # limpio
c++ ... -DCONFIG_DPMR -DCONFIG_DPMR_TX openrtx/src/rtx/OpMode_DPMR.cpp  # limpio
# C: las dos ramas del bloque de atributos
cc ... -DCONFIG_DSTAR [-D__ZEPHYR__] -fsyntax-only \
   openrtx/src/protocols/DSTAR/DStarSpeech.c    # limpio en las dos
```

`OpMode_DMR.cpp` con `-DCONFIG_DMR_TX` y **sin** `-DCONFIG_DMR_SMS` no compila,
pero tampoco compilaba antes del cambio (`dataTxSlot()` usa `dmrSms_txNext()`
sin guarda): configuración que no existe en la C62, que define los dos.
