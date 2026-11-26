# Caja negra: post-mortem de un cuelgue

Un cuelgue de esta radio no dejaba nada. La reproducción de esta semana es el
ejemplo perfecto: desde FM en VFO, cambiar a **AM** (banda aérea). La interfaz se
congela, el protocolo PC deja de contestar y **por el puerto serie no sale ni un
byte**: ni un `printk`, ni una traza, ni una línea de error. La única salida era
el mando de volumen, y con él se iba también la información.

El otro caso es el del **claveado en DMR**: la radio se bloquea, deja de
contestar y acaba reiniciando por el perro guardián de aplicación, que solo se
alimenta si la interfaz avanza. Un reinicio limpio, sin explicación.

Esta capa existe para que las dos cosas dejen de ser un misterio y para que la
radio **vuelva sola a recepción** en vez de exigir apagarla a mano.

| fichero | qué hay |
|---|---|
| `openrtx/include/core/blackbox.h` | el contrato: formato del registro, latidos, migajas, hooks |
| `openrtx/src/core/blackbox.c` | C portátil: detector, registro, CRC, texto |
| `platform/targets/c62/blackbox_c62.c` | hilo supervisor, sector de flash, recorrido de hilos, manejador de fallo |
| `tests/unit/blackbox.cpp` | tests de host: detección, contenido, lectura al arrancar |
| `tools/cps/blackbox.py` | lector: por el cable o de un fichero de 516 bytes |

---

## 1. Las tres piezas

### 1.1 Latidos

Cada hilo que importa incrementa su contador una vez por vuelta:

| latido | dónde | periodo nominal |
|---|---|---|
| `BB_BEAT_UI` | bucle de `ui_threadFunc()` (`core/threads.c`) | 25 ms |
| `BB_BEAT_RTX` | primera línea de `rtx_task()` | 30 ms |
| `BB_BEAT_PROTO` | bucle de `pcProtoMain()` (`pc_proto_c62.c`) | 1 ms |
| `BB_BEAT_MAIN` | bucle de `main_thread()` | 5 ms |
| `BB_BEAT_KBD` | hilo `kbd_thread` (`platform/drivers/keyboard/keyboard_c62.c`) | 20 ms |

El teclado tiene hilo propio y por eso tiene latido propio. El escaneo de la
matriz 4 × 5 (cuatro filas manejadas, cinco columnas leídas) lo hacía antes
quien pedía las teclas —el hilo de UI cuarenta veces por segundo, el principal
para el botón de encendido, el de RTX para el PTT—, y eso tenía dos
consecuencias: tres hilos tocando los mismos GPIO, y la vida del teclado atada a
la del hilo de UI. Un hilo que se queda en el volcado de pantalla se lleva por
delante el PTT y el botón de encendido sin que nada lo diga; ahora el escáner es
uno, los demás leen su última pasada, y su latido permite que el registro
señale el teclado parado por sí solo (`detail` bit 4).

`bb_beat()` es un incremento, una lectura de reloj y un store. Además refresca
el reloj grueso que estampan las migajas.

Un latido que **nunca** se ha incrementado no se vigila: así un build sin
protocolo PC (no hay `BB_BEAT_PROTO`) o un hilo que aún no ha arrancado no
provocan un reinicio en cada boot.

### 1.2 El detector: un hilo supervisor propio

`bb_supervisor` es un hilo de Zephyr (`K_THREAD_DEFINE`, prioridad 2, pila de
3 KiB) que **no toma ningún mutex, ninguna cola y ningún allocador**. Esto no es
un detalle de estilo: es la razón de que exista. El cuelgue típico es un hilo
bloqueado con `state_mutex` en la mano; cualquier detector que necesite ese
mutex se queda mirando. El supervisor solo lee palabras `volatile`.

Cada 100 ms compara cada contador con el valor que tenía en la pasada anterior.
Si uno no se ha movido durante **2 s** (`BB_STALL_MS`), es un cuelgue.

El supervisor **también alimenta el perro guardián** de aplicación, y deja de
hacerlo en cuanto detecta el atasco: escribe el registro y reinicia él mismo. Si
el supervisor no corre, no alimenta, y el `ap_wdt` hace lo de siempre. Ni una
sola vía deja la radio colgada para siempre.

