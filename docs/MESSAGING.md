# Mensajería: SMS de DMR, TETRA SDS y APRS en una sola cola

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre la capa de mensajería (`core/messaging.*`), el SMS de DMR
(`protocols/DMR/DmrSms.*`), el encolado del SDS de TETRA
(`protocols/TETRA/TetraSdsEnc.*`) y el enganche con APRS. La pantalla de
mensajería de la UI es otro trabajo: aquí está lo que tiene que llamar.

---

## 1. Qué entra

| pieza | ficheros | qué hace |
|---|---|---|
| SMS de DMR | `include/protocols/DMR/DmrSms.h`, `src/protocols/DMR/DmrSms.c`, `src/protocols/DMR/DmrSmsApp.c` (`DmrSmsApp.h` privado) | codificador y decodificador de las **siete variantes** que se ven en el aire, cabecera de datos, CRC-16 y CRC-32, reensamblado por timeslot y respuesta de entrega |
| ráfaga de datos | `DmrTx.c` (`dmrTx_dataSlot`, `dmrTx_dataTerminator`) | 96 bits de información + tipo de dato → 144 símbolos → 1440 muestras a 48 kHz, con la cadencia TDMA de siempre |
| integración en el modo | `src/rtx/OpMode_DMR.cpp` | datos recibidos → reensamblador del timeslot; mensaje recibido → cola + ACK; mensaje en cola → transmisión ráfaga a ráfaga cuando no hay voz |
| cola común | `include/core/messaging.h`, `src/core/messaging.c`, `src/core/messaging_store_zephyr.c` | modelo de mensaje, outbox de 8, inbox de 8, estados, registro en flash en un segundo par de sectores |
| APRS | `src/core/messaging_aprs.c` | mueve lo encolado para APRS al buzón de `aprs_service` (llamado desde `OpMode_FM`) |
| SDS de TETRA | `include/protocols/TETRA/TetraSdsEnc.h`, `src/protocols/TETRA/TetraSdsEnc.c` + `tetraSds_prepareQueued()` en `TetraSds.c` | codificador de uplink (U-SDS-DATA, GSM 7 bits / UCS-2) probado contra libtetra; el mensaje queda **encolado y codificado**, no se emite (no hay cadena de TX de TETRA) |
| pruebas | `tests/platform/c62_sms_host.c`, `tests/platform/c62_messaging_host.c`, `tests/platform/c62_sms_cost.c`, `tests/unit/assets/dmr_sms_ref.txt` | ida y vuelta, vectores de referencia, lazo TX→RX por la PHY real, coste en el M33 |

Opciones de compilación (todas ON por defecto en la C62):

```
-DCONFIG_C62_DMR_RX=y   modo DMR (necesario para el SMS)
-DCONFIG_DMR_TX=y       cadena de transmisión (voz y datos)
-DCONFIG_DMR_SMS=y      SMS de DMR (DmrSms.c); necesita CONFIG_MESSAGING
-DCONFIG_MESSAGING=y    cola común (messaging.c + registro en flash)
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
* La codificación de un mensaje usa `IP_HC` (la cabecera más corta) por
  defecto: entran ~150 caracteres en un paquete.

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
* `state`: `FREE`, `QUEUED`, `SENDING`, `SENT`, `DELIVERED`, `FAILED`.
* `flags`: `UNREAD`, `CONFIRM` (se pidió confirmación), `INBOUND`, `TRUNCATED`.

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
* **Persistencia**: registro empaquetado con magic `MSG1` y CRC-16 en dos
  sectores alternos de la partición de almacenamiento (`0x4000` y `0x5000`;
  ajustes en `0x0000`, VFO en `0x1000`, SDS de TETRA en `0x2000`/`0x3000`).
  Un corte de luz durante la escritura deja la copia anterior intacta. Un
  mensaje que estaba en el aire se reencola al arrancar: su resultado no se
  conoce.

### 3.1 Lo que tiene que hacer la UI

```c
/* enviar (DMR: ID de radio o talkgroup; TETRA: SSI) */
int id = messaging_send(MSG_TRANSPORT_DMR, dst, texto_utf8, confirmar, getTick());

