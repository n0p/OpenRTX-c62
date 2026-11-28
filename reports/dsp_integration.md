# Reducción de ruido dentro del firmware del DSP: drivers, encaje, fases y riesgos

Fecha: 2026-11-28 · Rama `dsp/integration-research` · Ámbito: investigación; no se ha tocado
la radio, ni `/dev/ttyUSB*`, ni el código de producción. Lo que se ha compilado y ejecutado
está marcado como tal.

Punto de partida verificado en este árbol: el algoritmo corre en el HiFi4 **bit-exacto**
(3 840/3 840 muestras contra el M33; 15 360/15 360 en el crosscheck de tres vías) a
**671 441 instrucciones / 933 066 ciclos por trama de 10 ms = 31,1 %** (`reports/dsp_ns.md`).
Es decir: el problema que queda **no es el algoritmo ni el coste**, es el *bring-up* del
firmware que lo rodea.

---

## 0. La respuesta corta

**Sí se puede llegar a «la radio suena con la reducción de ruido en el DSP»**, y el camino es
más corto de lo que sugiere el estado del esqueleto: los drivers de audio del CP **existen como
fuente en este árbol** (`extract/hal/hal_listenai/csk6/drivers/hifi4/*.c`) y **compilan hoy para
`venus_hifi4`** (comprobado: 0 errores, 0 avisos, ~46 KB de texto). La etapa de ruido está
medida y es *in-place* sobre la misma trama de 480 muestras que ya viaja por el anillo.

Lo que más probabilidades tiene de pararlo, por orden:

1. **El arranque de nuestra propia imagen DSP.** Hoy `dsp-openfw/build/dsp.elf` **no arranca**:
   su `_start` está en `0x600002f4` y no en `0x60000000` (el AP salta siempre a
   `0x60000000`), no inicializa el puntero de pila, no pone a cero `.bss` y no instala
   `vecbase`; en QEMU muere con `EXCCAUSE=15` antes de 3 s (§3). No es un
   problema de audio, es un `crt0` que falta. ~1 día, pero hasta que no esté, nada de lo demás
   se puede probar.
2. **El APC/DMA sólo se puede validar en la radio.** No hay modelo de APC en ningún emulador
   que tengamos (QEMU `-M sim` no tiene ni APC, ni DMA, ni mailbox). Y el PDF del chip que
   citan los propios headers (`Venus_Chip_Specification.pdf`, `venus_cp.h:452`) **no está en el
   árbol**. El modo de fallo es el peor posible: *códec en silencio sin error*.
3. **La revisión del HAL no es la de la imagen que corre hoy.** Nuestro `extract/` es de otra
   compilación (`adc_pdm_venus.c:346` no coincide), misma API pero no el mismo binario. Hay que
   validar contra hardware, no por inspección.
4. **El coste en silicio.** El 31,1 % es del ISS sin cachés. Con una D-cache de 16 KB y un
   *working set* de ~150 KB (estado 62 KB + pesos 87 KB), cada trama recorre todo desde PSRAM.
   Es acotable (estado y pesos caben en DRAM1, que está libre), pero el número real puede ser
   35–45 %, lo que sigue cabiendo y sigue sin cumplir el criterio de producción (≤10 %).

Lo que **no** va a pararlo: la licencia del toolchain (XOS 2.10 viene con las herramientas y
está instalado), el tamaño (cabe de sobra en la partición de 1 MB) ni el contrato con el AP
(no hace falta tocar el protocolo).

---

## 1. Los drivers de audio del DSP

### 1.1 Qué hardware hay, y con qué nombres

El CP ve el códec y el DMA por el bus de periféricos; el mapa está en
`extract/hal/hal_listenai/csk6/drivers/hifi4/include/hal/venus_cp.h:118-163`:

| Bloque | Base (vista CP) | Qué es | Evidencia |
|---|---|---|---|
| `AON_CODEC` | `0x46E20000` | códec del dominio AON: **ADC01** (PDM2PCM/AMIC) | `venus_cp.h:142`, `audio_codec_venus.h:406` |
| `CP_CODEC` | `0x49100000` | códec del CP: **ADC23** y **DAC01** | `venus_cp.h:147`, `audio_codec_venus.h:407,623` |
| `APC` | `0x49000000` | *Audio Processing Center*: FIFOs, EQ, mezcla, eco | `venus_cp.h:146` |
| `DMAC` (CP) | `0x4A200000` | DMA genérico del CP, 8 canales | `venus_cp.h:157`, `485` |
| `AUDPLL_CTRL` | `0x46030000` | PLL de audio | `venus_cp.h:136` |
| `CMN_SYSCTRL` | `0x46000000` | relojes y *gating* | `venus_cp.h:133` |
| `VAD_REG/RAM` | `0x49200000` / `0x49300000` | detector de voz por hardware | `venus_cp.h:148-149` |
| `CMN_MAILBOX` | `0x46100000` | buzón AP↔CP (el que usa el LSF) | `venus_cp.h:137` |

El AP tiene **otro** DMA (`0x40000000`, comentado como «AP DMA» en `venus_cp.h:118`): los dos
dominios no comparten controlador.

### 1.2 El modelo de transferencia: APC ⇄ DMA, no «ADC a pelo»

