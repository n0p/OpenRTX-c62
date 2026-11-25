# Protocolo binario radio–PC (CPS y banco)

El cable de programación es el UART de consola. Hasta ahora por ahí hablaban
tres cosas: el log en texto, el volcado de banda base en sus propias tramas con
*magic* y el control remoto con sus líneas de texto (`s`, `tx`, `ptt`, `rx`).
Eso sirve para un banco de pruebas y no sirve para un CPS: sin versiones, sin
capacidades, sin escritura verificada y sin forma de añadir un mensaje sin
inventarse otra sintaxis.

`core/pc_proto` es la capa que lo sustituye. Todo lo que hay debajo es C
portátil y se compila también para el host, así que el mismo código que corre
en la radio corre en los tests.

| fichero | qué hay |
|---|---|
| `openrtx/include/core/pc_proto.h` | el contrato: formato, IDs, tags, capacidades, errores |
| `openrtx/src/core/pc_proto.c` | tramado, sesión, tabla de mensajes, eventos, log |
| `openrtx/src/core/pc_proto_ops.c` | los handlers de los almacenes (canales, contactos, zonas, ajustes, mensajes) |
| `openrtx/src/core/pc_proto_dbg.c` | el espacio de nombres de depuración |
| `platform/targets/c62/pc_proto_c62.c` | transporte por el UART de consola y hooks de plataforma |
| `tools/cps/` | cliente de referencia en Python y su test contra una radio simulada |

Dos opciones de build, **las dos apagadas por defecto**:

```
-DC62_PC_PROTO=ON      el protocolo y su transporte
-DC62_DEBUG_PROTO=ON   además el espacio de nombres de depuración
```

---

## 1. Formato de trama

```
FRAME  := END  cuerpo_escapado  END
END     = 0xC0
ESC     = 0xDB
ESC_END = 0xDC
ESC_ESC = 0xDD
```

El cuerpo, ya desescapado, en little endian:

```
0   u8   versión del sobre (1)
1   u8   tipo: 1 petición, 2 respuesta, 3 evento
2   u8   secuencia (la elige el cliente, la respuesta la repite)
3   u16  identificador de mensaje
5   u8   flags (bit 0: la lista continúa)
6   u16  longitud del payload
8   n    payload: lista de TLV
8+n u16  CRC-16/CCITT-FALSE sobre los bytes 0..8+n
```

### Por qué delimitador *y* longitud *y* CRC

Las tres cosas, y cada una por un motivo distinto:

- **Delimitador con escape (estilo SLIP)**: el receptor no tiene que buscar un
  patrón para saber dónde empieza una trama. Cualquier basura anterior al
  primer `END` se tira, y una trama cortada por un reset se descarta entera en
  el siguiente `END`. Con *magic* (lo que hace el volcado) hay que escanear
  cuatro bytes y una secuencia casual dentro de un payload puede dar un falso
  bloqueo.
- **Longitud dentro del sobre**: una trama truncada se rechaza antes de parsear
  nada. Sin ella, el receptor aceptaría el principio de una trama como trama
  completa y el CRC tendría que decidirlo con menos información.
- **CRC**: el puerto tiene ruido, y un cable de programación a 115200 con el
  PTT en el mismo pad lo tiene de verdad. Un byte cambiado en el `id` con el
  resto correcto es exactamente lo que un CRC evita.

El CRC es **CRC-16/CCITT-FALSE** (polinomio 0x1021, registro inicial 0xFFFF).
Ojo: `crc_ccitt()` de `core/crc.h` es el mismo polinomio pero arrancando en
cero —numéricamente es CRC-16/XMODEM—, así que el protocolo llama a
`crc_ccitt_update(0xFFFF, ...)` explícitamente. Es el mismo CRC que ya usa el
volcado de banda base y su lector (`scripts/c62_bbdump.py`), de modo que un
cliente lleva una sola rutina para todo el cable.

Tamaños: el cuerpo máximo es `PC_PROTO_MAX_BODY` = 400 bytes (payload máximo
390). Una trama con todo escapado ocupa como mucho 452 bytes en el cable. La
instancia completa (`pcProto_t`) son ~1,7 kB de RAM, de los que ~550 son el
anillo del log.

### Resincronización

El receptor es una máquina de tres estados (`rxInFrame`, `rxEsc`,
`rxOverflow`) que se alimenta byte a byte y, por tanto, con cualquier
troceado: da igual que llegue un byte por llamada o un búfer entero.

- Bytes antes del primer `END`: se ignoran.
- `END` con un cuerpo acumulado: se valida y se entrega.
- Cuerpo mayor que el búfer: se marca `overflow` y se descarta **hasta el
  siguiente `END`**; no se entrega nada.
- Un par de escape que no es `ESC_END` ni `ESC_ESC`: se toma literal y decide
  el CRC, porque adivinar la intención de un byte corrupto es peor que
  descartar la trama.

