# Mensajería: SMS de DMR, TETRA SDS y APRS en una sola cola

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre la capa de mensajería —**un solo almacén y una sola cola** para APRS, DMR
SMS y TETRA SDS (`core/messaging.*`)—, el SMS de DMR
(`protocols/DMR/DmrSms.*`), el encolado del SDS de TETRA
(`protocols/TETRA/TetraSdsEnc.*`), el enganche con APRS y la capa fina que usa
la pantalla (`core/msg_service.*`). La pantalla en sí está en `docs/UI.md`.

Nada más guarda mensajes: el antiguo `core/msg_store.*` (inbox, enviados y
dos borradores en `0x10000` de la partición de canales) ya no existe, y con él
se fueron los hooks débiles `msg_dmr_sms_*` / `msg_tetra_sds_*`. DMR SMS y
TETRA SDS hablan con `core/messaging` **directamente**.

---

## 1. Qué entra

| pieza | ficheros | qué hace |
|---|---|---|
| SMS de DMR | `include/protocols/DMR/DmrSms.h`, `src/protocols/DMR/DmrSms.c`, `src/protocols/DMR/DmrSmsApp.c` (`DmrSmsApp.h` privado) | codificador y decodificador de las **siete variantes** que se ven en el aire, cabecera de datos, CRC-16 y CRC-32, reensamblado por timeslot y respuesta de entrega |
| ráfaga de datos | `DmrTx.c` (`dmrTx_dataSlot`, `dmrTx_dataTerminator`) | 96 bits de información + tipo de dato → 144 símbolos → 1440 muestras a 48 kHz, con la cadencia TDMA de siempre |
| integración en el modo | `src/rtx/OpMode_DMR.cpp` | datos recibidos → reensamblador del timeslot; mensaje recibido → cola + ACK; mensaje en cola → transmisión ráfaga a ráfaga cuando no hay voz |
| cola común | `include/core/messaging.h`, `src/core/messaging.c`, `src/core/messaging_store_zephyr.c` | **el único** modelo de mensaje: outbox de 6, inbox de 6, estados, reintentos, borrador y registro en flash en su par de sectores |
| capa de la pantalla | `include/core/msg_service.h`, `src/core/msg_service.c` | qué transporte puede enviar en el modo actual, límites de texto, traducción destinatario → dirección (contacto o dígitos) y el enganche de APRS con su buzón |
| APRS | `src/core/messaging_aprs.c` | mueve lo encolado para APRS al buzón de `aprs_service` (llamado desde `OpMode_FM`) |
| SDS de TETRA | `include/protocols/TETRA/TetraSdsEnc.h`, `src/protocols/TETRA/TetraSdsEnc.c` + `tetraSds_prepareQueued()` en `TetraSds.c` | codificador de uplink (U-SDS-DATA, GSM 7 bits / UCS-2) probado contra libtetra; el mensaje queda **encolado y codificado**, no se emite (no hay cadena de TX de TETRA) |
| pruebas | `tests/platform/c62_sms_host.c`, `tests/platform/c62_messaging_host.c`, `tests/platform/c62_sms_cost.c`, `tests/unit/assets/dmr_sms_ref.txt` | ida y vuelta, vectores de referencia, lazo TX→RX por la PHY real, coste en el M33 |

Opciones de compilación (todas ON por defecto en la C62):

```
-DCONFIG_C62_DMR_RX=y   modo DMR (necesario para el SMS)
-DCONFIG_DMR_TX=y       cadena de transmisión (voz y datos)
-DCONFIG_DMR_SMS=y      SMS de DMR (DmrSms.c); necesita CONFIG_MESSAGING
-DCONFIG_MESSAGING=y    cola y almacén únicos (messaging.c + registro en flash)
-DCONFIG_C62_APRS=y     APRS en FM (y su enganche con la cola)
```

---

## 2. Por dónde va el SMS de DMR

### 2.1 Las siete variantes

