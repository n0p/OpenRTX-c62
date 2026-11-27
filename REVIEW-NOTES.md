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
