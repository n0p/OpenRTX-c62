# Packet modem: KISS TNC, AFSK 1200 y G3RUH 9600

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre el modem de paquetes: el codec KISS TNC2 (`protocols/KISS/`), los dos
modems AFSK, el motor que los unifica (`packet_modem.c`), el servicio que los
mueve desde su propio hilo (`core/packet_service.c`), las dos pantallas
(`ui/default/ui_packet.c`) y el puerto del cable (`platform/drivers/audio/c62_kiss.c`).

Lo que hay hoy, en una línea: **la radio se convierte en un TNC2** —recibe y
transmite AX.25 a 1200 y 9600 baudios por el conector de programación— y
**expone como tramas KISS** lo que ya decodifican los receptores de datos del
firmware (el mensaje de capa 3 del SACCH de NXDN y el datagrama de un paquete
DMR). Nada de esto usa el DSP HiFi4: el modem cabe en el M33.

> **No es un modo de operación.** No aparece en la lista de modos ni toca
> `OPMODE_*`. El servicio corre en su hilo y solo abre el camino de audio
> cuando el operador lo enciende en `PKT_CFG`; mientras está apagado no toca ni
> el códec ni el PTT. Con el PTT del operador pulsado no transmite.

---

## 1. Cómo se enciende

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y \
                                   -DCONFIG_C62_KISS=y