Esto es lo que cambia el diseño respecto a lo que asume el esqueleto. Los drivers de códec **no
hablan con el DMA directamente**: piden un canal al APC y el APC es el que mueve datos con DMA.

* `ADC_PDM_Initialize()` → `apc_initialize()` → `apc_dual_channel_acquire(APC_INTF_ADC, …)`
  (`adc_pdm_venus.c:261-268`). Igual el DAC con `APC_INTF_DAC` (`dac_venus.c:218`).
* `apc_initialize()` habilita el reloj del APC, resetea las dos mitades, **registra y habilita
  la IRQ 4**, inicializa el DMA (**IRQ 3**) y enciende el APC (`apc_venus.c:395-445`).
* La lectura/escritura real es `apc_channel_read()`/`apc_dual_channel_write()`, que dentro
  hacen `dma_channel_configure()` con: fuente/destino fijo en el registro de datos del canal del
  APC, `DMA_CH_CFGH_FIFO_MODE`, y el **handshake** del periférico
  (`SRC_PER(DMA_HSID_…)`, `apc_venus.c:1073-1090`).
* Para flujo continuo hay variante *scatter-gather*: `dma_channel_configure_LLP()` con una lista
  `AUDIO_BUFFER_LLI` (`apc_venus.c:1487` lectura, `apc_venus.c:1625` escritura) — es el
  equivalente al doble/triple buffer clásico, sin parar el canal entre bloques.
* El fin de bloque llega como **callback**: `DMA_SignalEvent_t(event_info, xfer_bytes,
  usr_param)` (`dma.h:176`), que el APC reenvía al driver
  (`apc_dma_event` → `CSK_ADC_PDM_SignalEvent_t` / evento del DAC). Los eventos son
  `RECEIVE_COMPLETE`, `RX_FIFO_OVERRUN`, `RX_FIFO_FULL`
  (`Driver_ADC_PDM.h:162-165`).

Parámetros del transporte, del propio HAL:

| Cosa | Valor | Evidencia |
|---|---|---|
| Canales DMA del CP | 8 | `venus_cp.h:485` |
| Handshake APC OUT CH0..3 / IN CH0..5 | IDs 0–3 / 4–9 | `venus_cp.h:453-462` |
| Canales por defecto | ADC01L=1, ADC23L=2, DAC01L=3, ADC01R=4, ADC23R=5, DAC01R=6 | `venus_cp.h:488-499` |
| Ancho de muestra | `DMA_WIDTH_HALFWORD` para PCM16 (offset +2 en el registro para el canal R) | `apc_venus.c:1088-1091` |
| Ráfaga | `DMA_BSIZE_1..256`; el APC limita a `<= DMA_BSIZE_16` | `dma.h:31-39`, `str-factory.txt:585` |
| IRQs relevantes | 0 mailbox, 2 dual-timer, 3 DMAC, 4 APC, 5 NPU, 17 AP2CP combinada | `venus_cp.h:248-266` |

**Consecuencia de diseño:** la ruta de audio del CP es *asíncrona y dirigida por interrupción*.
El esqueleto actual (`main_dsp.c`) es un bucle que sondea el buzón y fabrica una rampa; no hay
reloj de audio, ni interrupciones, ni `vecbase`. Eso es exactamente lo que hay que añadir.

### 1.3 Qué usa la imagen de fábrica

No es deducción: el blob de fábrica lleva **la misma familia de drivers compilada dentro**, con
sus `__FILE__:__LINE__` en los `assert`:

| Cadena en `dsp-factory.bin` | Fichero:línea del HAL | Línea en `str-factory.txt` |
|---|---|---|
| `adc_pdm/adc_pdm_venus.c:346 adc->info->ch_bmp != 0` | `adc_pdm_venus.c` | 519 |
| `adc_pdm/adc_pdm_venus.c:1933 ret == CSK_DRIVER_OK` | idem | 548 |
| `apc/apc_venus.c:921 pdci->dma_bsize_bits <= DMA_BSIZE_16` | `apc_venus.c` | 585 |
| `apc/apc_venus.c:993 (uint32_t)pdci->cfg_reg_p == (uint32_t)rx2_cfg_p` | idem | 591 |
| `apc/apc_venus.c:2625 pdci->dma_ch_lr[lr_idx] < DMA_NUMBER_OF_CHANNELS` | idem | 629 |
| `dma/dma_venus.c:858 size <= MAX_BLK_TS` | `dma_venus.c` | 687 |
| `%s: NO free DMA channel!!` / `Failed to call dma_channel_configure!!` | `apc_venus.c` | 599-601 |
| `error:ADC_PDM_Receive(%d)` / `error:DAC_SEND(%d)` | `modules/codec/mic.c`, `spk.c` | 53, 113 |
| `mods/codec/mic.c:211`, `mods/codec/spk.c:397` | capa de aplicación del SDK | 64, 74 |

Los literales de las bases también están en el blob: `0x49000000` ×3, `0x49100000` ×6,
`0x46E20000` ×3, `0x4A200000` ×2, `0x46030000` ×2 (barrido de `struct.pack('<I')` sobre
`work/dsp-factory/dsp-factory.bin`). Y arranca como imagen XOS (cabecera `0x1006`, `vecbase`
`0x60000400`, store-table de 44 entradas) — `reports/dsp_factory.md:74,93,97`.