| app (`enum dmrSmsApp`) | cabecera | payload |
|---|---|---|
| `DMR_SMS_APP_DD_UTF16BE` | `DD_HEAD`, DPF 0xD, SAP 0xA, DD 0x14 | UTF-16BE |
| `DMR_SMS_APP_DD_UTF8` | `DD_HEAD`, DD 0x12 | UTF-8 |
| `DMR_SMS_APP_IP` | `U_HEAD`/`C_HEAD`, DPF 2/3, SAP 4 | IPv4+UDP 5016, UTF-16BE (TS 102 361-3 §5.7.1) |
| `DMR_SMS_APP_IP_HC` | `U_HEAD`, SAP 3 | cabecera UDP/IPv4 comprimida, DPID 1 = puerto 5016 |
| `DMR_SMS_APP_ANYTONE` | `U_HEAD`, SAP 4 | UDP 5016, `00 0d 00 0a` + UTF-16LE + NUL |
| `DMR_SMS_APP_TMS` | `U_HEAD`, SAP 4 | UDP 4007, registro TMS, CR LF + UCS-2 LE |
| `DMR_SMS_APP_HYTERA` | `DD_HEAD`, DD BCD | 2 octetos + UTF-16LE + NUL |

En recepción, `IP_HC` se reporta como `ETSI_IP`: por el aire son el mismo texto
y sólo cambia cómo se comprime la cabecera al transmitir.

### 2.2 Cabecera, CRC y respuesta

Numeración de bits = orden de transmisión (bit 0 = primer bit = MSB del octeto
0), como en las figuras de ETSI TS 102 361-1:

* **CRC-16** de la cabecera sobre los 96 bits con el campo a cero, XOR con la
  máscara del tipo de dato (`0xCCCC` para data header, §B.3.12). En recepción
  se deshace la máscara y el residuo debe ser cero.
* **CRC-32** del paquete (§B.3.9), polinomio `0x04C11DB7`, init 0, y su rareza:
  los octetos se procesan **en pares con el segundo primero**; el CRC se
  coloca con el octeto menos significativo delante.
* **Relleno**: `POC` (octetos) en DPF 2/3, `Bit Padding` en `DD_HEAD`; sin él el
  receptor no puede recortar el último bloque y el CRC cubre otro tramo.
* **Respuesta de entrega**: `C_RHEAD`, DPF 1, con Clase/Tipo/Estado en los bits
  72..79 (`dmrSms_buildResponse`). Un `A=1` en la cabecera recibida dispara un
  ACK automático. TMS y AnyTone confirman en la capa de aplicación (registro
  TMS tipo `0x1F`, `dmrSms_encodeTmsAck`): su cabecera **nunca** pide respuesta
  de capa 2, porque una radio real contestaría a una llamada de datos que no
  abrió.

### 2.3 Transmisión

* Un mensaje ocupa una llamada de datos: cabecera + N bloques rate 1/2 + un
  **TD_LC** de terminación (`dmrTx_dataTerminator`, FLCO 110000) + un slot de
  cola.
* **Cadencia**: una ráfaga por trama TDMA (una cada 60 ms), en el timeslot del
  modo, igual que la voz. Un mensaje de 4 bloques ocupa 6 tramas ≈ 360 ms de
  aire.
* **Nunca pisa la voz**: `needDataTx()` exige que no haya PTT, que TX no esté
  deshabilitado y que el modo esté en RX. Un mensaje encolado durante una
  llamada espera a que termine; un ACK tiene prioridad sobre un mensaje.
* La codificación de un mensaje usa el **formato elegido en los ajustes**
  (`settings.sms_format`, ver §2.4): con `auto` es `IP_HC`, la cabecera más
  corta, y entran ~150 caracteres en un paquete.

### 2.4 El formato de envío es un ajuste

`settings.sms_format` (`enum dmrSmsFormat`, `include/core/datatypes.h`) elige
cuál de las siete variantes se **transmite**:

| valor en el ajuste | app que se construye |
|---|---|
| `DMR_SMS_FMT_AUTO` (0, por defecto) | `DMR_SMS_APP_IP_HC` |
| `DMR_SMS_FMT_DD_UTF16BE` | `DMR_SMS_APP_DD_UTF16BE` |
| `DMR_SMS_FMT_DD_UTF8` | `DMR_SMS_APP_DD_UTF8` |
| `DMR_SMS_FMT_IP_5016` | `DMR_SMS_APP_IP` |
| `DMR_SMS_FMT_IP_HC` | `DMR_SMS_APP_IP_HC` |
| `DMR_SMS_FMT_ANYTONE` | `DMR_SMS_APP_ANYTONE` |
| `DMR_SMS_FMT_TMS` | `DMR_SMS_APP_TMS` |
| `DMR_SMS_FMT_HYTERA` | `DMR_SMS_APP_HYTERA` |

`dmrSms_appForFormat()` hace la traducción y **degrada a `IP_HC`** con un valor
fuera de rango (un registro de ajustes de otra compilación no puede dejar el SMS
mudo). `auto` es exactamente lo que este firmware enviaba antes del ajuste: una
actualización no cambia lo que sale por el aire hasta que el operador elija otro
formato.

La elección importa porque cada fabricante lee uno: ETSI (DD o IP 5016) es lo
estándar, TMS es Motorola, AnyTone y Hytera tienen sus prefijos. Con un solo
formato elegido, un mensaje a una radio de otra marca se puede ver como datos
crudos.

### 2.5 El DMRID propio va en la cabecera

`settings.dmr_id` (24 bits, 1..16777215, validado con `settings_dmrIdValid()`)
es la identidad de la radio: se copia a `rtxStatus.dmrSourceId` en
`threads.c` y de ahí al **campo origen de cada cabecera** —link control de voz
(`dmrTx_linkControl`), cabecera de datos del SMS (`req.src` en
`OpMode_DMR::startDataTx`), terminador (`dmrTx_dataTerminator`) y respuesta de
entrega—. Sin DMRID válido el modo DMR **no transmite** voz: una llamada desde
la dirección 0 no la puede contestar nadie. El valor por defecto es 0 («sin
configurar»), así que el operador tiene que ponerlo una vez.

### 2.6 El destino DMR de la llamada

`settings.dmr_dest_type` (`DMR_DEST_NONE` / `DMR_DEST_GROUP` / `DMR_DEST_USER`)
y `settings.dmr_dest_id` son el destino por defecto; un canal del banco puede
llevar el suyo (`chstore_channel_t.dmr_dest_type/dmr_dest_id`) y
`chstore_applyRtx()` lo pone en `rtxStatus`. El modo DMR construye el link
control con él:

* `DMR_DEST_GROUP` → FLCO `0x00`, llamada de grupo al talkgroup;
* `DMR_DEST_USER` → FLCO `0x03`, llamada de unidad a un DMRID concreto;
* `DMR_DEST_NONE` → **no se transmite**: no hay a quién llamar.

En un SMS el destino es el de la dirección del mensaje (`msg->dst`) y el tipo lo
decide el modo: grupo salvo que el destino configurado sea `USER`.

---

## 3. La cola común (`core/messaging.h`)

```c
struct message {
    uint32_t src, dst;      /* direcciones del transporte */
    uint32_t time;          /* ms de uptime al encolar/recibir */
    uint16_t ref;           /* referencia del transporte (secuencia TMS, ...) */
    uint8_t  id, transport, state, flags, attempts, textLen;
    char     peer[12];      /* dirección en texto (indicativo APRS) */
    char     text[MSG_TEXT_MAX + 1];   /* 160 octetos UTF-8 */
};
```

* `transport`: `MSG_TRANSPORT_APRS`, `MSG_TRANSPORT_DMR`, `MSG_TRANSPORT_TETRA`.
* `state`: `FREE`, `QUEUED`, `SENDING`, `SENT`, `DELIVERED`, `FAILED`,
  `CANCELLED`.
* `flags`: `UNREAD`, `CONFIRM` (se pidió confirmación), `INBOUND`, `TRUNCATED`.