```

`CONFIG_C62_KISS` (`platform/targets/c62/Kconfig.board`, `default y`) mete en la
imagen el codec, los modems, el servicio y las dos pantallas. La capa AX.25, el
framer HDLC y el modulador de Bell 202 viven en el bloque de APRS; si el build
lleva `-DCONFIG_C62_APRS=n`, el bloque de KISS los enlaza él mismo (son fuentes
de protocolo sueltas, no arrastran la interfaz de APRS), así que el modem se
compila sin el modo APRS. Lo que no se puede es no tener ninguno de los dos.

| opción | qué añade | por defecto |
|---|---|---|
| `CONFIG_C62_KISS` | codec, modems, servicio, pantallas `PKT_CFG` y `PKT_STATUS` | `y` |
| `CONFIG_C62_KISS_UART` | el puerto del cable como transporte KISS, con el log mudo | `n` |

`CONFIG_C62_KISS_UART` no se puede combinar con `CONFIG_C62_REMOTE`: los dos
quieren el mismo pad (GPIOA_15) y el CMake aborta con un `FATAL_ERROR` si están
los dos. **Sin ninguna de las dos, el modem arranca pero no tiene puerto**: la
pantalla de estado dice `port: none`, `packet_service_bind()` no se ha llamado
y no entra ni sale ninguna trama. Es el estado correcto del build por defecto:
la capa de protocolo del PC (`pc_proto`) enlazará su propio `kiss_port_t` y
`c62_kiss.c` no se compilará.

### Tamaño en la imagen

Medido con el mismo build (`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`), las
tres variantes:

| imagen | FLASH | RAM | PSRAM | ITCM | DTCM |
|---|---|---|---|---|---|
| sin KISS (`-DCONFIG_C62_KISS=n`) | 567 016 B (54,07 %) | 179 504 B (68,48 %) | 435 980 B | 8 468 B | 12 640 B |
| KISS (`ncuart`) | 573 944 B (54,74 %) | 188 656 B (71,97 %) | 435 980 B | 8 468 B | 12 640 B |
| KISS + cable (`uart`) | 574 116 B (54,75 %) | 188 656 B (71,97 %) | 435 980 B | 8 468 B | 12 640 B |
| KISS sin APRS (`-DCONFIG_C62_APRS=n`) | 548 980 B (52,35 %) | 181 720 B (69,32 %) | 430 388 B | 8 468 B | 12 640 B |

El modem cuesta **+6 928 B de flash y +9 152 B de RAM** (el driver del cable
añade 172 B más de flash y nada de RAM). De la RAM, 3 072 B son la pila del
hilo del servicio; el resto es el motor (dos demoduladores, dos moduladores,
deframer y buffers) más la cola de tres tramas. No toca PSRAM. La cuarta fila
está ahí porque demuestra lo de arriba: sin APRS el enlace lo resuelve el propio
bloque de KISS, y la imagen baja 25 KB de flash (todo el APRS) con el modem
dentro.

---

## 2. Los dos puertos: el del cable y el de la consola

Son **el mismo UART**. En la C62 el conector de programación va a `uart2`,
`zephyr,console` en el devicetree, 115 200 8N1 (`platform/targets/c62/c62.dts`),
y el driver KISS toma su base del *chosen* del devicetree:

```c
#define UART_BASE DT_REG_ADDR(DT_CHOSEN(zephyr_console))
```

Lo que cambia no es el puerto sino **qué viaja por él**:

| build | qué sale por el cable | log de texto | PTT |
|---|---|---|---|
| por defecto | el log del firmware | sí | sí (pad GPIOA_15) |
| `CONFIG_C62_BASEBAND_DUMP` | log + peticiones de volcado | sí | sí |
| `CONFIG_C62_REMOTE` | las respuestas de control del volcado | sí | **no**: el pad es UART2 RXD |
| `CONFIG_C62_KISS_UART` | **KISS binario** | **no**, mudo | **no**: el pad es UART2 RXD |

Tres consecuencias que conviene tener escritas:

1. **El log se calla.** KISS es binario y cualquier `printk` mete basura en
   medio de una trama. `kiss_uart_init()` instala `__printk_hook_install()` con
   una función que descarta el carácter, el mismo enganche que usa el volcado
   de banda base. Después de eso la radio no dice nada por el cable: si hay que
   depurar, se hace en un build sin `C62_KISS_UART`.
2. **El PTT deja de funcionar.** GPIOA_15 es a la vez UART2 RXD y el botón PTT;
   el driver lo conmuta a `CSK_IOMUX_FUNC_ALTER3` (UART2 RXD) igual que el
   control remoto. Por eso la opción está apagada por defecto.
3. **Se inicializa tarde.** `platform.c` llama a `kiss_uart_init()` cuando todo
   lo que imprime ya ha impreso, justo antes de armar el watchdog, y solo si el
   arranque del DSP fue bien.

El cable no tiene control de flujo: `uartWrite()` espera a `LSR.THRE` con un
tope de 200 000 vueltas y devuelve `false` si se agota, que el servicio cuenta
como `txErrors`. A 115 200 baudios son 11,5 kB/s: una trama de 330 octetos
tarda 29 ms en el cable, así que el enlace nunca puede mantener ocupado un
canal de 9600 baudios, pero una trama larga sí puede hacer esperar al hilo.

---

## 3. La trama KISS

Un mensaje va entre dos `FEND` (0xC0). Dentro, un `FEND` o un `FESC` (0xDB) se
escapan como `FESC` + `TFEND` (0xDC) o `FESC` + `TFESC` (0xDD). El primer byte
del mensaje, sin escapar, es

```
bit 7..4  puerto (0..15)
bit 3..0  comando
```

El decodificador (`protocols/KISS/kiss.c`) entrega un evento por byte y solo
mira la trama cuando llega el `FEND` de cierre:

* un mensaje que no cabe se **descarta entero** y se cuenta en `dec.bad` —una
  trama AX.25 truncada es una trama corrupta y con ella se modularía basura—;
* un escape indefinido (`FESC` seguido de otra cosa) deja el byte tal cual y
  cuenta un `bad`;
* el servicio **siempre usa KISS completo**, nunca *KISS simple*: el codec lo
  soporta (sin byte de comando y sin escapes, para los firmwares más simples)
  pero al firmware no le sirve, porque con *simple* un 0xC0 en el payload no se
  puede representar.

**Cheese.** Al entrar en modo KISS el TNC manda varios `FEND` seguidos
(`kiss_enc_cheese()`) para que el host vea un límite antes de la primera trama
y sincronice su parser. El código está y está probado; hoy solo lo usa el banco
de host, porque el servicio no tiene un "entrar en KISS" explícito: el puerto
se enlaza en el arranque y la primera trama ya va precedida de su `FEND`.

---

## 4. Comandos

| cmd | nombre | payload | unidad / rango | qué hace el firmware |
|---|---|---|---|---|
| `0x00` | DATA | trama AX.25 **sin FCS** | hasta 328 octetos | la encola para transmitir; el FCS lo añade el motor |
| `0x01` | TXDELAY | 1 byte | **10 ms** (0..255 → 0..2,55 s) | `svc.txDelay`; ver la nota de abajo: hoy se gasta **dos veces** |
| `0x02` | PERSIST | 1 byte | 0..255, sobre 256 | probabilidad de transmitir en cada slot (CSMA del TNC2) |
| `0x03` | SLOTTIME | 1 byte | **10 ms** | `svc.slotTime`; duración del slot de acceso al canal |
| `0x04` | TXTAIL | 1 byte | **10 ms** | `svc.txTail`; silencio después del último flag |
| `0x05` | FULLDUPLEX | 1 byte | 0 o 1 | 1: no se hace *carrier sense*, se transmite cuando el host lo pide |
| `0x06` | SET HARDWARE | 1 byte | específico del TNC | **se acepta y se ignora** (no hay hardware que configurar) |
| `0x0F` | END-KISS (direwolf) | — | — | deja el modo KISS: `svc.enabled = false` |
| `0xFF` | RETURN (clásico) | — | — | lo mismo |

Los nueve comandos están implementados y probados en el banco de host; los tres
que el firmware **no** implementa (`0x07`..`0x0E`, `0x10`..`0xFE`) devuelven
`KISS_EV_UNKNOWN` y se cuentan como `txDropped`, que es lo que hace un TNC2
cuando el host le pide algo que no entiende.

Un detalle que no coincide entre la especificación, direwolf y la pila `ax25`
de Linux: direwolf usa `0x0F` para salir de KISS y el TNC2 clásico `0xFF`. Aquí
se aceptan los dos y los dos apagan el servicio; la diferencia solo importa si
alguien espera que `0xFF` signifique otra cosa (no lo hace en ninguna de las dos).

### TXDELAY, que hoy se gasta dos veces

`pkt_tx_begin()` cuenta el `TXDELAY` como **flags** (`bitRate · ms / 1000 / 8`,
mínimo 2, máximo 240), que es lo que enseña el cruce con direwolf: el receptor
ajeno necesita portadora modulada para abrir el squelch y enganchar el reloj de
bit. Pero además pone `silenceLeft` a los mismos milisegundos de **silencio**, y
`pkt_tx_block()` emite ese silencio antes de los flags. Con los 300 ms por
defecto, una trama de 274 octetos a 1200 dura **2,46 s** (118 120 muestras), de
las cuales 0,3 s son portadora sin modular y 0,3 s flags: medido, 2 953 bits en
el aire, de los que 360 son ese silencio (sin él serían 2 593, que es la trama
de 276 octetos con su FCS más los flags de preámbulo y los de cierre).

No se ha tocado porque no hay radio con la que decidir cuál de los dos es el que
el PA necesita, y moverlo cambiaría justo la forma de onda que está verificada
contra direwolf en las dos direcciones. Queda como primer punto de banco (§8).

---

## 5. Modems: framing y límites

Registro en `protocols/KISS/packet_modem.c` (`pkt_modem_info()`):

| modo | nombre | baudios | TX | RX | payload máx. | qué va por el aire |
|---|---|---|---|---|---|---|
| 0 | `AFSK1200` | 1200 | sí | sí | 328 | AX.25 / HDLC / NRZI, Bell 202: mark 1200 Hz, space 2200 Hz, 40 muestras/bit a 48 kHz |
| 1 | `AFSK9600` | 9600 | sí | sí | 328 | AX.25 / HDLC, *scrambler* G3RUH `1+x¹²+x¹⁷`, NRZ en banda base, 5 muestras/bit a 48 kHz |
| 2 | `NXDN` | — | no | sí | 9 | mensaje de capa 3 del SACCH de un superframe NXDN |
| 3 | `DMRIP` | — | no | sí | 256 | datagrama IPv4/UDP de una llamada de datos DMR (`dmr_data.c`) |

Límites que no son obvios:

* **328 octetos** es el techo real de AX.25: `7 · (2+8) + 2 + 256 = 328` sin el
  FCS. `KISS_MAX_PAYLOAD` es 340 y deja holgura para los modos de payload, que
  no son tramas AX.25.
* El ajuste `packet_maxframe` vale 330 (el techo **con** FCS) y es el que corta
  lo que llega del host: una trama de más se cuenta en `txDropped` y no se
  transmite.
* La cola de transmisión es de **3 tramas**; la cuarta se descarta.
* 9600 **necesita 48 kHz** (o 96). `pkt_engine_init()` devuelve `false` en
  cualquier otra frecuencia y el motor no arranca; 1200 funciona a cualquier
  frecuencia que divida 1200.
* Los modos 2 y 3 son de **solo recepción**: su cola no se vacía nunca
  (`txStep()` los salta a propósito) y un `0x00` que llegue del host se queda
  ahí hasta que se cambie de modo. Transmitir NXDN necesitaría el codificador
  de voz y el *short data call* va en FACCH1, que el receptor no decodifica;
  transmitir un paquete DMR necesitaría el constructor de cabecera y bloques,
  que el paquete DMR no tiene.

---

## 6. Mapeo al firmware

### 6.1 De un comando KISS a un registro de la radio

| comando | variable del servicio | ajuste persistente | efecto en el hardware |
|---|---|---|---|
| `TXDELAY` | `svc.txDelay` | `packet_txdelay_ms` | `pkt_tx_begin()`: muestras de flags antes de la trama |
| `PERSIST` | `svc.persist` | `packet_persist` | `channelFree()`: comparación con el byte alto de `k_cycle_get_32()` |
| `SLOTTIME` | `svc.slotTime` | `packet_slottime_ms` | `channelFree()`: programación de `nextSlotMs` |
| `TXTAIL` | `svc.txTail` | `packet_txtail_ms` | `pkt_tx_begin()`: silencio después del último flag |
| `FULLDUPLEX` | `svc.fullDuplex` | `packet_fulldup` | salta el *carrier sense* |
| `DATA` | cola `svc.queue[3]` | — | `radio_enableTx()`, `platform_ledOn(RED)`, audio por `SOURCE_MCU→SINK_RTX` |

Los valores que escribe el host valen para la sesión; los que se ponen desde la
pantalla se guardan en los ajustes (y sobreviven al apagado), que es lo que hace
un TNC2 con sus parámetros y lo que un operador espera de un menú.

### 6.2 De la trama a los registros

| qué | dónde | valor |
|---|---|---|
| UART del cable | `DT_REG_ADDR(DT_CHOSEN(zephyr_console))` = `uart2` | 115 200 8N1 |
| registros que se tocan | `RBR/THR` en `+0x00`, `LSR` en `+0x14` | bits `DR` (0) y `THRE` (5) |
| pad del cable | `IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, 15, ALTER3)` | GPIOA_15 = UART2 RXD |
| audio de recepción | `audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX)` | 48 kHz, bloques de 480 muestras (10 ms), `BUF_CIRC_DOUBLE` |
| audio de transmisión | `audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX)` | el mismo códec que usan los demás modos de datos |
| hilo | `packetMain`, prioridad 6, pila 3 072 B | un bloque cada 10 ms; `packet_service_status()` lee contadores de 32 bits sin mutex |
| ajustes | bloque `packet_*` al final de `settings_t`, detrás de `am_depth` | `SETTINGS_VERSION` 6, la misma subida que el TX AM (`c62/mega`) |

El PTT es del operador: `transmit()` sale sin transmitir si
`platform_getPttStatus()` es cierto, y al terminar devuelve el radio a recepción
con `radio_disableRtx()` + `radio_enableRx()`. Una trama que no cabe, un
comando desconocido y un desbordamiento del decodificador se cuentan por
separado (`txDropped`, `txErrors`, `rxFcs`) y se ven en `PKT_STATUS`.

---

## 7. Qué está verificado (y cómo)

### 7.1 Banco de host, sin radio

```
tests/host/kiss_loop/run.sh
```

* **Codec KISS:** 44 comprobaciones (FEND/FESC, escapes, `simple`, desbordamiento,
  `0xFF`/`0x0F`, *cheese*), 0 fallos.
* **Loopback completo** (trama → modulador → demodulador → trama) con tramas de
  20, 64, 200 y 256 octetos: **96 comprobaciones por modem**, 0 fallos, a 1200 y
  a 9600.
* **Direwolf, las dos direcciones**, con `atest` y `gen_packets` de direwolf
  1.7: decode de nuestro audio a 1200 y 9600 (2 de 2 tramas cada uno) y nosotros
  decodificamos su audio a 1200 y 9600 (4 de 4, byte a byte). Dos bugs reales
  salieron de aquí: direwolf hace **NRZI y después** el *scrambler* (no al
  revés), y el `TXDELAY` tiene que llevar **flags**, no silencio.

  ```sh
  DIREWOLF=~/src/direwolf/build/src tests/host/kiss_loop/run.sh
  # si el direwolf prebuilt pide una libgps que el sistema ya no tiene:
  mkdir -p /tmp/dwlib && ln -sf /usr/lib/libgps.so.33 /tmp/dwlib/libgps.so.32
  LD_LIBRARY_PATH=/tmp/dwlib DIREWOLF=... tests/host/kiss_loop/run.sh
  ```
* **BER contra ruido** (20 000 bits por punto): AFSK 1200 cae de 4,5·10⁻¹ a
  6,5·10⁻⁴ en −2 dB y llega a 0 en 0 dB de SNR (≈16 dB Eb/N0); G3RUH 9600 cae a
  1,5·10⁻⁴ en 8 dB y llega a 0 en 10 dB.

### 7.2 Coste en el M33 (QEMU, Cortex-M33)

```
scripts/kiss_cost.sh O2      # o Os
```

Arnés `tests/qemu/kiss_cost.c`, contador de instrucciones por región de
`work/dsp-probe`. Cada etapa va encerrada entre dos `frame_marker()`, así que
las diferencias de las 12 instantáneas son el coste de esa etapa. El arnés
imprime `rx_frames 1` por modem: **el demodulador engancha** con el audio que
genera el propio modulador, así que lo medido es el camino que decide, no ruido.

Por bloque de 10 ms de audio (480 muestras), `-O2`:

| etapa | instrucciones / bloque | por muestra | equivalente a 48 kHz | % del M33 a 300 MHz |
|---|---|---|---|---|
| 1200 modula | 39 306 | 82 | 3,9 M/s | 1,3 % |
| 1200 demodula | 56 044 | 117 | 5,6 M/s | 1,9 % |
| 9600 modula | 34 444 | 72 (358/bit) | 3,5 M/s | 1,2 % |
| 9600 demodula | 85 301 | 178 (888/bit) | 8,5 M/s | 2,8 % |

Y por trama, `-O2`: armar la transmisión 12 903 (1200) y 13 653 (9600)
instrucciones, KISS decodificar+codificar **16 914** instrucciones por trama de
274 octetos. Una trama de 274 octetos son 118 120 muestras a 1200 (2,46 s) y
36 005 a 9600 (0,75 s).

`-Os` mueve poco y en direcciones distintas: 1200 demodula baja a 115 por
muestra, 9600 demodula **sube** a 192 (el receptor de 9600 depende del
desenrollado de la correlación), y el KISS sube a 24 008 instrucciones por
trama. La imagen se compila en `-O2`: sale más barato en el receptor que más
consume.

La cuenta de CPU: el modulador solo corre mientras se transmite (0,75 s a 9600,
2,46 s a 1200 por trama de 274 octetos) y el KISS solo cuando hay tráfico; lo
que es continuo es el demodulador, **1,9 % del M33 a 1200 y 2,8 % a 9600**, y
solo con el servicio encendido: apagado no se abre ni el camino de audio.

### 7.3 La imagen

Los dos builds de la §1 compilan enteros y enlazan (`arm-none-eabi-nm` del ELF
confirma `packet_service_init`, `kiss_dec_byte`, `pkt_engine_init`,
`ui_packet_draw`, y `kiss_uart_init` **solo** en la variante del cable).
No se ha ejecutado en la radio: ver §8.

---

## 8. Lo que queda de banco (sin aire)

Todo lo anterior es host y QEMU. **No hay una sola prueba con la radio, el cable
ni un transmisor delante**, y eso deja estos huecos, en el orden en que se
cerrarían:

1. **El cable, de verdad.** Ni un byte por el conector. Lo que hay que mirar:
   que el pad conmute a UART2 RXD sin dejar el PTT colgado, que 115 200 8N1 no
   pierda bytes con el códec sonando (el driver no usa DMA ni interrupciones:
   sondea `LSR` cuando el hilo se despierta cada 10 ms), y que el log mudo no
   deje al operador sin diagnóstico en un fallo de arranque.
2. **El PTT, el `TXDELAY` y el tiempo de subida del PA.** `TXDELAY` son 300 ms
   por defecto, pensados para FM analógica, y hoy se gastan dos veces (§4: 0,3 s
   de portadora sin modular + 0,3 s de flags). Hay que decidir con la radio
   delante cuál de los dos se queda, y barrer el valor a 1200, donde los flags
   del preámbulo son los que enganchan el PLL del receptor ajeno. También falta
   comprobar que el `TXTAIL` de 20 ms no corta el último flag al bajar el PA.
3. **El desvió y el filtro del BK4819.** El modulador entrega banda base por el
   códec; el desvió real del canal de 1200 (¿2,5 kHz? ¿3?) y el ancho de FI que
   hay que poner para 9600 (el 9600 G3RUH necesita toda la banda del canal de
   12,5 kHz) no están calibrados. Sin esto, 9600 puede modular bien y no oírse.
4. **CSMA con una estación de verdad.** `PERSIST` y `SLOTTIME` están probados
   como aritmética, no contra un canal ocupado: nadie ha comprobado que la
   radio no pise a otra, ni que `carrier` (el `pkt_rx_signal()` del
   demodulador) se levante con una señal real y no con el ruido del receptor.
5. **El enganche de los modos de payload.** `packet_service_nxdnPayload()` y
   `packet_service_dmrPayload()` están enlazados pero no ejercitados: hace falta
   un canal NXDN con SACCH y una llamada de datos DMR reales. Tampoco hay
   pruebas de que el tamaño (9 y 256 octetos) sea el que llega.
6. **`SET HARDWARE` (0x06).** Se acepta y se ignora. Si un host espera respuesta
   (direwolf pregunta por el `TXDELAY` con un `0x06` en algunas versiones), se
   quedará esperando.
7. **KISS simple y *cheese* en el firmware.** El codec los tiene y el banco los
   prueba, pero el servicio siempre usa KISS completo y no manda *cheese* al
   enlazar el puerto.
8. **La pantalla en la radio.** `PKT_CFG` y `PKT_STATUS` compilan y están
   enlazadas al teclado, pero nadie las ha visto en el panel: faltan las
   comprobaciones de que 8 filas caben con la fuente de 6 pt y de que el
   `txDisable` que activan no deja el PTT muerto al salir.
9. **`packet_maxframe` = 330 frente al techo real de 328.** La cola acepta hasta
   330 octetos y el motor rechaza a partir de 329 (`pkt_tx_begin()` compara con
   `maxPayload`), así que una trama de 329 o 330 muere en el motor y se cuenta
   como `txErrors`, no como `txDropped`. Funciona, pero el operador ve el
   contador equivocado; o el ajuste baja a 328 o el motor devuelve el motivo.

---

## 9. Ficheros

```
openrtx/include/protocols/KISS/kiss.h        codec KISS TNC2 (FEND/FESC, comandos)
openrtx/src/protocols/KISS/kiss.c
openrtx/include/protocols/KISS/afsk9600.h    G3RUH: NRZI, scrambler, conformado
openrtx/src/protocols/KISS/afsk9600.c
openrtx/include/protocols/KISS/packet_modem.h  registro de modos + motor
openrtx/src/protocols/KISS/packet_modem.c
openrtx/include/core/packet_service.h        API del servicio (hilo, cola, estado)
openrtx/src/core/packet_service.c
openrtx/include/ui/ui_packet.h               pantallas PKT_CFG y PKT_STATUS
openrtx/src/ui/default/ui_packet.c
platform/drivers/audio/c62_kiss.c            puerto del cable (C62_KISS_UART)
tests/host/kiss_loop/                        banco de host + audio para direwolf
tests/qemu/kiss_cost.c                       coste por etapa (QEMU mps2-an505)
scripts/kiss_cost.sh                         compila y resume el perfil del plugin
```

Documentos vecinos: `docs/APRS.md` (el demodulador de 1200 y el HDLC que este
modem reutiliza, con su propio coste y sus capturas con direwolf) y
`docs/RX_NXDN.md`, `docs/DMR_PKG.md` (de dónde salen los payloads de los modos
2 y 3).