Cada descarte se cuenta (`pcProtoStats_t`): `crcErrors`, `formErrors`,
`overflows`. El cliente lo puede pedir con `PC_MSG_COUNTERS`, y también ve las
pérdidas en el hueco de los números de secuencia del log.

---

## 2. Versiones y capacidades: `HELLO`

El cliente abre la sesión con `HELLO`, que lleva **su** versión de protocolo y
**sus** capacidades:

| tag | tipo | significado |
|---|---|---|
| `PC_TAG_VERSION` 0x0003 | u16 | versión del cliente |
| `PC_TAG_CAPS` 0x0009 | u32 | capacidades que el cliente implementa |

La respuesta es un `HELLO` con:

| tag | tipo | significado |
|---|---|---|
| `PC_TAG_STATUS` 0x0002 | u8 | 0 si la sesión vale, `PC_ERR_VERSION` si el cliente es más viejo que `PC_PROTO_VERSION_MIN` |
| `PC_TAG_VERSION` / `PC_TAG_VERSION_MIN` | u16 | versión del firmware, mínima que sirve |
| `PC_TAG_MODEL` / `PC_TAG_FIRMWARE` / `PC_TAG_HARDWARE` | str | identidad |
| `PC_TAG_CAPS` 0x0009 | u32 | lo que este firmware sirve |
| `PC_TAG_CHCOUNT` / `PC_TAG_CTCOUNT` | u16 | canales y contactos |
| `PC_TAG_CHSTORE_VER` | u16 | versión del registro de canal (`CHSTORE_VERSION`) |
| `PC_TAG_SETTINGS_VER` / `PC_TAG_SETTINGS_SIZE` | u16 | versión y tamaño de `settings_t` |
| `PC_TAG_LASTUSED` | u16 | canal recordado, 0xFFFF ninguno |
| `PC_TAG_UPTIME_MS` | u32 | uptime |
| `PC_TAG_TOKEN` | u32 | testigo de sesión (solo con `PC_CAP_DEBUG`) |

**Degradar, no romper.** Un cliente por debajo del mínimo no recibe un error
seco: recibe el `HELLO` completo con `status = PC_ERR_VERSION`, así que puede
decir "este firmware necesita protocolo ≥ N" en vez de enseñar un *timeout*.
Un mensaje que el firmware no conoce se responde `unknown_msg`; uno que conoce
pero este build no sirve, `not_supported`; ambos con el identificador en
`PC_TAG_MSG_ID`. Un mensaje de depuración en una build de producción responde
`not_allowed`, no `not_supported`: la radio se niega, no finge no entender.

El cliente anuncia `PC_CAP_EVENTS` si quiere eventos; sin eso, el firmware no
emite ninguno (y así un cliente que no los implementa no recibe tramas que no
sabe leer).

### Bitmap de capacidades

| bit | valor | qué habilita |
|---|---|---|
| `PC_CAP_CHANNELS` | 0x0001 | banco de canales |
| `PC_CAP_CONTACTS` | 0x0002 | agenda |
| `PC_CAP_MESSAGES` | 0x0004 | bandeja y cola |
| `PC_CAP_SETTINGS` | 0x0008 | ajustes |
| `PC_CAP_ZONES` | 0x0010 | planes de canales |
| `PC_CAP_STATS` | 0x0020 | estadísticas por modo (`CONFIG_C62_MODE_STATS`) |
| `PC_CAP_PACKET_MODEM` | 0x0040 | reservado |
| `PC_CAP_ANALYZER` | 0x0080 | reservado |
| `PC_CAP_EVENTS` | 0x0100 | eventos asíncronos |
| `PC_CAP_CHAN_BLOCK` | 0x0200 | canales en bloque |
| `PC_CAP_MSG_SEND` | 0x0400 | encolar un mensaje |
| `PC_CAP_SETTINGS_FIELDS` | 0x0800 | ajustes por campos con nombre |
| `PC_CAP_ZONE_IMPORT` | 0x1000 | importar un canal de zona al banco |
| `PC_CAP_DEBUG` | 0x2000 | espacio de nombres de depuración |

---

## 3. Tabla de mensajes

Los rangos están asignados para que dos features no elijan el mismo número:

```
0x0000-0x00FF  núcleo: sesión, liveness, errores, eventos
0x0100-0x01FF  CPS: canales, contactos, zonas, ajustes, estado en vivo
0x0200-0x02FF  mensajería: bandeja, cola, envío
0x0300-0x03FF  packet modem (reservado)
0x0400-0x04FF  analizador y espectro (reservado)
0x0500-0x05FF  depuración
0x0600-0xFEFF  libre, un bloque por feature
0xFF00-0xFFFF  respuestas de error
```

### Núcleo

| id | nombre | petición | respuesta |
|---|---|---|---|
| 0x0001 | `HELLO` | version, caps | identidad, tamaños, caps, token |
| 0x0002 | `PING` | – | uptime, status |
| 0x0003 | `SESSION_CLOSE` | – | status; el firmware suelta el enlace y devuelve el log al cable |
| 0x0004 | `COUNTERS` | – | `pcProtoStats_t` (32 B) en `PC_TAG_STATS` |
| 0x00FF | `ERROR` | – | `PC_TAG_ERROR`, `PC_TAG_MSG_ID` |