La cadena que describe el reverse previo (`reports/dsp_factory.md:212-216`) es coherente con
todo esto: **RX** `ADC1` (discriminador del BK4819) → resample 48k→16k → CAE(NS/AGC/VAD) →
EQ → 16k→48k → `DAC0` (altavoz); **TX** `ADC0` (micrófono) → CAE → `DAC1` (entrada de audio del
BK4819); más un canal **ECHO** (`IN2` copia de `OUT1`, `apc_venus.h:115-125`) que el CAE de
fábrica no usa porque no hace AEC.

**Aviso de honestidad:** el *orden* exacto dentro de `algo_run_proc` no está confirmado
instrucción a instrucción (FLIX), y el reparto ADC01/ADC23 entre micrófono y radio es
**deducción** del código de la aplicación de fábrica, no una traza.

### 1.4 La imagen que la radio carga hoy **también** lleva los drivers

Esto es lo que reordena todo el plan. La imagen que OpenRTX copia a `0x60000000` es
`dsp48k.bin` / `resources/dsp_firmware.bin` (sha256 `9579b617…`, 466 832 B;
`platform/targets/c62/CMakeLists.txt:945`), y **ya inicializa el códec**:

```
str48k.txt:313  [ERR] AudioStreamInVenus: ADC_PDM_Receive() failed: %d
str48k.txt:356  [ERR] AudioStreamInVenus: ADC_PDM_Initialize() failed
str48k.txt:1411 ADC_PDM_Initialize      (símbolo)
str48k.txt:1421 /home/tanha/workspace/csk6/cp_6012_nano/modules/driver/src/adc_pdm/adc_pdm_venus.c:346
```

Es decir: **la radio suena hoy porque la imagen A trae los drivers**; `dsp-openfw` es una
reimplementación del *servidor* que todavía no los tiene. El trabajo de drivers no hace falta
para que haya sonido: hace falta para que el sonido lo ponga **nuestro** firmware, que es la
condición para meter la etapa de ruido dentro.

Cuidado con un detalle: nuestro `extract/` **no es el código fuente de la imagen A**. Misma API,
misma familia, otra revisión (en la imagen A el `assert` de `ch_bmp != 0` está en la línea 346;
en nuestro árbol la 346 es un `}`). Sirve para portar; no sirve para afirmar «esto es lo que
corre».

### 1.5 El HAL del vendor compila hoy para `venus_hifi4` (verificado)

Compilado en el scratch dir (sin tocar el repo), con `xt-clang --xtensa-core=venus_hifi4`,
`-DXOS_USED -DPLATFORM_CP -DCSK6001 -DIC_BOARD` y un *shim* de 4 cabeceras
(`toolchain/xcc.h` con `__packed/__IO/__aligned…`, un `venus_ap.h` vacío porque
`reg/dma_venus.h:16` incluye la cabecera del **AP** por error, y `zephyr.h` + `sys/printk.h`
vacíos porque `venus_log.h` los incluye para un `CLOG` que está `#if 0`):

| Fichero | .text | .data | .bss |
|---|---:|---:|---:|
| `adc_pdm_venus.c` | 8 802 | 24 | 48 |
| `apc_venus.c` | 9 780 | 0 | 220 |
| `dac_venus.c` | 7 610 | 12 | 20 |
| `dma_venus.c` | 7 834 | 4 | 400 |
| `ClockManager.c` | 11 671 | 0 | 0 |
| `IOMuxManager.c` | 592 | 0 | 0 |

0 errores y 0 avisos en los seis. Con `cache_cp.c` y `mbox.c` (que ya compilaban sin shim) son
**~46 KB de texto**: el «trabajo más grande que queda» de `reports/dsp_ns.md:340-343` es, en la
práctica, **pegamento y arranque**, no reverse.

### 1.6 Qué asume `dsp_sim` (y por qué no vale como modelo)

`src/main_dsp.c` es explícito: «the mailbox is polled (no vecbase, no interrupt setup)»
(líneas 8-10) y «Audio is intentionally a stub… Wiring ADC_PDM/DAC/APC from the HAL is the next
task» (11-14). Concretamente:

* `audio_task()` (113-128) rellena una rampa de 480 muestras y la empuja **cada vez que se
  ejecuta el bucle**, sin ninguna noción de 10 ms; y en reproducción saca la trama a un buffer
  estático que se tira (124-127). No hay reloj: si el bucle corre 10 000 veces por segundo, se
  empujan 10 000 tramas.
* El buzón se sondea **una trama por vuelta** (151), no por interrupción.
* `srv_hook_record_start/play_start` sólo levantan un flag (109-110).
* `af_set_parameters()` **no aplica nada**: parsea el `k=v` y lo imprime
  (`srv_lsf.c:437-467`). Es decir, las ganancias `ADC_PDM_GAIN_*`/`DAC_GAIN_*` que manda el AP
  (`csk6_codec.c:457-500`) se pierden hoy.

Para el protocolo, perfecto (`make test` → `PASSED (0 failures)`, `make dsp_sim` → `dsp_sim ok`,
ejecutados). Como modelo temporal, inútil: en la radio todo esto lo marca el DMA del APC.