Dos formas de encolar: `messaging_send(transporte, dst, texto, confirmar, now)`
para quien ya tiene la dirección (DMR: ID de radio o talkgroup; TETRA: SSI) y
`messaging_sendAddr(transporte, dst, peer, texto, ...)` para añadir además el
nombre que se pinta en pantalla. APRS usa `messaging_sendToText()`, que guarda
el indicativo y deja un hash en el campo numérico.

Cada transporte bombea su propia cola:

```
const struct message *messaging_head(MSG_TRANSPORT_x)   /* el más antiguo */
int  id = messaging_begin(MSG_TRANSPORT_x)              /* lo marca SENDING */
     ...transmitir...
messaging_done(id, entregado, ref, now)                 /* SENT/DELIVERED/QUEUED/FAILED */
messaging_confirmed(id, peer, now)                      /* ACK que llega después */
```

* `messaging_done(..., false, ...)` reintenta hasta `MSG_MAX_ATTEMPTS` (3) y
  luego `FAILED`.
* Un mensaje con `CONFIRM` queda en `SENDING` hasta que llega la confirmación;
  `messaging_task(now)` lo devuelve a la cola a los 30 s sin respuesta (y lo
  da por fallido si ya agotó los intentos).
* `MSG_STATE_CANCELLED`: lo cancela el operador y **mantiene** ese estado
  aunque el transporte termine la ráfaga que ya tenía empezada (su resultado
  llega tarde y se ignora). Una entrada cancelada no vuelve a elegirse.
* **Persistencia**: registro empaquetado con magic `MSG1`, versión 2, y CRC-16
  en dos sectores alternos de la partición de almacenamiento (`0x4000` y
  `0x5000`, mapa completo en §3.2). Un corte de luz durante la escritura deja
  la copia anterior intacta. Un mensaje que estaba en el aire se reencola al
  arrancar: su resultado no se conoce.

### 3.1 La capa de la pantalla (`core/msg_service.*`)

La pantalla no habla con los transportes ni con direcciones numéricas: llama a
`msg_service`, que traduce y encola.

```c
/* destinatario tal y como está en pantalla + texto -> cola */
int id = msg_service_queue(MSG_TRANSPORT_DMR, "EA5ABC-7", texto_utf8, true,
                           (uint32_t)getTick());

/*   APRS: el indicativo, tal cual (la dirección es texto).
     DMR y TETRA: el nombre se resuelve a número: primero en la agenda de
     contactos (campo `dmr_id`), y si no está, los dígitos escritos a mano.
     Devuelve -1 si el transporte no puede enviar en este modo, si el nombre no
     es un contacto ni un número, o si el texto no cabe. */

bool ok = msg_service_cancel(id, (uint32_t)getTick());  /* deja de reintentar */

msg_service_statusText(m->state);   /* "QUEUED","TX","SENT","ACK","FAIL","CANC" */
```

Y para pintar las listas, directamente el almacén:

```c
/* recibidos: el 0 es el más nuevo; NULL pasada la lista */
const struct message *m = messaging_inbox(0);
int unread = messaging_unread();
messaging_markReadId(m->id);        /* al abrir un mensaje */

/* enviados: i-ésimo en uso, 0 el más antiguo; la pantalla pinta el último
   arriba, así que recorre de messaging_outCount() - 1 hacia abajo */
int n = messaging_outCount();
const struct message *o = messaging_outbox(n - 1);

/* borrador: una entrada más del registro, no un segundo almacén */
struct message *d = messaging_draft();   /* d->transport, d->peer, d->text */
messaging_draftTouch();                  /* lo escribe la tarea, con debounce */
messaging_draftClear();
```

* `msg_service_poll(getTick())` lo llama la pantalla (cada 500 ms): mueve lo
  encolado para APRS al buzón del servicio, copia al inbox lo que el servicio
  recibe por FM y trae de vuelta el resultado de sus reintentos. DMR y TETRA no
  necesitan nada: bombean la cola ellos mismos.
* Los punteros son válidos **hasta el siguiente `messaging_send`,
  `messaging_received` o `messaging_clear`**: copiad el texto si lo vais a
  guardar (misma regla que `dmr_getStatus`).