### Eventos (tipo 3, nunca se responden)

| id | nombre | contenido |
|---|---|---|
| 0x0080 | `EVT_STATUS` | estado en vivo, periódico si se suscribe |
| 0x0081 | `EVT_CHANNEL` | índice y nombre del canal + estado |
| 0x0082 | `EVT_PTT` | estado de TX + estado |
| 0x0083 | `EVT_MESSAGE` | transporte, peer, texto, no leídos |
| 0x0084 | `EVT_SETTINGS` | versión y CRC del registro nuevo |
| 0x0085 | `EVT_LOG` | una línea de log: secuencia, tiempo, nivel, módulo, texto, descartes |

### CPS

| id | nombre | petición |
|---|---|---|
| 0x0101 | `CHAN_COUNT` | – → `CHCOUNT`, `CHSTORE_VER` |
| 0x0102 | `CHAN_GET` | `INDEX` → `CHANNEL` |
| 0x0103 | `CHAN_SET` | `INDEX` + `CHANNEL` → el registro tal como quedó |
| 0x0104 | `CHAN_ADD` | `CHANNEL` → `INDEX` |
| 0x0105 | `CHAN_DELETE` | `INDEX` → `CHCOUNT` |
| 0x0106 | `CHAN_LIST` | `FIRST`, `COUNT` → `FIRST`, `COUNT`, `CHCOUNT`, `CHANNEL_LIST` |
| 0x0107 | `CHAN_WRITE_BLOCK` | `FIRST` + `CHANNEL_LIST` → `VERIFY` |
| 0x0108 | `CHAN_LASTUSED` | sin `INDEX` lee, con `INDEX` escribe |
| 0x0110-0x0115 | `CONTACT_*` | igual, con `CONTACT`/`CONTACT_LIST` (33 B) |
| 0x0120 | `ZONE_COUNT` | – → `ZONE_COUNT` |
| 0x0121 | `ZONE_GET` | `ZONE` → nombre, prefijo, canales, modo |
| 0x0122 | `ZONE_CHANNEL` | `ZONE` + `INDEX` → `ZONE_CHANNEL` (28 B empaquetado) |
| 0x0123 | `ZONE_IMPORT` | `ZONE` + `INDEX` → el canal creado |
| 0x0130 | `SETTINGS_GET` | sin tags: registro completo; con tags: esos campos |
| 0x0131 | `SETTINGS_SET` | campos por tag → `VERIFY` |
| 0x0132 | `SETTINGS_INFO` | versión, tamaño, CRC, uptime |
| 0x0140 | `STATUS_GET` | – → estado en vivo |
| 0x0141 | `STATUS_SUBSCRIBE` | `PERIOD_MS` (0 apaga) |
| 0x0150 | `STATS_GET` | – → líneas de `core/mode_stats` |

### Mensajería

| id | nombre | petición |
|---|---|---|
| 0x0201 | `MSG_COUNT` | – → bandeja, cola, no leídos |
| 0x0202 | `MSG_INBOX` | `INDEX` → un mensaje |
| 0x0203 | `MSG_OUTBOX` | `INDEX` → un mensaje |
| 0x0204 | `MSG_SEND` | transporte, peer (texto y/o id), texto, flags |
| 0x0205 | `MSG_DELETE` | `MSG_STORE_ID` (cancela; lo que ya está en el aire termina) |
| 0x0206 | `MSG_MARK_READ` | `MSG_STORE_ID` o todos |

### Reservados

`0x0301 PM_TX`, `0x0302 PM_STATUS`, `0x0401 ANA_SPECTRUM`, `0x0402 ANA_STATS`.
La fila existe en la tabla con su capacidad: mientras la capacidad no esté,
responden `not_supported` y no `unknown_msg`, para que un cliente sepa que el
mensaje existe en el protocolo y no en este build.

### Depuración

Ver la sección 6.

---

## 4. Mapa de TLV

Cada tag implica su tipo: los lectores comprueban la anchura, así que un
cliente que manda el tipo equivocado recibe `bad_request` en vez de un campo
mal leído.

**Sesión e identidad**

| tag | tipo | uso |
|---|---|---|
| 0x0001 `ERROR` | u8 | código de `pcProtoError` |
| 0x0002 `STATUS` | u8 | 0 ok |
| 0x0003 `VERSION` | u16 | versión de protocolo |
| 0x0004 `VERSION_MIN` | u16 | mínima servida |
| 0x0005 `MSG_ID` | u16 | mensaje al que se refiere un error |
| 0x0006 `MODEL` / 0x0007 `FIRMWARE` / 0x0008 `HARDWARE` | str | identidad |
| 0x0009 `CAPS` | u32 | capacidades |
| 0x000A `UPTIME_MS` | u32 | uptime |
| 0x000B `VERIFY` | u8 | 1: se leyó de vuelta y coincide |