### 1.7 Seguro / deducido / desconocido

**Seguro (leído en fuente, compilado o medido aquí):**
mapa de periféricos y sus bases; números de IRQ; IDs de handshake y canales DMA por defecto;
modelo FIFO+DMA con callback; API de ADC/DAC/APC; soporte de 48 kHz en captura
(`adc_pdm_venus.c:1074`) y reproducción (`Driver_DAC.h:30`); que la fábrica y la imagen A llevan
esta familia de drivers; que el HAL compila para el core; que XOS 2.10 está instalado
(`venus_hifi4/xtensa-elf/arch/lib/libxos.a`, fuente en `XtensaTools/xtensa-elf/src/xos`, doc en
`XtensaTools/doc/xos-2.10.zip`).

**Deducido (coherente, no probado):** que el reparto es ADC01 = (L mic, R radio) y DAC0 = altavoz,
DAC1 = TX; que la interrupción que despierta la aplicación es la del DMA y no la del APC (el
código registra las dos; `apc_irq_handler` existe pero su uso final no está descompilado); el
tamaño de bloque del APC en la fábrica (16 bits de muestra, ráfaga ≤16).

**Desconocido (y hay que decirlo):**
* El microcódigo del APC (EQ, mezcla, *rate conversion*). Sólo conocemos el interfaz de registros.
* La latencia real de PSRAM. `XCHAL_HAVE_DCACHE_DYN_ENABLE=0` (caché siempre activa, 16 KB, 4
  vías; I-cache 32 KB, LX7.1.4), pero nadie ha medido el coste de un *miss* en este chip y el
  PDF del chip no está en el árbol.
* Los valores de calibración del códec (ALC, HPF, offset DC, ganancias de arranque) que pone
  `mic.c`/`spk.c` de la fábrica: están en el blob, no descompilados.
* Si el APC del C62 (CSK6011B) es la misma revisión que la del SDK del que salió `extract/`.

---

## 2. Dónde encaja la etapa de ruido en el camino del servidor LSF

### 2.1 Dónde está hoy la reducción de ruido

En el AP, dentro del worker del códec, sobre la trama **capturada** y **sólo en el canal de
radio**, justo antes de la cola del demodulador:

```c   // resumido; el original está en las líneas citadas
platform/drivers/audio/csk6_codec.c:836-859
  for (source = 0..1)
    raw = &samples[rec->mChannelOutIdx[source]]
    if (source == SOURCE_RTX && count <= CAPTURE_FRAME_SAMPLES
        && noiseSuppress_enabled() && outputSource[SINK_SPK] == SOURCE_RTX)
        nsSamples[i] = raw[i*stride];            // de-intercala
        noiseSuppress_process(nsSamples, count); // rnnoise, 480 muestras
        played = nsSamples; playedStride = 1;
  captureToFmQueues(source, played, playedStride, count);   // fuera del for
```

Es decir: **entre el anillo de captura del DSP y el demodulador**. No toca los streams que leen
los modos digitales (reciben las muestras crudas), ni la ruta del micrófono.

### 2.2 Dónde tiene que ir en el CP

En el mismo punto del camino, pero un salto antes: **entre el buffer que llena el DMA del ADC y
`srv_record_push()`**. En el servidor de `dsp-openfw` eso es una llamada dentro de la tarea de
audio, sobre la trama de 480 muestras que ya está en memoria:

```
[DMA ADC01, L=mic R=radio] → buffer de trama (480×2 int16, 1 920 B)
   → si (canal radio) ns_dsp_process(estado, canal_R)      <-- etapa nueva, in-place
   → srv_record_push(trama, 1 920)  → anillo 3 → PUSH EP2 al AP
[anillo 0] → srv_play_pull() → buffer de trama → DMA DAC01 (L altavoz, R TX)
```

Por qué encaja sin fricción:

* **Misma trama.** `NS_DSP_FRAME = 480` (`dsp/ns/ns_dsp.h`) y el AP pide 48 kHz con tramas de
  10 ms = 480 muestras (`csk6_codec.c:48-58`), y los anillos se crean a 480 muestras ×2 canales
  (`srv_lsf.c:888-890`). No hay remuestreo ni cambio de geometría.
* **Mismo punto de señal.** El AP ya procesa la señal que sale del ADC y la entrega ya filtrada
  al demodulador; poner la etapa en el CP, en ese mismo punto de la cadena, da *exactamente* la
  misma señal con los mismos bits (medido bit-exacto en el banco).
* **Latencia cero añadida.** Al ser in-place y por trama completa, la etapa no acumula muestras:
  la trama que entra es la que sale.

### 2.3 Buffers y memoria

| Objeto | Bytes | Dónde |
|---|---:|---|
| anillo de captura (4 tramas × 480 × 2ch × 2 B) | 7 680 | banco compartido `0x200C0000` |
| los dos anillos de reproducción | 15 360 | idem |
| estado de la etapa (`ns_dsp_state_bytes()`) | **62 312** | `.bss` (PSRAM) o DRAM1 |
| scratch de de-intercalado (1 canal × 480) | 960 | pila o `.bss` |
| pesos `int8` de RNNoise v0.1.1 | 87 503 | `.rodata` |
| código del port + tablas | ~110–160 KB (medido: el programa de banco completo, con 8 tramas de PCM de prueba y arnés, son 162 912 B de texto) | `.text` |