* `messaging_task()` y `messaging_init()` ya están llamados desde `state.c`
  (tarea de estado, 100 ms): nadie más bombea la persistencia.
  `messaging_ready()` dice si el almacén ya está construido: abrir la pantalla
  **no** lo reconstruye, porque reconstruirlo relee el registro y perdería lo
  que la radio acaba de recibir.
* El contador de no leídos (`messaging_unread()`) es el que pinta el menú y la
  fila `MSGS` del menú rápido.

### 3.2 Mapa de flash

Nada se solapa. Son dos particiones del devicetree de la C62
(`platform/targets/c62/c62.dts`):

| Partición | Offset | Tamaño | Dueño |
|---|---|---|---|
| `channels_partition` (256 KiB, `0x3C0000`) | `0x00000`-`0x03FFF` | 16 KiB | banco de canales (`chstore`), dos slots de 8 KiB |
| | `0x08000`-`0x0BFFF` | 16 KiB | agenda de contactos (`cstore`), dos slots de 8 KiB |
| | `0x0C000`-`0x3FFFF` | 240 KiB | libre |
| `storage_partition` (640 KiB) | `0x00000`-`0x000FF` | 256 B | ajustes (`nvmem_C62.c`) |
| | `0x01000`-`0x01FFF` | 4 KiB | VFO |
| | `0x02000`-`0x03FFF` | 8 KiB | historial SDS de TETRA (`TetraSds.c`), dos sectores |
| | `0x04000`-`0x05FFF` | 8 KiB | **mensajes** (`messaging_store_zephyr.c`), dos sectores |

Los mensajes guían el registro con su propio `sequence`: se escribe siempre en
el sector que **no** tiene la copia buena, y al arrancar gana el número mayor
con CRC-16 correcto. Un corte a mitad de escritura cuesta la escritura, nunca
las dos copias. El registro (magic `MSG1`, versión 2) lleva cabecera de 8 B,
los mensajes de la outbox, los del inbox, un byte de marca y el borrador, y
CRC-16 al final.

---

## 4. Verificación

### 4.1 Vectores de referencia (byte a byte)

`tests/unit/assets/dmr_sms_ref.txt` guarda 9 mensajes de las siete variantes
generados por el prototipo de `work/dmr-sms` (el que se validó contra ETSI y
contra capturas de Motorola/Hytera/AnyTone). `c62_sms_host --ref` compara,
ráfaga a ráfaga, los 196 bits BPTC, la palabra Golay del slot type y el sync:
**49 ráfagas idénticas**. Los 24 bits de CACH no se comparan: son del repetidor
que repite la llamada y el generador del prototipo los rellena con su propio
patrón.

### 4.2 Ida y vuelta y lazo TX→RX

`tests/platform/c62_sms_host.c` (287 comprobaciones, 0 fallos):

| prueba | resultado |
|---|---|
| ida y vuelta a nivel de paquete de las 7 variantes | texto, direcciones, app y CRC-32 exactos |
| reensamblado ráfaga a ráfaga (cabecera + N bloques) | mensaje idéntico; el flag «pide respuesta» coincide |
| respuesta de entrega y ACK de TMS | se generan, se decodifican y se reconocen; una cabecera corrupta **no** genera respuesta |
| **lazo completo**: mensaje → `dmrTx_dataSlot` → 1440 muestras → `dmrPhy_push` → ráfagas → reensamblador → texto | **7 de 7 variantes** con texto idéntico y CRC correcto |
| un bit del codeword BPTC | corregido, el bloque sale exacto |
| cabecera con CRC malo / bloque sin cabecera | descartados: nunca texto basura |
| texto de 160 caracteres y texto de más | 30 bloques exactos; el que no cabe se corta en frontera de carácter y se marca `truncated` |

`tests/platform/c62_messaging_host.c` (51 comprobaciones, 0 fallos): estados,
reintentos, timeout de confirmación, orden, cola llena, truncado UTF-8 y el
registro de flash (ida y vuelta, registro corrupto rechazado, escritura
debounced).

En la suite de host de OpenRTX (`meson test`) hay además dos casos de la
pantalla (`UI Messages Test`) y siete del almacén y de la agenda
(`User Stores Test`), incluidos el borrador y los estados en el registro
(versión 2), la cancelación y la marca de leído por mensaje.