**Identificadores y cuentas**

| tag | tipo | uso |
|---|---|---|
| 0x0010 `INDEX` | u16 | ranura |
| 0x0011 `FIRST` | u16 | primera ranura de un bloque |
| 0x0012 `COUNT` | u16 | entradas en el payload |
| 0x0013 `ZONE` | u8 | id de zona |
| 0x0016 `ZONE_PREFIX` | str | "R", "U", "PMR" |
| 0x0017 `ZONE_COUNT` | u16 | canales de la zona / zonas del build |
| 0x0018 `CHCOUNT` / 0x0019 `CTCOUNT` | u16 | canales / contactos |
| 0x001A `SETTINGS_VER` / 0x001B `SETTINGS_CRC` / 0x001C `SETTINGS_SIZE` | u16 | registro de ajustes |
| 0x001D `CHSTORE_VER` | u16 | versión del registro de canal |
| 0x001E `LASTUSED` | u16 | canal recordado |
| 0x001F `MSG_STORE_ID` | u8 | id dentro del almacén de mensajes |

**Registros y arrays**

| tag | tipo | contenido |
|---|---|---|
| 0x0040 `CHANNEL` | bytes | un `chstore_channel_t`, 37 B |
| 0x0041 `CHANNEL_LIST` | bytes | `COUNT` × 37 B, seguidos |
| 0x0042 `CONTACT` | bytes | un `cstore_contact_t`, 33 B |
| 0x0043 `CONTACT_LIST` | bytes | `COUNT` × 33 B |
| 0x0044 `ZONE_CHANNEL` | bytes | 28 B empaquetados (abajo) |
| 0x0045 `SETTINGS` | bytes | `settings_t` empaquetado |
| 0x0046 `SETTINGS_REC` | bytes | registro completo con cabecera |
| 0x0047 `STATS` | bytes | texto de `mode_stats` |

`chstore_channel_t` (37 B, empaquetado, little endian):

```
0   16s  nombre, NUL terminado
16  u32  rx_freq, Hz
20  u32  tx_freq, Hz
24  u16  potencia, mW
26  u8   modo (enum opmode)
27  u8   ancho de banda
28  u8   rx_tone_en:1 | rx_tone:7
29  u8   tx_tone_en:1 | tx_tone:7
30  u8   dmr_cc:4 | dmr_ts:4     (ts 0 = TS1, 1 = TS2)
31  u8   flags (0x01 rx_only, 0x02 simplex, 0x04 skip)
32  u8   dmr_dest_type
33  u32  dmr_dest_id (24 bits usados)
```

`cstore_contact_t` (33 B): `call[10]`, `name[17]`, `u32 dmr_id`, `u8 mode`,
`u8 flags` (0x01 grupo).

`zone_channel_t` en el cable (28 B, empaquetado a mano porque el de RAM lleva
relleno de alineación): `u8 zone`, `u16 index`, `name[8]`, `u32 rx`, `u32 tx`,
`i32 shift`, `u8 mode`, `u8 bandwidth`, `u16 power`, `u8 flags`.

**Estado en vivo**

| tag | tipo |
|---|---|
| 0x0080 `RX_FREQ` / 0x0081 `TX_FREQ` | u32 Hz |
| 0x0082 `MODE` | u8 |
| 0x0083 `RSSI` | i16 dBm |
| 0x0084 `TX_STATE` | u8 (0 apagado, 1 recibiendo, 2 transmitiendo) |
| 0x0085 `CHANNEL_IDX` | u16 (0xFFFF en VFO) |
| 0x0086 `BATTERY_MV` | u16 |
| 0x0087 `SQUELCH` | u8 |
| 0x0088 `PERIOD_MS` | u16 |
| 0x0089 `CHANNEL_NAME` | str |
| 0x008A `TX_POWER` | u16 mW con la que la radio transmitiría (la del canal, o el ajuste) |
| 0x008B `PORT_MODE` | u8 `enum portMode`: 0 auto, 1 always, 2 never (docs/ACCESSORY.md) |
| 0x008C `PORT_PTT` | u8, 1 si el pulsador del microaltavoz está abajo ahora mismo (solo en `DBG_INFO`) |
| 0x008D `PORT_LOST` | u32 bytes y errores de tramado tirados porque el pulsador tenía el pad |
| 0x00EF `TX_REASON` | str, `none` o por qué no transmitiría (docs/TX_SAFETY.md) |

`TX_POWER` y `TX_REASON` viajan en **todo** estado (respuesta de `STATUS_GET` y
eventos `EVT_STATUS`), y también en `DBG_INFO`: un PTT que no hace nada se
diagnostica por el cable, sin abrir la radio.

`PORT_MODE` y `PORT_LOST` viajan con el estado y con `DBG_INFO`; `PORT_PTT` solo
en `DBG_INFO` (es un instante, no un estado que tenga sentido cachear). Son la
respuesta a «el CPS se queda callado mientras alguien aprieta el PTT del
microaltavoz»: el modo dice quién es dueño del pad y el contador, cuántas tramas
costó. Todo el asunto está en `docs/ACCESSORY.md`.