El banco compartido no se toca (23 KB de 64 KB usados). La etapa vive en memoria privada del
CP; el AP no la ve.

### 2.4 Qué tiene que cambiar en el AP

Casi nada, y todo *fuera* del cable:

1. **Apagar la etapa del AP** cuando la haga el DSP (hoy `CONFIG_C62_NOISE_SUPPRESS`, ver
   `csk6_codec.c:840`). Si no, se aplica dos veces.
2. **Opcional, sin tocar el protocolo:** el `k=v` de `AudioSystem_setParameters` ya llega al CP
   como texto (`srv_lsf.c:437-467`). Un `NS=dsp` ahí es compatible hacia atrás: el servidor que
   no lo entienda lo ignora, como hoy. Permite conmutar en caliente y volver al camino del AP
   como *fallback*.
3. **Nada más.** No hace falta un `ICStream` nuevo, ni otro anillo, ni un RPC nuevo, ni cambiar
   `frameCount`, ni la profundidad de los anillos. El contrato de `docs/INTEGRACION_AP.md` no se
   toca.

Lo que **no** conviene: poner la etapa en la ruta de reproducción (anillo 0/1 → DAC). Ahí el NS
procesaría al altavoz lo que ya viene demodulado por el AP, y no es donde está hoy; cambiaría la
señal que oye el usuario sin que el banco actual lo respalde.

---

## 3. Plan por fases

Cada fase, con su criterio medible, lo que se puede validar **sin radio**, y el estado real de
partida (comprobado hoy, no supuesto).

### Fase 0 (previa, imprescindible): que la imagen arranque

> **Cerrada el 2026-11-28.** La imagen arranca, no cae en ninguna trampa en 30 s
> en QEMU y el servidor contesta `lsf_connect()` del AP por el mailbox emulado.
> Evidencia, decisiones (bare-metal + `-mabi=call0`) y los dos defectos de cable
> que aparecieron: `reports/dsp_bringup.md`. La tabla de abajo es el estado de
> partida, se deja como registro.

No estaba en el encargo, pero sin esto la fase (a) no existe. Estado inicial, medido:

| Comprobación | Resultado | Evidencia |
|---|---|---|
| `_start` en el mapa | `0x600002f4` | `xt-nm build/dsp.elf` |
| primeros bytes de la imagen | `08 00` = `l32i.n a0, a0, 0` (un literal), no código | `xt-objdump -D -b binary build/dsp_firmware.bin` |
| pila | `entry a1, 32` sin haber inicializado `a1` | desensamblado de `_start` |
| `.bss` | 4 200 B + `.clib.bss` 740 B; **no hay bucle de puesta a cero** en `_start` | `xt-readelf -S` + desensamblado de `_start` |
| `vecbase` / interrupciones | no se instalan | `main_dsp.c:8-10` |
| en QEMU (`-M sim -cpu venus_hifi4`) | `PC=0x6000067c`, `EXCCAUSE=0x0f` (LoadStorePIFAddrError), `EXCVADDR=0x10` a los 3 s | ejecutado |
| `make dsp` | falla: `XT_TOOLS` apunta a `$(CURDIR)/toolchain/…`, que no existe | ejecutado |

**Criterio de terminado:** `xt-objdump` de `dsp_firmware.bin` muestra un `j`/`entry` válido en el
offset 0; en QEMU el PC se queda dentro de `.text` 10 s sin excepción; `_bss_start.._bss_end` a
cero; `vecbase` apuntando a una tabla de 16 vectores válida.

**Coste:** 8–16 h. Se puede hacer de dos maneras: (i) `crt0` propio de ~40 líneas (fijar `a1`,
cero de `.bss`, `wsr.vecbase`, `rsil 0`) + arreglo del linker script (`KEEP(*(.init))` primero y
`.literal` detrás de `.text`); o (ii) apoyarse en XOS (`crt1-boards.o` + `libxos.a`), que ya trae
despacho de interrupciones y semáforos, pero cuyo BSP asume otro mapa de memoria y habrá que
sustituir su linker script por el nuestro.

> Nota de por qué importa: los drivers del vendor llaman a `register_ISR()`, `enable_IRQ()`,
> `disable_IRQ()` y esas tres **son vacías si no se compila con `XOS_USED`**
> (`venus_cp.h:396-426`, el `#else` está comentado). Sin XOS hay que implementar esos tres
> ganchos; con XOS hay que pagar su `crt`. Es la decisión de arranque, y hay que tomarla en la
> fase 0.

### Fase (a) — audio de ida y vuelta en el emulador con el core importado

Qué se hace: sustituir los stubs de `main_dsp.c` por una fuente/sumidero respaldados por fichero
(WAV → trama → `srv_record_push`; `srv_play_pull` → WAV) y un reloj virtual (un contador que
avanza 10 ms por trama, ya que no hay APC que lo marque). No se pretende emular el APC.

**Criterio medible:**
* 300 tramas (3 s) de ida y vuelta por el servidor: la salida es **bit-idéntica** a la entrada
  (el camino LSF no debe tocar las muestras).