/* enviar por APRS: la dirección es un indicativo */
int id = messaging_sendToText(MSG_TRANSPORT_APRS, "EA5XXX-9", texto_utf8,
                              false, getTick());

/* lista de recibidos, el 0 es el más nuevo */
int n = messaging_unread();
const struct message *m = messaging_inbox(0);   /* -> src, text, time, transport */
messaging_markRead();

/* enviados: incluye los terminados, para pintar cola/enviado/entregado/fallido */
const struct message *o = messaging_outbox(i);  /* NULL cuando no hay más */

messaging_transportName(m->transport);          /* "DMR", "APRS", "TETRA SDS" */
messaging_stateName(o->state);                  /* "queued", "sent", ... */
```

* Los punteros son válidos **hasta el siguiente `messaging_send`,
  `messaging_received` o `messaging_clear`**: copiad el texto si lo vais a
  guardar (misma regla que `dmr_getStatus`).
* `messaging_task(getTick())` y `messaging_init()` ya están llamados desde
  `state.c` (tarea de estado, 100 ms): la UI no tiene que bombear la
  persistencia.
* Un mensaje nuevo recibido sube `messaging_unread()`: para avisar en la
  pantalla principal basta con mirar ese contador (o `ui_pushEvent` desde el
  hilo de UI cuando cambie).

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

`west build -b c62` con `-DCONFIG_C62_DMR_RX=y -DCONFIG_DMR_TX=y
-DCONFIG_C62_APRS=y` (y `CONFIG_DMR_SMS`, `CONFIG_MESSAGING` por defecto ON):

| | referencia | con SMS + mensajería | diferencia |
|---|---|---|---|
| FLASH | 547 596 B (52,22 %) | **558 272 B** (53,24 %) | **+10 676 B** (+2,0 %) |
| RAM | 192 296 B (73,36 %) | **203 360 B** (77,58 %) | **+11 064 B** (+5,8 %) |
| ITCM / PSRAMAP | 8 468 / 1 427 640 B | iguales | 0 |

Referencia: `master` (`e4f6f6a2`) con las mismas tres opciones. Lo que ocupa:
dos reensambladores (uno por timeslot, ~460 B cada uno) y el paquete en
transmisión (420 B), la cola (8 + 8 mensajes de ~200 B = 3,2 KiB), el registro
empaquetado (3 100 B) y el PDU de TETRA preparado (400 B). El grueso del
código nuevo son ~4,5 KiB de FLASH.

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
6. **Grupo o individual**: el mensaje se marca como de grupo si su destino
   coincide con el talkgroup configurado en el modo; si no, es una llamada
   individual. Un destino que sea un talkgroup distinto del seleccionado
   saldría como individual.
7. **ACK de capa 2 sin reintento selectivo**: se responde ACK (clase 00) cuando
   el paquete está íntegro; NACK/SACK no se emiten (el receptor descarta y el
   emisor reintenta el mensaje entero).

---

## 7. Reproducir

```sh
# pruebas de host del SMS (7 variantes, lazo TX→RX por la PHY, vectores)
cc -O2 -std=gnu17 -I openrtx/include tests/platform/c62_sms_host.c \
   openrtx/src/protocols/DMR/{DmrDefs,DmrFec,DmrPhy,DmrTx,DmrTxTables}.c \
   openrtx/src/protocols/DMR/{DmrSms,DmrSmsApp}.c -lm -o c62_sms_host
./c62_sms_host --ref tests/unit/assets/dmr_sms_ref.txt

# cola de mensajería
cc -O2 -std=gnu17 -I openrtx/include tests/platform/c62_messaging_host.c \
   openrtx/src/core/messaging.c -o c62_messaging_host && ./c62_messaging_host

# coste en el M33 (necesita arm-none-eabi y qemu-system-arm)
work/sms-cost/build.sh        # -O2;  OPT=Os work/sms-cost/build.sh

# imagen completa
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_DMR_TX=y \
     -DCONFIG_C62_APRS=y
```

Regenerar los vectores de referencia (necesita el prototipo de host):

```sh
work/dmr-sms/build/dmr_sms gen --fmt tms --text "Motorola TMS" --src 1234567 \
    --dst 1234568 --confirmed --seq 15 --cc 1 --bits out/tms.bits --out /dev/null
```