**Mensajes**: 0x00A0 transporte, 0x00A1 peer (str), 0x00A2 peer id (u32),
0x00A3 texto, 0x00A4 estado, 0x00A5 flags, 0x00A6 no leídos, 0x00A7 bandeja,
0x00A8 cola.

**Ajustes por nombre**: 0x00C0 callsign, 0x00C1 brillo, 0x00C2 contraste,
0x00C3 squelch, 0x00C4 vox, 0x00C5 zona horaria (i8, medias horas),
0x00C6 DMR id, 0x00C7 destino DMR, 0x00C8 tipo de destino, 0x00C9 timeslot,
0x00CA M17 CAN, 0x00CB destino M17.

---

## 5. Escritura: validar, escribir, leer de vuelta

La regla es la misma para todos los almacenes y está en un solo sitio
(`chanStoreVerified()` y su gemelo de contactos):

1. **Validar el registro entero** con el propio validador del almacén
   (`chstore_valid`, `cstore_valid`). Un bloque con una entrada mala no escribe
   **nada**: se comprueban todas antes de tocar la flash.
2. **Escribir** con la función del almacén, que ya usa el ping-pong de dos
   ranuras (ver `docs/CHANNELS.md`): un corte de corriente a mitad de guardado
   cuesta el guardado y nunca el banco anterior.
3. **Leer de vuelta y comparar** byte a byte. Si no coincide, la respuesta es
   `PC_ERR_IO`, nunca un éxito: un CPS que cree una escritura que no quedó deja
   al operador con un codeplug que no es el de la radio.

Los ajustes siguen el mismo criterio en RAM: se aplica sobre una copia, se
valida cada campo, se copia a `state.settings` bajo el mutex, se relee y se
compara. El registro llega a la flash por el *debounce* de `state_task()`, el
mismo camino que una edición con el teclado; la respuesta lleva versión y CRC
del registro resultante.

Un bloque de canales se escribe por registro (cada uno atómico) y verificado
uno a uno; si uno falla, la respuesta dice cuál y cuántos quedaron
(`PC_TAG_INDEX`, `PC_TAG_COUNT`) para que el cliente reintente desde ahí.

---

## 6. Espacio de nombres de depuración

Depurar la radio sin abrirla ni mirar un LED: eso es lo que hay aquí. Está
detrás de tres puertas y **todas** tienen que abrirse.

| id | nombre | qué hace |
|---|---|---|
| 0x0501 | `DBG_INFO` | versiones, uptime, motivo del reinicio, watchdog, heap, PSRAM, flash, RAM, pilas por tarea, descartes del log |
| 0x0502 | `DBG_LOG_SUB` | suscribe el log: nivel mínimo, filtro por módulo, caudal |
| 0x0503 | `DBG_LOG_DRAIN` | cuántas líneas esperan, cuántas se perdieron |
| 0x0510 | `DBG_REG_READ` | un registro del BK4819 o del BK1080 |
| 0x0511 | `DBG_REG_WRITE` | escritura con lista blanca, testigo y confirmación |
| 0x0512 | `DBG_AGC` | RSSI filtrado, RSSI crudo (REG_67), squelch |
| 0x0520 | `DBG_MEM_READ` | bloque de memoria acotado (máx. 128 B) |
| 0x0530 | `DBG_CAP_START` | arranca una captura de audio RX (máx. 16000 muestras) |
| 0x0531 | `DBG_CAP_STOP` | la para |
| 0x0532 | `DBG_CAP_READ` | lee muestras (128 por trama) |
| 0x0540 | `DBG_BB` | estado y disparo del volcado de banda base |
| 0x0550 | `DBG_TX_TRACE` | traza viva del ciclo de transmisión (texto) |
| 0x0551 | `DBG_RX_COUNTERS` | contadores por modo, igual que el comando `rx` |

### Las tres puertas

1. **`CONFIG_C62_DEBUG_PROTO`**. Sin él, `PC_CAP_DEBUG` no se anuncia, el
   dispatcher responde `not_allowed` antes de mirar la petición y los handlers
   están compilados como stubs. Una build de producción no lleva el código ni
   lo anuncia.
2. **El testigo de sesión**. `HELLO` lo entrega en `PC_TAG_TOKEN` y rota en
   cada `HELLO` (un segundo cliente invalida al primero). Las escrituras —y
   las lecturas de memoria y de captura— lo tienen que repetir.
3. **La lista blanca y los límites**. La lectura de registros cubre el fichero
   legible de cada chip (BK4819 0x00-0x7F, BK1080 0x00-0x21). La **escritura**
   solo admite lo que hay en una tabla de seis entradas —registros que el
   driver reescribe en el siguiente cambio de modo o de frecuencia, así que un
   valor raro puede dejar la radio sorda un momento pero no para siempre—:

   | chip | reg | qué es |
   |---|---|---|
   | BK4819 | 0x30 | camino de audio |
   | BK4819 | 0x40 | desviación de TX |
   | BK4819 | 0x47 | nivel de AF |
   | BK4819 | 0x48 | ganancias de audio RX |
   | BK4819 | 0x7D | ganancia de micrófono |
   | BK1080 | 0x02 | alimentación y mute |

   La tabla está en `pc_proto_dbg.c` y es el único sitio que hay que tocar para
   ampliarla. La memoria se limita a la SRAM y a la ventana de PSRAM, y la
   lectura a 128 bytes por petición, pida lo que pida el cliente.