* El test de contrato sigue en `PASSED (0 failures)` y el AP simulado ve incrementos de
  `dataFrames`/`emptyFrames` coherentes (sin pérdidas ni duplicados en 3 s).
* Conteo de instrucciones por trama del bucle completo en el ISS (`xt-run
  --client_commands=summary`), como cifra de referencia.

**Validable sin radio:** todo. Es la fase que se puede dejar hecha y repetible.

**Estado:** no pasa (falla en fase 0). Con fase 0 hecha, es trabajo mecánico.

**Coste:** 16–24 h.

### Fase (b) — la etapa dentro del servidor, bit-exacta contra el banco actual

Qué se hace: enlazar `dsp/ns` en el servidor, reservar el estado de 62 312 B en `.bss`
(alineado a 8), y llamar `ns_dsp_init()` al arrancar y `ns_dsp_process()` por trama en la ruta de
captura, canal de radio, con de-intercalado a un scratch de 960 B y re-intercalado.

**Criterio medible:**
* **Bit-exactitud por el camino real**: las 8 tramas del banco, metidas por el anillo de captura
  y leídas como las leería el AP, dan **3 840/3 840 muestras idénticas** a la referencia del M33
  (`make compare` de `dsp/ns`, que hoy da ese número en el camino directo).
* Las 32 tramas del crosscheck (15 360/15 360) también, si el arnés se engancha al servidor.
* Coste: ≤ 933 066 ciclos/trama en el ISS **con** el servidor en el bucle (es decir, el servidor
  no debe añadir más de un pequeño porcentaje).
* Con la etapa desactivada por el `k=v`, la salida es idéntica a la de fase (a).

**Validable sin radio:** todo (ISS + host). Es la fase que da la garantía de que «lo que suena es
lo que se midió».

**Coste:** 12–20 h (la etapa existe; el pegamento son ~200 líneas).

### Fase (c) — la imagen DSP flasheable que arranca y suena

Qué se hace: integrar el HAL (`adc_pdm`, `apc`, `dac`, `dma`, `ClockManager`, `IOMuxManager`,
`cache_cp`) con XOS o con ganchos propios; escribir el pegamento de audio que hoy no existe:
`ADC_PDM_Initialize(cb)`, `ADC_PDM_Receive_LLP()` con dos buffers de 480 muestras, callback que
despierta la tarea, `DAC_Send_LLP()` con la lista enlazada de buffers desde el anillo; aplicar las
ganancias del `k=v`
(`ADC_PDM_SetVolume`/`DAC_SetVolume`) y la tasa de 48 kHz; y el reparto L/R → DAC0/DAC1.

**Criterio medible (en la radio, porque no hay emulador del APC):**
* La imagen mide < 1 MB y arranca: el handshake LSF completa (`lsf_connect()` no reintenta en
  bucle; hoy lo garantiza el fence EP3 h1, `docs/INTEGRACION_AP.md:24-27`).
* Con la radio en un canal analógico: el anillo de captura entrega tramas **no silenciosas** y
  con nivel dentro de ±3 dB del que da la imagen A con la misma señal (comparación A/B).
* Un tono de 1 kHz inyectado por el camino de reproducción sale por el altavoz con la amplitud
  esperada y sin *glitches* (grabarlo y mirar el espectro: un solo pico, sin espurias de
  conmutación de buffer).
* Continuidad: 10 minutos sin `RX_FIFO_OVERRUN` ni trama perdida (contadores del driver).
* Con NS on/off y la misma fuente de ruido, la diferencia de suelo de ruido es medible (>6 dB en
  el pasabanda de voz) y **la salida con NS del DSP coincide con la del AP actual** (≤1 LSB).

**Validable sin radio:** sólo el arranque y el handshake (QEMU). Todo lo demás necesita la radio.

**Riesgo y coste:** es la fase grande: 40–60 h de pegamento + 16–32 h de *bring-up* en la radio
(que incluye descubrir si el APC necesita algo que no está en los headers).

### Fase (d) — el AP usando la etapa remota

Qué se hace: `CONFIG_C62_NOISE_SUPPRESS=n` (o el `k=v` `NS=dsp`), y dejar el camino del AP como
*fallback* si la imagen no lo implementa.

**Criterio medible:**
* Con el mismo registro de basebanda, la salida al altavoz es la misma que con la etapa del AP
  (bit-exacta o ≤1 LSB).
* La carga del M33 baja en el coste medido de la etapa (contadores del *blackbox* ya existentes).
* La latencia total no cambia (misma profundidad de anillo; medir ida y vuelta con un tono).
* Si la imagen DSP no trae la etapa, el AP la aplica y no hay regresión (probarlo con la imagen
  A, que no la tiene).

**Validable sin radio:** parcialmente (con el AP en QEMU y el DSP en QEMU no hay enlace real; el
criterio fuerte es en radio).

**Coste:** 8–16 h.

---

## 4. Riesgos, y qué costaría cada callejón

### 4.1 El bring-up de la imagen (probabilidad alta, coste acotado)