### 1.3 Migajas de pan

`bb_mark(id)` es **una escritura de una palabra** en un anillo de 32 entradas en
`.bss`, más un incremento del índice:

```c
ring[head & 31] = (id << 24) | (ms & 0xFFFFFF);
```

Ni cerrojo, ni reloj (usa el reloj grueso que refrescan los latidos), ni
asignaciones: se puede llamar desde un hilo, desde una interrupción y desde el
manejador de fallo. Dos hilos que marquen en el mismo instante pierden una
migaja (escriben la misma ranura); las que sobreviven conservan el orden, que es
para lo que se leen.

Las marcas que hay hoy:

| camino | marcas |
|---|---|
| arranque / apagado | `BOOT`, `TERM` |
| cambio de modo (`rtx.cpp:switchMode`) | `MODE_ENTER`, `MODE_READY`, `MODE_FAIL`, `MODE_OFF` |
| reconfiguración | `RTX_CONF`, `RADIO_CONF`, `RX_ON`, `TX_ON`, `TX_OFF`, `AIRBAND_LOAD` |
| transmisión | `TX_ON`, `TX_OFF`, `TX_ABORT` |
| rutas de audio (`core/audio_path.cpp`) | `AUDIO_OPEN`, `AUDIO_CLOSE` |
| volcado de pantalla (`display_c62.c`) | `DISP_DROP` (una por racha, no por frame) |
| antes de escribir el registro | `HANG` |

Una marca nueva se añade **al final** del `enum bbMark`: el número es lo que
queda grabado en el registro y el lector lo traduce con `bb_markName()`, así que
insertar una en medio cambiaría el significado de los registros viejos.

Para añadir una: un valor en `enum bbMark`, su nombre en `bb_markName()` y la
llamada. Nada más.

---

## 2. El registro

### 2.1 Dónde vive

Sector propio dentro de `storage_partition` (`platform/targets/c62/c62.dts`,
base `0x2B0000`):

| offset | uso |
|---|---|
| `0x0000` | ajustes (`nvmem_C62.c`) |
| `0x1000` | VFO (`nvmem_C62.c`) |
| `0x2000` / `0x3000` | historial SDS de TETRA (`TetraSds.c`) |
| `0x4000` / `0x5000` | mensajes (`messaging_store_zephyr.c`) |
| **`0x6000`** | **caja negra** (este documento), 4 KiB, de los que se usan 516 B |
| `0x7000`-`0x9FFFF` | libre (612 KiB) |

Se borra el sector entero y se escriben 516 bytes al principio. Un corte a mitad
de escritura deja un registro con CRC malo, que el lector rechaza: nunca se lee
como bueno un registro a medias.

### 2.2 Formato (versión 1, little endian, 516 bytes)

| offset | tamaño | campo |
|---|---|---|
| 0 | 4 | `magic` `"BBX1"` = `0x31425842` |
| 4 | 2 | `version` = 1 |
| 6 | 2 | `hdrSize` = 64 |
| 8 | 4 | `valid` = `"XRBB"` = `0x42425258`; **0** si ya se sirvió |
| 12 | 4 | `uptimeMs` |
| 16 | 1 | `reason`: 1 `STALL`, 2 `FATAL`, 3 `MANUAL` (+3 bytes a cero) |
| 20 | 4 | `detail`: máscara de latidos parados, o el motivo del fallo de Zephyr |
| 24 | 4 | `resetCause` (la CSK6 no expone el registro del SoC: 0) |
| 28 | 4 | `crumbSeq`: total de `bb_mark()` desde el arranque |
| 32 | 2 | `rtxMode` (enum `opmode` del hilo RTX) |
| 34 | 2 | `uiMode` (enum `opmode` que tenía la interfaz) |
| 36 | 4 | `rxFreq`, Hz |
| 40 | 4 | `txFreq`, Hz |
| 44 | 2 | `rtxFlags`: bit0 RX, bit1 TX (driver), bit2 TX (RTX) |
| 46 | 2 | `threadCount` (≤ 10) |
| 48 | 2 | `crumbCount` (≤ 32) |
| 50 | 2 | `threadRecSize` = 32 |
| 52 | 12 | reservado, cero |
| 64 | 32 × n | hilos |
| 384 | 4 × n | migajas, de la más vieja a la más nueva |
| 512 | 2 | CRC-16 (XMODEM: poli `0x1021`, semilla 0) sobre los bytes 0..511 |
| 514 | 2 | reservado |