### 4.3 Coste en el M33 (QEMU mps2-an505, plugin de instrucciones)

| operación | -O2 | -Os |
|---|---|---|
| `dmrSms_encode`, mensaje de 4 bloques | 13 895 instr (≈46 µs a 300 MHz) | 15 198 |
| reensamblar un paquete de 4 bloques | 31 928 instr (≈106 µs, 8 000 por ráfaga) | 32 357 |
| una ráfaga de datos a muestras (`dmrTx_dataSlot`) | 119 981 instr (≈0,4 ms, **1,3 % del slot de 30 ms**) | 117 692 |
| `dmrSms_decodePacket` + texto | 12 706 instr | 12 902 |

El camino de recepción cuesta ~8 000 instrucciones por ráfaga de datos: menos
del 0,1 % del slot. El modulador ya lo pagaba la voz (117 669 por ráfaga).

### 4.4 Tamaño de la imagen completa

`c62/next`, cuatro combinaciones, con `-DCONFIG_C62_DMR_RX=y` en todas y
`CONFIG_DMR_SMS` / `CONFIG_MESSAGING` en su valor por defecto (ON). El volcado
de banda base está **apagado** en las cuatro:

| `CONFIG_DMR_TX` | `CONFIG_C62_APRS` | FLASH | RAM |
|---|---|---|---|
| on | on | **568 832 B** (54,25 %) | **208 624 B** (79,58 %) |
| on | off | 540 600 B (51,56 %) | 196 104 B (74,81 %) |
| off | on | 508 124 B (48,46 %) | 195 672 B (74,64 %) |
| off | off | 479 644 B (45,74 %) | 183 152 B (69,87 %) |

PSRAMAP 379 064 B (20,66 %) en las cuatro: sin volcado de banda base.
Con `-DC62_BASEBAND_DUMP=ON` (imagen de captura, no la de uso normal) la misma
combinación sube a 212 928 B de RAM (81,23 %) y 1 427 640 B de PSRAMAP
(77,80 %): por eso el volcado va apagado por defecto.

El peor caso son 79,58 %: por debajo del 80 % con ~1,1 KiB de margen. Lo que
ocupa la mensajería: dos reensambladores (uno por timeslot, ~460 B cada uno),
el paquete en transmisión (420 B), las dos colas (6 + 6 mensajes de 196 B =
2,3 KiB), el registro empaquetado (2 500 B) y el PDU de TETRA preparado
(400 B). Para llegar ahí se recortaron las colas de 8+8 a 6+6, el registro de
3 400 a 2 500 B y la agenda de 64 a 32 contactos, y se quitó la segunda copia
del registro que el backend de flash guardaba para leer (3,4 KiB: ahora lee
directo al buffer de `messaging.c`).

---

## 5. TETRA SDS: preparado y encolado

`TetraSdsEnc.c` es el codificador de uplink de `work/tetra-sds` traído al
firmware tal cual (U-SDS-DATA, GSM 7 bits con reserva a UCS-2, U-STATUS,
envoltura MLE/LLC, MAC-ACCESS y MAC-DATA), con
`tetraSds_encodeText()` como entrada de más alto nivel.

`tetraSds_prepareQueued()` (llamada desde la tarea de estado) coge el mensaje
más antiguo encolado para TETRA, lo codifica y **lo deja encolado**: esta
imagen no tiene cadena de transmisión de TETRA (el hardware está vetado), así
que no se emite nada. `tetraSds_preparedBits()` y `tetraSds_preparedPdu()` dan
el PDU para diagnóstico y para el transmisor futuro.

---

## 6. Límites conocidos

1. **Rate 3/4 y rate 1** no están: sólo rate 1/2 (12 octetos por bloque), que
   es lo que usa todo el mundo para texto.
2. **Sin fragmentación multi-paquete**: un mensaje es un paquete. El texto se
   corta a 160 octetos UTF-8 (≈30 bloques) y se marca `truncated`.