Ya está medido que la imagen no arranca (§3, fase 0). Es el riesgo con **mayor probabilidad de
aparecer primero** y el más barato de cerrar. Pero arrastra una decisión con consecuencias: XOS
o *bare-metal*. Con XOS se hereda el despacho de interrupciones y los semáforos, y el `crt` de
Cadence; a cambio, su BSP asume un mapa de memoria de EVB y hay que sustituirlo, y el depurador
pasa a ser «XOS + nuestro mapa». Sin XOS hay que escribir 3 ganchos y una tabla de vectores, y no
hay planificador (el bucle actual basta si el audio se atiende por callback). **Coste de
equivocarse:** ~1 semana perdida en el segundo caso si el callback del DMA no se puede atender
desde una ISR sin aplazar trabajo.

Un detalle del contrato de carga que muerde justo aquí: el AP no copia el fichero, copia
**la partición entera de 1 MB** (`dsp.c:122-123`, `DT_REG_SIZE(DSP_FIRMWARE_NODE)`), así que
todo lo que haya en flash detrás de la imagen aterriza en PSRAM encima de `.bss` y de la pila.
`_start` tiene que poner a cero `.bss` y fijar `a1` **antes** de llamar a nada; la imagen de
fábrica lo hace, la nuestra no. Y conviene rellenar la partición de forma deliberada al flashear,
no confiar en lo que hubiera.

### 4.2 El APC es una caja negra con interfaz documentada a medias

Sabemos los registros (2 003 líneas de `apc_reg_venus.h`), no la semántica del microcódigo (EQ,
mezcla, *rate conversion*). El PDF del chip no está (`venus_cp.h:452` lo cita; no está en el
árbol ni en `extract/`). **Cómo se manifiesta:** códec en silencio, o con la tasa equivocada, sin
error del driver; el registro que falta no está en ningún `assert`. **Cómo se mitiga:** arrancar
de la configuración de la imagen A (que funciona) en lugar de inventar una: `ADC_PDM_Control(CSK_ADCPDM_SR_48KHZ)`,
AMIC (no DMIC), 16 bits, ganancias del AP. Y comparar registro a registro contra el blob de
fábrica si hace falta. **Coste si sale mal:** días de prueba y error en la radio, no semanas,
porque la referencia (imagen A) existe y es observable.

### 4.3 Revisión del HAL distinta a la de la imagen A

Misma API, distinto binario (§1.7). Un `sc_AMIC_SR_OSR` con otro `rc_ctrl`, o un `ALC` con otros
máximos, cambia el comportamiento sin fallar. **Mitigación:** validar contra A/B de nivel y
espectro (fase (c)), y no fiarse de «compila».

### 4.4 El coste en silicio

31,1 % es un número del ISS: cero *misses*. D-cache 16 KB / 4 vías, I-cache 32 KB, *working set*
de la etapa ~150 KB. Cada trama recorre el estado y los pesos enteros. Estimación de orden de
magnitud: 150 KB / 32 B de línea ≈ 4 700 líneas por trama (32 B es el tamaño que asumen las
operaciones de caché del SDK, `docs/INTEGRACION_AP.md:120`); a 20–40 ciclos por línea son
**94 000–188 000 ciclos/trama (+10–20 %)**, y eso si el *prefetch* ayuda. Si el estado queda en
PSRAM sin localidad, puede ser peor.

**Mitigación concreta y barata:** estado y pesos en **DRAM1** (`0x5FEA0000`, SRAM on-chip; en la
imagen de fábrica hay ~290 KB libres ahí, y en la nuestra está entera libre) y el código de la
etapa en IRAM/DRAM. Requiere que el `crt0` copie esas secciones (la store-table de la fábrica hace
exactamente eso). **Criterio:** ciclos medidos en la radio con el contador del CP (si no hay,
medir por diferencia de carga del M33 no sirve: hay que instrumentar).

**Coste si no se cumple el objetivo:** el criterio de producción (≤10 %) no se alcanza con este
port en ningún caso (ya lo dice `reports/dsp_ns.md:265-273`); el primer escalón que lo cumple es
GRU `int16`, que **no es bit-exacto** y exige medir calidad. Eso es otra fase, no un bloqueo de
ésta.

### 4.5 El bucle sondeado y la latencia del RPC

`main_dsp.c:150-153` hace `srv_poll()` y luego `audio_task()`, una vez por vuelta. Con la etapa
dentro de `audio_task()` (3,1 ms de cálculo por trama), el buzón se atiende **una vez cada
10 ms**: el AP puede esperar hasta 10 ms por una respuesta síncrona (`lsf_set()` es síncrona) y
los *doorbells* de datos se retrasan igual. Con la radio funcionando eso se traduce en
*under-runs* del anillo de reproducción. **Mitigación:** o se sondea el buzón entre trozos de la
trama (procesar el NS en 4 bloques de 120 muestras y sondear entre ellos), o se separa en dos
tareas XOS (audio y RPC) con prioridades. **Coste:** 4–8 h, pero es un fallo que sólo aparece
bajo carga, así que hay que diseñarlo desde el principio.

### 4.6 Doble procesado / regresión de ajustes

Si el AP no se apaga a la vez que el DSP enciende, el usuario oye NS dos veces (artefactos
metálicos) — y el ajuste `settings.noiseSuppression` (`openrtx/src/core/state.c:113-115`) pasa a
significar dos cosas distintas. **Mitigación:** un solo dueño de la etapa, decidido por
configuración de build en la fase (d), y el `k=v` sólo como conmutador de depuración.