Un hilo (32 B): `name[12]` · `prio` (i8) · `state` (u8, codificación de Zephyr:
0 ready, 1 pended, 2 dead, 8 prestart, 16 suspended) · `stackFree` (u16, mínimo
histórico de bytes libres) · `stackSize` (u16) · `role` (u16, `enum bbBeat`+1, 0
si es un hilo que no se vigila) · `pc`, `lr`, `sp` (u32).

Una migaja (4 B): `id << 24 | (ms & 0xFFFFFF)`.

El `role` es lo que hace legible la tabla: los hilos vigilados se reconocen por
su papel (`ui`, `rtx`, `proto`, `main`, `kbd`) aunque el sistema operativo los
llame de otra forma. El nombre real se guarda igual (`CONFIG_THREAD_NAME`).

### 2.3 De dónde salen PC y LR

Cuando una excepción entra en un hilo, el Cortex-M apila `{r0-r3, r12, lr, pc,
xpsr}` en su propia pila, y el cambio de contexto guarda ese PSP en el TCB
(los registros callee-saved van al TCB, no a la pila:
`arch/arm/core/aarch32/swap_helper.S`). Así que el marco está en `psp`, `lr` en
`+0x14` y `pc` en `+0x18`.

Un valor solo se informa si cae dentro de la imagen de aplicación
(`0x18000000`+1 MiB) o del ITCM: un hilo a medio conmutar produce números que
parecen direcciones y no lo son. Un post-mortem que miente es peor que uno que
dice cero.

Para el hilo que provocó el fallo, el marco bueno es el del `esf` que recibe el
manejador, y ese es el que se guarda.

---

## 3. Qué pasa cuando se detecta

1. `bb_supervise()` deja de vigilar (`bbArmed = false`) y llama al hook de
   plataforma. **Un atasco se informa una sola vez**: si el reinicio no prende,
   el supervisor no vuelve cada 100 ms a borrar y reescribir el sector (diez
   borrados por segundo borrarían el registro del cuelgue con el registro de la
   reescritura).
2. `bb_capture()` construye el registro (instantánea del RTX, `state`, tabla de
   hilos, migajas) y calcula el CRC.
3. `bb_platformPersist()` borra el sector `0x6000` y escribe los 516 bytes.
4. `bb_platformReboot()`: PA fuera (`radio_disableRtx()`), DSP parado
   (`lsf_dsp_terminate()`) y `sys_reboot(SYS_REBOOT_COLD)`. Después, si el SoC
   no toma el reinicio, un bucle apretado con las interrupciones abiertas —nunca
   `k_cpu_idle()`— para que el perro guardián de hardware reinicie en su ventana.

Al arrancar, `bb_c62_init()` (desde `platform_init_csk6()`, justo después de
`nvm_init()`) lee el registro; si es válido lo imprime por el UART de consola
—antes de que el protocolo PC se apropie de él— y escribe cero sobre la palabra
de validez (4 bytes, sin borrar el sector: un `1 -> 0` sobre el bloque de
escritura es legal en NOR). El registro queda además en RAM para servirlo por el
protocolo hasta el siguiente cuelgue.

### 3.1 El reinicio, extremo a extremo

El camino se escribió como si «pedir el reinicio» bastara, y no basta: el 26 de
noviembre de 2026 la radio registró un `STALL` a los 17,2 s y siguió viva hasta
los 75 s, con la UI muerta y el protocolo contestando. Los tres puntos donde se
puede quedar atascado el reinicio, y lo que hace este puerto con cada uno:

| punto | qué puede pasar | qué se hace |
|---|---|---|
| el registro no se puede escribir | el sector está gastado o el driver de flash se atasca | se pide el reinicio **igual**: un post-mortem no puede ser el precio de volver a recibir |
| el reinicio de la SoC no prende | `sys_reboot()` termina en un `k_cpu_idle()` eterno (el respaldo del kernel), y el perro guardián **pausado en reposo** no lo ve | el bucle del paso 4 no duerme, `platform_wdtStart()` ya no pide `WDT_OPT_PAUSE_IN_SLEEP`, y el supervisor deja de alimentarlo en cuanto hay atasco |
| el DSP sigue corriendo | `dsp.h` lo dice en la declaración de `lsf_dsp_terminate()`: «un reset del núcleo de aplicación deja el DSP corriendo». Y el comentario de `platform_terminate()` añade lo que se ve después: con el cargador puesto el chip no se apaga con el reset, y la interrupción del UART del DSP es lo primero con lo que tropieza el arranque siguiente | `lsf_dsp_terminate()` antes de `sys_reboot()`, igual que en el apagado |

Nada de esto toma un cerrojo ni escribe flash *después* del registro: el PA y el
DSP son escrituras de registro, `sys_reboot()` toma las interrupciones y nada
más, y la única escritura de flash del camino es la del propio registro, que ya
ocurrió. Si algo de eso se atasca, el camino que queda es el perro guardián, que
es justo el que el supervisor deja de alimentar en ese instante.

### 3.2 Fallo duro, no cuelgue

Un *hard fault*, un `assert` o un desbordamiento de pila no son un atasco: el
hilo no se para, se muere. El puerto CSK6 instala su propio
`k_sys_fatal_error_handler()` (`csk/soc/arm/csk6/fatal.c`), que imprime una
traza y termina en `arch_system_halt()` — el hook débil del kernel para "para
esta CPU". Ahí es donde engancha `blackbox_c62.c`: es el único punto por el que
pasan todos los caminos fatales sin parchear un módulo, y su implementación por
defecto deja la radio muerta hasta el mando de volumen.

El nuevo `arch_system_halt()` escribe el registro (`reason = FATAL`, `detail` =
motivo de Zephyr, PC/LR leídos del marco de excepción que el hardware apiló en
el PSP del hilo) y reinicia **por el mismo camino que un atasco detectado**
(`bb_platformReboot()`: PA fuera, DSP parado, reset y respaldo por perro
guardián). Antes suspende al supervisor, y el kernel llama al hook con las
interrupciones cerradas: si la escritura se atasca (un fallo dentro del driver
de flash es posible), el perro guardián de hardware reinicia igual y el registro
es el del cuelgue anterior.

---

## 4. Cómo se lee

### 4.1 Por el puerto serie, al arrancar

Con cualquier build (con o sin protocolo PC), el arranque siguiente al cuelgue
imprime:

```
=== C62 post-mortem from the previous boot ===
C62 post-mortem: record v1, 516 B, crc ok
reason: STALL detail 0x00000002 [rtx]
uptime: 101100 ms (1 min 41 s), reset cause 0x00000000
radio: rtx=AM(10) ui=AM(10) flags 0x01
  rx 145500000 Hz, tx 145500000 Hz
threads: 7
  ui         pr 5  st ready     free  512/2048  role ui    pc 0x1801a2b4 lr 0x1801a2b8 sp 0x200c1234
  ...
breadcrumbs: 32 shown of 41 marks, oldest first
  +100000 ms  MODE_ENTER
  +100250 ms  RADIO_CONF
  ...
=== end of post-mortem ===
```

`115200 8N1`, el mismo cable de siempre. La primera línea es la que hay que
copiar en el informe de un fallo.

### 4.2 Por el protocolo de depuración

Con un build con `-DC62_PC_PROTO=ON -DC62_DEBUG_PROTO=ON`, el mensaje nuevo
`PC_MSG_DBG_BLACKBOX` (`0x0552`) devuelve el registro **crudo** en trozos:

* petición: `PC_TAG_OFFSET` (`0x00EB`, u32) = offset desde 0;
* respuesta: `PC_TAG_OFFSET` (eco), `PC_TAG_BLACKBOX_LEN` (`0x00F1`, u16, total)
  y `PC_TAG_BLACKBOX` (`0x00F0`, bytes, hasta 200 por respuesta);
* se pide en bucle desde 0 hasta llenar `BLACKBOX_LEN`.

El cliente de referencia lo hace todo y lo imprime legible:

```sh
tools/cps/blackbox.py /dev/ttyUSB0          # o -f registro.bin
tools/cps/blackbox.py /dev/ttyUSB0 -o pm.bin   # guarda los 516 bytes
```