**Qué queda expuesto.** Con el espacio de nombres activo, quien tenga el cable
puede leer la memoria de la radio, sus registros, su log, 333 ms de audio RX y
escribir seis registros del transceptor. Puede además reiniciar el volcado de
banda base, que ocupa PSRAM y toma el UART. El testigo **no es** una frontera
de seguridad: se lee de la propia respuesta de `HELLO`. Es un seguro contra un
cliente despistado o una sesión mezclada, y así está escrito aquí y en
`pc_proto.h`. Contra alguien con el cable y ganas, la única defensa es no
compilar la opción. Eso es exactamente lo que hace una build de producción:
`C62_DEBUG_PROTO=OFF` (el valor por defecto).

### Log en streaming, sin poder inundar

El transporte no deja que el log salga como bytes crudos nunca más: el hook de
`printk` mete cada línea en un anillo de 4 líneas y, **si no hay cliente**,
además la escribe al UART como siempre. Con cliente enlazado, la línea solo
puede salir dentro de un `EVT_LOG`. Por eso una línea de log no puede corromper
una trama: no comparten el cable.

- Nivel mínimo y filtro por módulo: el prefijo de Zephyr
  (`[00:00:01.234] <inf> net: texto`) se parsea para sacar nivel y módulo. Una
  línea que no encaja queda en `info` con módulo `fw`, que es la suposición
  segura.
- **Secuencia y tiempo**: cada línea lleva `PC_TAG_LOG_SEQ` (contador que
  sube con cada línea *encolada*) y `PC_TAG_UPTIME_MS`. Un salto en la
  secuencia es una pérdida.
- **Caudal limitado y descarte contado**: cubo de fichas de `PC_TAG_RATE`
  líneas por segundo (por defecto 20, máximo 200), con cuatro líneas de crédito
  como mucho: un atraso no se suelta de golpe. Lo que no cabe en el anillo o en
  el caudal se cuenta en `PC_TAG_DROPPED`, que viaja en cada trama y también en
  `DBG_INFO`. Suscribirse empieza un flujo nuevo: lo que hubiera en el anillo
  antes se tira, para no mandar líneas que no pasan el filtro recién puesto.

### Captura de audio

`DBG_CAP_START` reserva hasta 16000 muestras (0,33 s a 48 kHz, en PSRAM) y
`pc_proto_capturePush()` las copia desde el worker del códec. `DBG_CAP_READ`
las entrega en tramas de 128 muestras s16. La captura se para sola al llenarse
y lo dice (`PC_TAG_RUNNING` a 0, overflow marcado).

### Volcado de banda base

`DBG_BB` lee el estado (0 apagado, 1 grabando, 2 en pausa, bit 7 si el build no
lleva volcado) y lo arranca o lo para con el testigo. El transporte pausa el
volcado mientras hay cliente enlazado y lo reanuda al soltarlo: sus paquetes y
nuestras tramas no se mezclan en el cable.

---

## 7. Añadir un mensaje nuevo en tres pasos

1. **Elegir id y capacidad.** Un id libre del bloque de la feature
   (`enum pcProtoMsg`) y, si el build puede no tenerla, un bit de capacidad
   (`enum pcProtoCap`). Si el payload necesita campos nuevos, un tag por campo
   en `enum pcProtoTag` con su tipo.
2. **Añadir la fila a `pc_table[]`** en `pc_proto.c`:
   `{ PC_MSG_LO_NUEVO, PC_CAP_LO_NUEVO, pc_ops_loNuevo }`. Con eso ya responde
   `not_supported` cuando la capacidad no está, sin escribir una línea más.
3. **Escribir el handler** (`int pc_ops_loNuevo(struct pcReq *r)`), declararlo
   en `pc_proto_internal.h`, leer los tags con `pc_tlv_get_*`, escribir la
   respuesta con `pc_tlv_*` en `r->out` y devolver 0. Para un error:
   `r->err = PC_ERR_...; return -1;`.

Si el mensaje es de un bloque reservado que todavía no existe, no hay nada que
hacer: la fila con su capacidad ya está y responde lo correcto.

---

## 8. Ejemplo de sesión

Cliente (Python de `tools/cps`) contra la radio. Se abre la sesión, se lee el
canal 0, se le cambia el nombre, se comprueba y se cierra:

```
$ tools/cps/cps.py -d /dev/ttyUSB0 hello
model        c62
firmware     OpenRTX c62  (hardware rev 0)
protocol     v1 (min served v1, client v1)
capabilities channels contacts messages settings zones events chan_block
             msg_send settings_fields zone_import debug
channels     12  (bank format v2)
contacts     3
settings     v6, 210 bytes
last used    4
debug token  0x9c4a1f02

$ tools/cps/cps.py -d /dev/ttyUSB0 chan get 0
{ "name": "CALL1", "rx_freq": 145500000, "tx_freq": 145500000,
  "power": 1000, "mode": 1, "dmr_cc": 0, "dmr_ts": 0, "flags": 2, ... }

$ tools/cps/cps.py -d /dev/ttyUSB0 chan set 0 --name REPEATER --rx 145.600M
written and read back:
  0  REPEATER           145.600000 MHz  fm    12.5k  1000mW  simplex
```

Los bytes de esa escritura, para quien esté depurando el cable:

```
c0                          END
01 02 14 03 01 00 00 26 00  sobre v1, respuesta, seq 0x14, id 0x0103 (CHAN_SET)
...                          payload: INDEX=0 + CHANNEL (37 B)
ab cd                       CRC-16/CCITT-FALSE
c0                          END
```

Una sesión de depuración:

```
$ tools/cps/cps.py -d /dev/ttyUSB0 diag
firmware    OpenRTX c62  hw rev 0  model c62
protocol    v1 (min v1)
uptime      1284.7 s   reset cause 0x0 (unknown)   wdt 0
heap        18944 / 40960 B free
psram       1105920 / 4194304 B free
flash       601132 / 4194304 B
ram         219704 / 262144 B
agc         unknown
stack       main        1024 free of 2048
stack       pcproto     3100 free of 4096
log dropped 0

$ tools/cps/cps.py -d /dev/ttyUSB0 log --level info --module dmr --seconds 5
# subscribed: level<=2 rate=20 lines/s module='dmr' dropped=0
   3.204s INF   dmr      sync lost
   3.480s INF   dmr      slot 2 idle

$ tools/cps/cps.py -d /dev/ttyUSB0 reg read --chip bk4819 --reg 0x30
0x30 = 0x1234

$ tools/cps/cps.py -d /dev/ttyUSB0 reg write --chip bk4819 --reg 0x30 --value 0x4321
0x30 <- 0x4321, read back 0x4321 (verified)

$ tools/cps/cps.py -d /dev/ttyUSB0 reg write --chip bk4819 --reg 0x10 --value 1
error: invalid_value (dbg_reg_write)          # 0x10 no está en la lista blanca

$ tools/cps/cps.py -d /dev/ttyUSB0 mem read --addr 0x20000000 --len 32
error: invalid_value (dbg_mem_read)           # fuera de la ventana permitida

$ tools/cps/cps.py -d /dev/ttyUSB0 tx
tx stage=idle slots=0 | last over ok, 0 retries
```

Contra una build de producción, lo mismo:

```
$ tools/cps/cps.py -d /dev/ttyUSB0 diag
this firmware has no debug capability (C62_DEBUG_PROTO off)

$ tools/cps/cps.py -d /dev/ttyUSB0 reg read --chip bk4819 --reg 0x30
refused: not_allowed (dbg_reg_read)
```

---

## 8.bis El transporte nunca espera para siempre

Una sola hebra —`pcProtoMain` en `platform/targets/c62/pc_proto_c62.c`— lee el
UART, despacha y escribe la respuesta. Es el único sitio del firmware que puede
contestar a un `HELLO`, así que **cualquier cosa que la bloquee deja la radio
muda entera**: sin respuesta, sin traza, sin línea de log que lo explique. Eso
es exactamente lo que se vio con un canal guardado aplicado al arrancar: la
pantalla en NFM 145.5, las teclas respondiendo, el log del SDK saliendo una vez
por segundo y ni un `HELLO`, ni en crudo, obteniendo contestación.

Las reglas que se derivan de ahí, y que el código cumple ahora:

1. **La instantánea de estado no toma `state_mutex`.** `pc_proto_platformStatus()`
   lee palabras alineadas y las copia; un valor de un tick antes sigue siendo la
   respuesta correcta. La UI tiene ese mutex mientras corre `ui_updateFSM()` y la
   tarea de estado mientras lee la batería: esperarlo es aceptar quedar muda. Es
   el mismo trato que `vfo_search_service.c` da al mismo estado.
2. **Donde el cerrojo es imprescindible** (una escritura que tiene que ver un
   `state.channel` coherente: `TUNE`, el tono del banco, los ajustes) se toma con
   `state_tryLock(20)` y, si no llega, se contesta `busy`. Un cliente que
   reintenta acaba servido; una radio bloqueada no acaba nunca.
3. **El hilo se vigila desde fuera.** Cuenta sus vueltas (`loopSeq`) y los bytes
   que ha leído (`rxBytes`); `pc_proto_c62_supervise()`, llamada desde el hilo
   principal junto al refresco del watchdog, imprime una línea si no ha dado una
   vuelta en 2 s:

   ```
   pcproto: STALLED 2043ms | loop=184322 rx=512 frames=3 req=1 lastErr=0000/0000 linked=0
   ```

   `rx` dice si el cable y el pad están vivos, `frames` si el tramado valida, y
   `req` si fue un handler el que se quedó. Es la primera cosa que hay que mirar
   si el CPS vuelve a quedarse callado.