### 4.7 Licencia y publicabilidad

* RNNoise es BSD-3; el port y los pesos ya están en el árbol.
* El HAL del vendor: está en un SDK que se distribuye; **conviene revisar los términos antes de
  publicar** el árbol con esos ficheros copiados.
* El *stub* `tenlp.so` que hace funcionar `xt-clang` **no es una licencia**
  (`reports/dsp_ns.md:66-74`). Para el laboratorio vale; para fabricar imágenes de producto hace
  falta la licencia de Cadence. **Coste:** es una compra, no un riesgo técnico.
* XOS 2.10 viene con las herramientas; su licencia es la de Cadence, no la del proyecto.

---

## 5. Esfuerzo

Supuestos: una persona que ya conoce el árbol; las cifras son horas de trabajo, no de calendario.

| Fase | Trabajo | Horas | Se puede validar sin radio |
|---|---|---:|---|
| 0. `crt0` + linker + `make dsp` | asm/C + script | 8–16 | sí (QEMU) |
| a. Ida y vuelta en el emulador | fuente/sumidero de fichero + reloj virtual | 16–24 | sí |
| b. Etapa en el servidor, bit-exacta | enlace + estado + de-intercalado + arnés | 12–20 | sí |
| c1. Pegamento de audio (ADC/APC/DAC/DMA) | init, LLP doble buffer, ganancias, 48 kHz, reparto L/R | 40–60 | no |
| c2. Imagen + flasheo + *bring-up* en radio | arranque real, silencio/ruido, A/B contra imagen A | 16–32 | no |
| d. AP con la etapa remota | apagar la del AP, *fallback*, medidas | 8–16 | parcial |
| **Total** | | **100–168 h** | |

**Qué hay que comprar u obtener: nada.** Toolchain Xtensa 14.07 con el core `venus_hifi4`
(instalado en `~/xtensa`), XOS 2.10 (incluido), QEMU con el core importado
(`work/xtensa-tools/qs`), los drivers del CP (en `extract/hal`), el port y su banco (rama
`dsp/ns`). Lo único que no es «nada» es la licencia FlexNet si esto sale del laboratorio.

**Camino crítico:** fase 0 → (c1) → (c2). Las fases (a), (b) y (d) son paralelizables con (c1)
porque no necesitan la radio.

---

## 6. Reproducción de lo que se afirma aquí

```bash
H=/home/n0p/src/c62/extract/hal/hal_listenai/csk6/drivers/hifi4

# El esqueleto pasa lo que dice pasar
cd ~/src/c62/work/dsp-openfw && make test && make dsp_sim

# La imagen no arranca: entry ≠ 0x60000000, y el byte 0 es un literal
source ~/xtensa/env.sh
$XTENSA_TOOLS_ROOT/bin/xt-nm build/dsp.elf | grep ' _start'
$XTENSA_TOOLS_ROOT/bin/xt-objdump -D -b binary -m xtensa \
    --adjust-vma=0x60000000 build/dsp_firmware.bin | head -6
( sleep 3; echo "info registers"; echo quit ) | \
  $QEMU_HIFI4 -M sim -cpu venus_hifi4 -nodefaults -display none \
              -kernel build/dsp.elf -monitor stdio | grep -E 'PC=|EXCC'

# El HAL del CP compila para el core (0 errores) con el shim de 4 cabeceras
cd ~/.hermes/cache/scratch/halbuild   # toolchain/xcc.h, venus_ap.h, zephyr.h, sys/printk.h
$XTENSA_TOOLS_ROOT/bin/xt-clang --xtensa-core=venus_hifi4 -O2 -ffreestanding -nostdlib \
  -DXOS_USED -DPLATFORM_CP -DCSK6001 -DIC_BOARD \
  -I. -I$H/include/hal -I$H/include/hal/reg -I$H/include \
  -c $H/adc_pdm_venus.c -o adc.o

# La fábrica y la imagen A llevan esos drivers
grep -n 'adc_pdm_venus.c\|apc_venus.c\|dma_venus.c\|ADC_PDM_Receive\|DAC_SEND' \
     work/dsp-factory/str-factory.txt | head
grep -n 'AudioStreamInVenus\|ADC_PDM_Initialize' work/dsp-re/str48k.txt | head

# XOS está instalado
ls $XTENSA_ROOT/venus_hifi4/xtensa-elf/arch/lib/libxos.a \
   $XTENSA_TOOLS_ROOT/xtensa-elf/src/xos $XTENSA_TOOLS_ROOT/doc/xos-2.10.zip
```

---

## 7. Lo que este informe **no** afirma

* Que el reparto ADC01/ADC23 entre micrófono y radio esté confirmado instrucción a instrucción en
  el blob de fábrica (es deducción del código de aplicación y del ruteo de canales).
* Que el HAL de `extract/` sea el que se compiló en la imagen A (no lo es: otra revisión).
* Que el coste real en la radio sea 31,1 %; es el número del ISS, sin memoria.
* Que el criterio de producción (≤10 %) se alcance con este port; no se alcanza, y el escalón que
  lo alcanza (GRU `int16`) rompe la bit-exactitud.