Si no ha habido cuelgue desde la última lectura, el mensaje responde
`PC_ERR_NOT_SUPPORTED` y el script lo dice: no hay post-mortem que enseñar.

### 4.3 De una lectura de flash

Los 516 bytes de `storage_partition + 0x6000` son el registro tal cual, así que
sirven `cskburn` (leer) o cualquier volcado de flash:

```sh
tools/cps/blackbox.py -f volcado_0x2b6000.bin
```

---

## 5. Cómo se interpreta

* `reason: STALL` con `detail` = máscara: los bits dicen qué latido se paró
  (bit0 ui, bit1 rtx, bit2 proto, bit3 main, bit4 kbd). `[rtx]` significa que el
  hilo de radio dejó de dar vueltas. Los cinco son hilos distintos: `[kbd]` a
  secas apunta a la matriz del teclado o a sus GPIO, y `[ui kbd]` es algo que se
  lleva por delante a los dos a la vez.
* `state` de cada hilo: `pended` es un hilo esperando algo (un mutex, una cola,
  un stream); `ready` en el hilo parado suele ser un bucle sin dormir.
* `free` muy bajo (o 0) en un hilo es un desbordamiento de pila a punto de
  ocurrir; con `reason: FATAL` y `detail` 3 (`K_ERR_STACK_CHK_FAIL`) es lo que
  ya ocurrió.
* Las migajas, leídas al revés desde la última: la última marca es el último
  camino que el firmware entró antes de pararse. `MODE_ENTER` seguido de nada es
  un cambio de modo que no llegó a `MODE_READY`; `RADIO_CONF` es
  `radio_updateConfiguration()`; `AUDIO_OPEN` es una ruta de audio que se quedó
  en el driver.
* `rtx` y `ui` distintos en el registro son precisamente un cambio de modo a
  medias: la interfaz ya pidió el nuevo y el hilo de radio aún no lo aplicó.

---

## 6. Coste

Medido con la build completa (todas las banderas, `-DC62_PC_PROTO=ON
-DC62_DEBUG_PROTO=ON -DC62_BENCH_PROTO=ON -DC62_AIR_TX=ON`), contra el mismo
commit sin la caja negra:

| concepto | coste |
|---|---|
| `bb_mark()` | 60 B de código, 12 instrucciones: 2 cargas, 1 AND, 1 OR, 2 stores. Sin cerrojos, sin reloj, sin llamadas |
| `bb_beat()` | 60 B; 1 incremento + 1 lectura de reloj + 1 store, una vez por vuelta del hilo |
| FLASH | **+5.528 B** (793.044 → 798.572 B, 75,63 % → 76,16 % del MiB). De ellos 2.668 B son el grabador, el detector y el volcado de texto; el resto, las tres opciones de kernel |
| RAM | **+5.904 B** (180.472 → 186.376 B, 68,84 % → 71,10 % de 256 KiB): 3 KiB de pila del supervisor, 1 KiB de pila de ISR (el manejador de fallo escribe desde ella), 516 B de copia del registro, el anillo de 128 B y ~50 B por hilo de las dos opciones de kernel |
| PSRAM | **sin cambios** (653.910 B) |
| flash reservada | 1 sector de 4 KiB, de los que se usan 516 B; se escribe **solo** al colgarse |

Tamaños por función en el ELF: `bb_mark` 60 B, `bb_beat` 60 B, `bb_supervise`
156 B, `bb_recordBuild` 432 B, `bb_recordValid` 96 B, `bb_textDump` 1.232 B
(es la que gasta: cadenas y números; solo se ejecuta al arrancar y detrás de un
cuelgue).

Las tres opciones de kernel están en `c62_defconfig` y son lo que convierte la
tabla de hilos en algo legible (nombre, estado, mínimo histórico de pila libre
con `k_thread_stack_space_get()`). Sin ellas el registro sigue escribiéndose,
con nombres vacíos y sin mínimo histórico.

El hilo del teclado (`kbd_thread`, 1 KiB de pila) no es de la caja negra: llegó
con el arreglo de la congelación de la banda aérea, porque el latido del teclado
exige un hilo que lo merezca (docs/RX_AIRBAND.md §9). Cuesta 1 KiB de RAM más
~20 B de TCB, y ahorra los dos hilos que antes escaneaban la matriz a la vez.