3. **Sin cifrado** (DMRA ARC4/DES): los bloques cifrados saldrán con CRC malo y
   se descartan.
4. **UDT (DPF 0)**, el short data del canal de control de troncal, se identifica
   y se deja en crudo: su contenido lo define TS 102 361-4.
5. **Un bloque casi todo relleno puede perderse.** Un bloque de datos con ocho
   octetos a cero (lo que produce un texto corto con relleno) codifica a un
   codeword con un tramo largo al mismo nivel; el estimador de niveles del PHY
   (mediana por slot) se descentra y el bloque sale con errores (medidos: 3-8
   bits en el último bloque de un mensaje de 4). El CRC-32 del paquete lo
   detecta y el mensaje se descarta: **nunca se muestra basura**, y la cola
   reintenta. El arreglo de verdad es del estimador de niveles del PHY, no de
   esta capa; está documentado aquí para que no se confunda con un fallo del
   SMS. Los mensajes de prueba del lazo se eligen para llenar los bloques.
6. **Grupo o individual**: el mensaje va como llamada de grupo salvo que el
   destino configurado en el modo sea `DMR_DEST_USER` (§2.6). La dirección del
   mensaje no distingue por sí sola un talkgroup de un DMRID de unidad.
7. **ACK de capa 2 sin reintento selectivo**: se responde ACK (clase 00) cuando
   el paquete está íntegro; NACK/SACK no se emiten (el receptor descarta y el
   emisor reintenta el mensaje entero).

---

## 7. Verificación del modelo (host)

Además de las pruebas de protocolo de §4, `tests/unit/` cubre la parte que
antes no tenía ninguna:

| prueba | qué comprueba |
|---|---|
| `dmr_sms_format.cpp` | las ocho opciones de `sms_format` producen la app que dicen, `auto` = `IP_HC`, un valor inválido degrada a `IP_HC`, y el texto sobrevive en las siete variantes (codifica con `dmrSms_encode`, decodifica con `dmrSms_decodePacket`, y un caso se reensambla ráfaga a ráfaga con `dmrSmsRx_feed`) |
| `dmr_lc_dest.cpp` | el link control lleva el destino (FLCO `0x00` grupo / `0x03` unidad), el origen en su campo, los 24 bits completos sin desbordar y el terminador de datos conserva destino y bit G/I |
| `settings_record.cpp` | `dmr_id`/`dmr_dest_id` se validan en 1..16777215, `sms_format` en 0..7, y un registro de ajustes de otra versión se descarta entero |

## 8. Reproducir

```sh
# pruebas de host del SMS (7 variantes, lazo TX→RX por la PHY, vectores)
cc -O2 -std=gnu17 -I openrtx/include tests/platform/c62_sms_host.c \
   openrtx/src/protocols/DMR/{DmrDefs,DmrFec,DmrPhy,DmrTx,DmrTxTables}.c \
   openrtx/src/protocols/DMR/{DmrSms,DmrSmsApp}.c -lm -o c62_sms_host
./c62_sms_host --ref tests/unit/assets/dmr_sms_ref.txt

# cola de mensajería
cc -O2 -std=gnu17 -I openrtx/include tests/platform/c62_messaging_host.c \
   openrtx/src/core/messaging.c -o c62_messaging_host && ./c62_messaging_host

# pantalla y almacén en la suite de host
meson setup build_linux && meson test -C build_linux ui_messages_test \
    user_stores_test

# coste en el M33 (necesita arm-none-eabi y qemu-system-arm)
work/sms-cost/build.sh        # -O2;  OPT=Os work/sms-cost/build.sh

# imagen completa (las cuatro combinaciones de la tabla de §4.4)
west build -b c62 -d build_tx_aprs . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_DMR_TX=y \
     -DCONFIG_C62_APRS=y
```

Regenerar los vectores de referencia (necesita el prototipo de host):

```sh
work/dmr-sms/build/dmr_sms gen --fmt tms --text "Motorola TMS" --src 1234567 \
    --dst 1234568 --confirmed --seq 15 --cc 1 --bits out/tms.bits --out /dev/null
```