4. **La frecuencia y la potencia de TX no se leen del canal a pelo**: salen de
   `state_txFrequency()` y `state_txPowerMw()`, que tampoco toman cerrojo
   (docs/TX_SAFETY.md).

Las pruebas de host lo fijan: `tests/host/pc_proto_host` arranca la radio con un
canal guardado, en VFO, en VFO con potencia 0, y con `state_mutex` tomado por
otra hebra durante 1,5 s. En los cuatro casos `HELLO` y `status` contestan, y las
escrituras que necesitan el cerrojo contestan `busy` en vez de callarse.

## 9. Convivencia con el log y con el volcado

Mientras no hay cliente, el cable es el de siempre: arranque, `printk`, y el
volcado si se armó con A/B. En cuanto llega una trama válida:

- el log deja de ir al UART y solo puede salir dentro de `EVT_LOG`;
- el volcado de banda base se pausa (sus paquetes y las tramas se pisarían).

Al cerrar con `SESSION_CLOSE`, o tras 15 s sin una trama válida, el enlace se
suelta, el volcado se reanuda y el log vuelve al UART. La radio no se queda
muda porque el PC se haya ido.

El UART va a 115200 8N1, el mismo que el resto del firmware. El protocolo toma
la línea RX del pad del PTT (GPIOA_15), que es **también el pulsador del
microaltavoz** del accesorio. Quién manda en ese pad lo elige el operador en
**Ajustes ▸ Radio ▸ Puerto PC** (`Auto` / `Always` / `Never`): con `Auto`, el
pad sigue siendo UART RX y se lee además como GPIO, y un nivel bajo sostenido más
de 30 ms se toma por el pulsador, descartando y contando lo que llegue mientras
tanto; con `Always`, el cable manda y el pulsador no se lee (el comportamiento
de siempre); con `Never`, el pad es GPIO y esta interfaz no existe. El estado lo
cuenta en `PORT_MODE`/`PORT_PTT`/`PORT_LOST`; todo, en `docs/ACCESSORY.md`.

---

## 10. Pruebas

**Host, en el árbol de meson** (contra los almacenes reales, con una flash
falsa que se puede cortar a mitad de escritura):

```
$ meson setup b -Dcpp_args=-fpermissive && ninja -C b pc_proto_test pc_proto_dbg_test
$ ./b/pc_proto_test        # 18 casos, 402 comprobaciones
$ ./b/pc_proto_dbg_test    # 22 casos, 430 comprobaciones
```

Cubren: tramado con bytes que colisionan con los delimitadores, basura y trama
truncada con resincronización, CRC corrupto descartado y contado, `HELLO` con
versión por debajo del mínimo, mensaje desconocido contra mensaje reservado,
escritura de un canal y lectura de vuelta contra el banco, corte de flash con
el banco anterior intacto, bloque con un registro malo que no escribe nada,
ajustes con versión y CRC, evento que solo va a quien lo pidió, suscripción de
estado, importación de zona, y todo el espacio de nombres de depuración con y
sin la capacidad.

**Extremo a extremo, por un pty** (el cliente que se envía contra una radio
simulada que es el código real compilado para el host):

```
$ tests/host/pc_proto_host/run.sh    # 43 casos
```

Es el que prueba el par: tramado, sesión, almacenes, eventos, flujo de log con
sus pérdidas, registros con y sin testigo, memoria acotada, captura, y que la
build de producción responde `not_allowed` a los trece mensajes de depuración
sin filtrar un solo byte.

---

## 11. Coste

Medido con la tabla de memoria del enlace, misma rama y misma configuración,
cambiando solo las dos opciones:

| build | FLASH | RAM | PSRAM |
|---|---|---|---|
| sin protocolo | 479 944 B | 166 848 B | 387 156 B |
| `C62_PC_PROTO=ON` | 490 440 B | 173 000 B | 387 156 B |
| `C62_PC_PROTO=ON C62_DEBUG_PROTO=ON` | 493 756 B | 173 016 B | 419 156 B |

Es decir: **+10 496 B de flash y +6 152 B de RAM** por el protocolo, y **+3 316 B
de flash y +16 B de RAM** por el espacio de nombres de depuración (más 32 000 B
de PSRAM por el búfer de captura, que es lo único que se lleva la PSRAM).

De esos 6 152 B de RAM, 4 096 son la pila del hilo del transporte y el resto la
instancia del protocolo (~1,7 kB: 400 de cuerpo de recepción, 452 de trama de
salida, 390 de payload de respuesta y ~550 del anillo de log), más el estado del
hilo. En una build de producción sin la segunda opción el código del espacio de
nombres no se enlaza: los trece handlers quedan en stubs que solo devuelven
`not_allowed`.