---

## 7. Límites

* Un cuelgue que se lleva por delante **también al supervisor** (interrupciones
  cerradas para siempre, un bucle en una ISR de prioridad más alta) no deja
  registro: el `ap_wdt` reinicia a ciegas, como antes.
* Si el cuelgue está dentro del driver de flash, la escritura del registro no
  completa: el CRC lo delata y el reinicio lo da el perro guardián.
* **El reinicio no es una garantía del software, es una cadena de tres.** El
  reset de la SoC, si el SoC lo toma; si no, el bucle sin dormir del paso 4; si
  no, el `ap_wdt` sin alimentar y **sin pausa en reposo**. Lo que sí está
  garantizado por código —y comprobado en el host— es que no se toma ningún
  cerrojo ni se escribe flash después del registro: los tres eslabones dependen
  del hardware, no de un hilo que pueda estar atascado.
* El registro es **uno**: el siguiente cuelgue pisa al anterior. Se lee al
  arrancar justo por eso. Un atasco se informa una vez y no se reescribe.
* Un hilo que se pasa 2 s dentro de una sola operación —una escritura grande de
  canales desde el CPS, por ejemplo— se considera colgado. Es deliberado: un
  hilo que no da una vuelta en 2 s no está sirviendo a nadie, y las escrituras
  de este firmware (4 KiB de sector) tardan decenas de milisegundos, no
  segundos.
* El tick de las migajas es de 24 bits (4,6 h) y su resolución es la del último
  latido o de la pasada del supervisor (≤ 100 ms).
* Un cuelgue determinista al arrancar produce un ciclo de reinicios; cada vuelta
  deja el mismo registro, que es justo lo que hace falta para diagnosticarlo.

---

## 8. Tests

```sh
meson setup build_linux && meson compile -C build_linux blackbox_test
meson test -C build_linux "Blackbox Test"     # o la suite entera
```

`tests/unit/blackbox.cpp` (11 casos, 140 aserciones):

| caso | qué comprueba |
|---|---|
| hilo que deja de latir | con un `std::thread` real y reloj de mentira: nada a 1999 ms, `STALL` a 2000, máscara exacta, latidos ausentes no vigilados |
| hilo que sigue latiendo | cuatro pasadas de 600 ms sin falso positivo, y el atasco se cuenta desde la última pasada |
| supervisor suspendido | no dispara; al reanudar se re-ceba y el hueco no cuenta |
| contenido del registro | magic, versión, validez, CRC, uptime, motivo, modo, frecuencias, tabla de hilos (nombre/prio/estado/pila/rol/pc/lr/sp) y las migajas en orden con su tick |
| registro corrupto | bit cambiado en cualquier campo, CRC tocado, sector borrado, versión ajena, registro truncado, contadores fuera de rango: todos rechazados |
| captura sin flash | si la escritura falla, el registro se construyó igual (lo que ve el banco) |
| lectura al arrancar | flash falsa con semántica NOR: vacía no hay registro, tras el cuelgue se lee, la invalidación son 4 bytes sin borrar el sector, y no se sirve dos veces |
| escritura a medias | 200 de 516 bytes: registro inválido, no un registro equivocado |
| texto | el volcado nombra motivo, hilos, PC y migajas (con `DISP_DROP` y `[ui kbd]` entre ellas); ninguna línea pasa de 120 caracteres |
| sin hilos ni migajas | sigue renderizando |
| teclado vigilado | el latido del escáner de teclado para por sí solo: máscara `1<<4` y nombre `kbd` |

`tests/unit/blackbox_hang.cpp` (4 casos, 44 aserciones) prueba el camino que el
otro fichero sustituye: la reacción **por defecto** a un atasco, la que corre en
la radio.

| caso | qué comprueba |
|---|---|
| atasco → registro → reinicio | el registro se escribe **antes** de pedir el reinicio, con `STALL`, la máscara y el uptime del instante |
| escritura imposible | si la flash falla, el reinicio se pide igual |
| un atasco, un registro | veinte pasadas más después del reinicio fallido: ni una escritura más, ni un reinicio más |
| teclado vigilado | el `detail` del registro nombra el teclado cuando es el teclado el que se para |
