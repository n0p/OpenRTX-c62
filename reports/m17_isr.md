# El PTT de M17 y el mutex desde una interrupción: reproducción en emulador y grafo de llamadas

**Rama** `m17-isr-repro`, desde `codec/improvements` (`82fc0ed1`, que contiene
`c1273d7b`).  **Imagen analizada** `dist/codec/v011/zephyr.elf`, la que el
usuario tiene flasheada: su sección `text` es idéntica, byte a byte, a la del
`build_c62` de la rama, así que lo que sigue vale para esa imagen.

---

## 1. Qué se ha hecho, y qué se ha encontrado

Tres piezas, en el orden en que se pidieron:

1. **Arnés de reproducción en QEMU** (`tests/qemu/m17_isr.c`, `scripts/m17_isr.sh`).
   Corre sobre el mismo kernel de la radio (Zephyr 3.4 de ListenAI, Cortex-M33,
   `mps2_an521` en QEMU) con la configuración que decide esta cuestión
   (aserción activada, log diferido con su hilo, `printk` enrutado al log,
   pthreads sobre los mutex del kernel).  **Validado por mutación**: un camino
   inyectado que toma un mutex desde una interrupción real aborta con el mensaje
   literal de la radio y el arnés dice **en qué función** ocurre, resolviendo la
   dirección de retorno contra el ELF que QEMU ejecutó.
2. **El camino real del firmware** dentro de ese arnés: `audioPath_request(
   SOURCE_MIC, SINK_MCU, PRIO_TX)`, la primera cosa que hace
   `OpMode_M17::txState()` al pulsar PTT (`openrtx/src/rtx/OpMode_M17.cpp:459`),
   compilada del árbol, llamada desde una interrupción.  **También aborta**, y
   el mutex lo toma `acquire_mutex()` (`lib/posix/pthread_mutex.c:148`) desde
   `pthread_mutex_trylock()` en `pathLock()` — es decir, **el camino de
   transmisión de M17 no es seguro en contexto de interrupción**, y el arnés
   nombra el punto exacto.
3. **El grafo de llamadas de la imagen** (`scripts/isr_mutex_graph.py`): desde
   **cada** entrada de interrupción registrada en `v011` (tabla `sw_isr_table`,
   los handlers que el HAL de CSK6 instala en su tabla de vectores en RAM, y
   los `k_timer`) hasta cualquiera de los cuatro puntos de la imagen que
   contienen la aserción de `mutex.c`.  Resultado: **ninguna entrada de
   interrupción alcanza un mutex** (0 de 123 funciones alcanzables desde una
   interrupción coinciden con las 1021 que alcanzan un lock).

Y el hallazgo que ordena todo lo anterior: **la captura del reinicio que se
aportó no es de `v011`**, y su `pc` no está en el mutex sino en el **procesado
del log**: en `dist/codec/branch/zephyr.elf` (la imagen anterior)
`0x1804a00c` resuelve a `z_impl_log_process()`
(`subsys/logging/log_core.c:529`) y `0x18049fff` a `msg_process()`
(`log_core.c:453`); en `v011` esa misma dirección es `esf_dump()`
(`arch/arm/core/aarch32/fatal.c:29`).  Es decir, lo que se vio en el puerto
serie fue **el camino de pánico muriendo dentro del log al procesar la cola
desde una interrupción** — el bug que arregló `aa49ce7d` (log diferido) — y no
un mutex tomado por el camino de M17.

Las secciones 2 a 5 dan las pruebas, y la 6 dice qué queda por hacer con la
radio en la mano.

---

## 2. El arnés: la regla, reproducida y localizada

    scripts/m17_isr.sh [mutation|m17_path|thread|printk|panic]
    WITH_TIMER=1 scripts/m17_isr.sh      # la interrupción es un k_timer

Compila cinco imágenes Zephyr para `mps2_an521` en el contenedor
`openrtx-c62` (el mismo SDK y el mismo árbol `zephyr-listenai` del build del
firmware), las corre en QEMU y lee el veredicto del puerto serie y de
semihosting.  La interrupción **no es un hilo disfrazado**: se conecta una línea
del NVIC con `IRQ_CONNECT()` y se dispara con `NVIC_SetPendingIRQ()`, de modo
que la entrada pasa por la tabla de vectores, `_isr_wrapper` y `_sw_isr_table`
igual que las interrupciones de SPI y del mailbox del DSP en la radio; con
`WITH_TIMER=1` el mismo trabajo se hace desde un `k_timer`, que es el otro
contexto de interrupción que usa el firmware (`radio_C62.cpp` arma uno para el
corte de PA).

**Cómo se localiza la función**: el kernel llama a `z_impl_k_mutex_lock()`
(`k_mutex_lock()` es un inline), así que el enlace se hace con
`-Wl,--wrap=z_impl_k_mutex_lock`.  El interpositor, si `__get_IPSR() != 0`,
imprime la dirección de retorno del que pidió el lock, y el guion la resuelve
con `arm-none-eabi-addr2line -i` **contra el ELF que QEMU acaba de ejecutar**.
No hay ninguna tabla escrita a mano: la respuesta la da el binario.

### 2.1 Mutación: el fallo inyectado se ve

`-DTEST_NAME=mutation`: la interrupción llama a `injected_path()`, que hace
`k_mutex_lock(&victim, K_NO_WAIT)`.

```
HARNESS boot
HARNESS software interrupt armed
HARNESS in interrupt: injected_path()
HARNESS mutex-in-interrupt
HARNESS trigger_isr
HARNESS caller 0x1000058b
HARNESS mutex 0x38000800
HARNESS ASSERT
HARNESS assert at WEST_TOPDIR/zephyr-listenai/kernel/mutex.c:101
*** Booting Zephyr OS build b8fcc47cc045 ***
ASSERTION FAIL [!arch_is_in_isr()] @ WEST_TOPDIR/zephyr-listenai/kernel/mutex.c:101
	mutexes cannot be used inside ISRs
   caller 0x1000058b = injected_path (/src/tests/qemu/m17_isr.c:186)
   interrupt entry: trigger_isr
   RESULT reproduced: mutex taken in interrupt context
```

Es la misma aserción, del mismo fichero y la misma línea que la captura de la
radio, y el arnés nombra la función (`injected_path`, `m17_isr.c:186`).  Con
`WITH_TIMER=1` sale lo mismo con entrada `timer_expiry`.  **El arnés sirve: ve
el fallo cuando existe.**

### 2.2 El camino real de M17, desde una interrupción

`-DTEST_NAME=m17_path`: la interrupción llama a `m17_audio_request()`, que es
`audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX)` sobre el
`openrtx/src/core/audio_path.cpp` real (sólo la capa de placa de debajo es un
doble, `tests/qemu/m17_isr_board.cpp`, con la misma regla que
`platform/drivers/audio/audio_c62.c`: un mutex y luego el driver).

```
HARNESS in interrupt: m17_audio_request()
HARNESS mutex-in-interrupt
HARNESS trigger_isr
ASSERTION FAIL [!arch_is_in_isr()] @ WEST_TOPDIR/zephyr-listenai/kernel/mutex.c:101
	mutexes cannot be used inside ISRs
   caller 0x100051eb = acquire_mutex (/zephyr-tree/zephyr-listenai/lib/posix/pthread_mutex.c:148)
   interrupt entry: trigger_isr
   RESULT reproduced: mutex taken in interrupt context
```

El mutex lo toma `acquire_mutex()`, en `pthread_mutex.c:148`, llamada desde
`pthread_mutex_trylock()` en `pathLock()` (`audio_path.cpp:153`), llamada desde
`audioPath_request()` (`audio_path.cpp:199`).  Si el PTT de M17 se atendiera en
una interrupción, este es el aborto que produciría, y este es el punto exacto.

### 2.3 El control: el mismo camino desde un hilo

`-DTEST_NAME=thread`: la misma llamada, desde un hilo, que es donde la hace el
firmware (`OpMode_M17::txState()`, en el hilo de RTX).

```
   in thread: the same call
   audioPath_request returned 0x00000001
   the thread finished
   VERDICT ok: no mutex was taken in interrupt context
   RESULT ok: no assertion
```

El camino es legal en un hilo (abre el camino de audio y devuelve `1`) y sólo
aborta en contexto de interrupción: el arnés distingue los dos contextos, que
es lo que hay que exigirle.

### 2.4 Log desde una interrupción, y pánico desde una interrupción

- `printk`: `printk("...")` desde la interrupción **no aborta** (`VERDICT ok`).
  Con `CONFIG_LOG_MODE_DEFERRED` + `CONFIG_LOG_PROCESS_THREAD` el mensaje se
  encola y lo imprime el hilo del log: el arreglo `aa49ce7d` se sostiene en este
  kernel y con esta configuración.
- `panic`: `k_panic()` desde la interrupción produce
  `ZEPHYR FATAL ERROR 4: Kernel panic` + `Fault during interrupt handling` y el
  sistema se para, **sin** una segunda aserción: en este árbol el backend UART
  del log escribe con `uart_poll_out()` (`log_backend_uart.c:char_out()`, con
  `k_sem` sólo en modo asíncrono), así que el camino de pánico dentro de una
  interrupción ya no toma ningún lock.  En la imagen anterior sí lo tomaba, y
  eso es lo que se vio en el puerto serie (sección 5).

---

## 3. El grafo de llamadas de la imagen `v011`

`scripts/isr_mutex_graph.py` responde, sobre el ELF enlazado, la pregunta "¿qué
interrupción puede llegar a un mutex?":

    scripts/isr_mutex_graph.py dist/codec/v011/zephyr.elf --audit \
        --extra-root z_timer_expiration_handler --extra-root dma_irq_handler \
        --extra-root gpadc_irq_handler --extra-root mbx_irq_handler

**Qué cuenta como entrada de interrupción** (nada de esto se deja fuera):

| fuente | cómo se obtiene | entradas en `v011` |
|---|---|---|
| `IRQ_CONNECT` (tabla `sw_isr_table`, 32 huecos) | lectura de la sección | 9: `csk_zephyr_spi_isr`, `csk_pwm_isr`, `csk_gpio_isr`, `adc_csk6_isr`, `csk_mbx_isr`, `wdt_csk6_irq_handler` (×2), `wdt_ap_irq_handler` (×2) |
| handlers que el HAL instala en `OS_CPU_Vector_Table` en tiempo de ejecución (`register_ISR`) | `--extra-root`, resueltos a mano (sección 3.2) | `dma_irq_handler`, `gpadc_irq_handler`, `mbx_irq_handler`, `i2c_irq_handler`, `aon_timer_irq_handler`… |
| `k_timer` (expiry en la ISR del reloj) | objetos `struct k_timer` leídos del DWARF | `paCutExpiry`, `log_process_thread_timer` |

**Resultado**

```
== audit
   functions reachable from an interrupt entry (direct calls): 123
   functions that reach a mutex lock (direct calls):           1021
   functions whose address is taken in the image:              628
   in both the ISR set and the mutex set:                      0
== paths from an interrupt entry to a mutex lock: 0
   none: no interrupt entry reaches k_mutex_lock through direct calls
== interrupt entries found: 11
```

Los cuatro puntos de la imagen que contienen la aserción (los que cuentan como
"aquí se coge un mutex") son `z_impl_k_mutex_lock`, su copia
`k_mutex_lock.constprop.0.isra.0`, `z_impl_k_condvar_wait` y
`pthread_mutex_lock`.  **Ninguno es alcanzable desde una interrupción.**

### 3.1 Las llamadas indirectas: 45 sitios, todos resueltos

Un grafo de llamadas directas no basta: un driver coge el mutex a través de una
tabla de operaciones (`backend->api->process`, `dev->api->transceive`), y ahí
es donde se esconde un camino real.  El guion sigue también esos saltos:
mantiene el valor de los registros dentro de cada función (inmediatos, pool de
literales y la palabra que el global contiene en un desplazamiento dado), lo
que resuelve un nivel de indirección por tabla.  Con eso, los 45 sitios
indirectos que quedan dentro del conjunto alcanzable desde una interrupción son
éstos, y ninguno lleva a un mutex:

| sitio | destino | por qué es seguro |
|---|---|---|
| `csk_zephyr_spi_isr` (`0x18079650`) | `hal_spi->irq_handler` → `csk_spi_isr_callback` | `spi_context_update_tx/rx` + `csk_spi_hal_transceive` → `SPI_Transfer`; sin lock (`spi_csk6.c:280`) |
| `adc_csk6_isr`, `adc_context_on_sampling_done`, `gpadc_irq_handler` (7) | callback de secuencia ADC / `k_sem_give` | `adc_context.h:229`: callback del usuario y rearranque; el `adc_read()` de la radio no instala callback |
| `csk_pwm_isr` → `gpt_irq_handler` (3) | callback del canal GPT | el proyecto no usa PWM salvo el backlight, cuyo callback es del driver |
| `wdt_csk6_irq_handler`, `wdt_ap_irq_handler` (3), `__AON_TIMER_Handler__`, `__AON_WDT_Handler__` | callbacks del watchdog/HAL | ninguno del proyecto |
| `Interrupt21_Handler` (`0x1804e4f0`) | `OS_CPU_Vector_Table[21]` → `mbx_irq_handler` | ver 3.2 |
| `mbx_irq_handler` (2) | `mbox_cb` → `_urpc_recv_notify_client` | encola en un `k_msgq` (`urpc_queue_send_from_isr`); el handler del endpoint sólo se llama en el camino de error |
| `csk_gpio_isr` (`0x1804ceb4`) | handler por pin instalado en tiempo de ejecución | **ningún pin tiene interrupción configurada** en el C62 (no hay `gpio_pin_interrupt_configure` en el árbol); la entrada nunca ocurre |
| `z_timer_expiration_handler` (`0x18058732`) | `timer->expiry_fn` | `paCutExpiry` sólo hace `k_work_submit()` (`radio_C62.cpp:298`); el otro es del log |
| `z_impl_k_timer_stop` (`0x1807eef8`) | `timer->stop_fn` | ninguno registrado (NULL) |
| `z_cbvprintf_impl` (15), `mpsc_pbuf_*` (5), `process`, `submit_to_queue_locked`, `drop_item_locked`, `buffer_write`, `outs` | el formateador de la libc y las notificaciones del `mpsc_pbuf` del log | son punteros de configuración del log/formateo, no de driver; el backend UART escribe con `uart_poll_out()` |

### 3.2 El único salto que el análisis estático no puede seguir, y su respuesta

`csk_mbx_isr` → `Interrupt21_Handler` → `OS_CPU_Vector_Table[21]`.  Esa tabla
está en `.bss` y el HAL la rellena con `register_ISR()`
(`modules/hal/listenai/csk6_cm33/src/venus_ap.c:50`), así que su contenido no
está en la imagen.  Quien la rellena para la línea 21 es el mailbox:
`MBX_Initialize()` → `register_ISR(IRQ_CMN_MAILBOX_VECTOR, mbx_irq_handler)`
(`mbox.c:400`).  Y `mbx_irq_handler` → `mbox_cb()`
(`modules/lib/urpc/src/urpc_mbox_client.c:19`) → `_urpc_recv_notify_client()`,
que **encola** la trama en `k_msgq` y sólo llama al handler del endpoint *si la
cola está llena* (`urpc_client.c:178-183`).  El despacho normal ocurre en
`urpc_task_client`, un hilo.  Es decir: el único sitio donde el firmware podría
ejecutar código arbitrario (el del DSP, LSF/AudioFlinger) en contexto de
interrupción no toma ningún mutex en el camino normal, porque no ejecuta código
del proyecto ahí.

Ese camino de error (cola llena → handler del endpoint dentro de la ISR) queda
como el único punto que un análisis estático no cierra al 100 %: los handlers
de endpoint de LSF se registran en tablas de `.bss`.  En el árbol están
compilados con el shim de FreeRTOS sobre Zephyr, y en ese shim "mutex" es
`xSemaphoreCreateMutex()` → `k_sem` (`freertos_shims/src/csk_os_semaphore.c:39`),
no un mutex — un `k_sem_take` desde una ISR daría la aserción de `sem.c`, no la
de `mutex.c:101`.

---

## 4. Por qué la captura del reinicio no es de esta imagen

La captura dice:

```
ASSERTION FAIL [!arch_is_in_isr()] @ zephyr/kernel/mutex.c:101
        mutexes cannot be used inside ISRs
***** HARD FAULT *****
Faulting instruction address (r15/pc): 0x1804a00c
Show more call stack info: 1804a00c 18049fff 1806410d   -> abort / __assert_func
Fault during interrupt handling
```

Resolviendo esas direcciones contra las dos imágenes que hay en `dist/`:

| dirección | `dist/codec/branch/zephyr.elf` (la anterior) | `dist/codec/v011/zephyr.elf` (la flasheada) |
|---|---|---|
| `0x1804a00c` | `z_impl_log_process()` — `subsys/logging/log_core.c:529` | `esf_dump()` — `arch/arm/core/aarch32/fatal.c:29` |
| `0x18049fff` | `msg_process()` — `log_core.c:453` | — (otra función) |
| `0x1806410d` | `atan2f` | `__ieee754_powf` |

Las tres sólo tienen sentido en la imagen **anterior** (`dist/codec/branch`, el
zip `OpenRTX-c62-codec-improvements-d319099c`): el `pc` está en el **procesado
del log**, no en el mutex.  La lectura que encaja con el código y con el propio
mensaje del commit `aa49ce7d` ("with the console's log control block in the
register dump") es:

1. algo entra en pánico **en una interrupción**;
2. `z_fatal_error()` → `LOG_PANIC()` → `panic_mode = true` →
   `z_log_msg_post_finalize()` procesa la cola **en el contexto del que
   llama** (`log_core.c:159-164`), o sea, dentro de la interrupción;
3. el backend UART de esa imagen toma su lock (un mutex en la versión anterior)
   → salta `!arch_is_in_isr() @ mutex.c:101` → se imprime el mensaje que se ve;
4. el segundo pánico, ya dentro del camino de pánico, es el `HARD FAULT` /
   `Fault during interrupt handling`.

En `v011` el backend UART no toma ningún mutex (escribe con `uart_poll_out()`),
y la variante `panic` del arnés lo confirma: un pánico en una interrupción
imprime el volcado y para, sin una segunda aserción.

En resumen: **la captura es el bug del log, ya arreglado**, no un mutex tomado
por el camino de M17.  La imagen `v011` no tiene ningún camino de interrupción a
un mutex (sección 3).

---

## 5. Estado, y qué queda por hacer

- **Reproducción del reinicio**: no, y no por falta de fidelidad del emulador.
  El arnés reproduce el fallo **inyectado** (mutación) y reproduce el **camino
  real de M17 en contexto de interrupción** (aborta en `acquire_mutex()`); lo
  que no existe en `v011` es una interrupción que llame a ese camino.  Eso es
  lo que demuestra el grafo, y es comprobable ejecutando el guion.
- **Si la radio sigue reiniciando al pulsar PTT en M17**, entonces no es esta
  aserción.  Hace falta una captura **de `v011`** (o del build actual) y, mejor,
  el registro de la caja negra (`tools/cps/blackbox.py`), que dice el hilo y el
  `pc` del fallo; con la captura nueva, `scripts/isr_mutex_graph.py` y
  `scripts/m17_isr.sh` contestan en minutos: el primero dice si el `pc` está en
  un camino alcanzable desde una interrupción, el segundo permite reproducir
  cualquier función concreta de ese camino en contexto de interrupción.
- **Lo que sí se puede afirmar hoy**: el camino de arranque de TX de M17
  (`audioPath_request` → `pathLock` → `pthread_mutex_trylock` →
  `acquire_mutex` → `k_mutex_lock`) **no es seguro en contexto de
  interrupción**; el hilo del códec (`codec_startEncode` → `startThread` →
  `init_mutex`, `audio_codec.c:428`) tampoco; y la regla está ahora comprobada
  en el kernel, no supuesta.

---

## 6. Ficheros

| fichero | qué es |
|---|---|
| `tests/qemu/m17_isr.c` | el arnés: la interrupción, el interpositor de `z_impl_k_mutex_lock`, las cinco variantes |
| `tests/qemu/m17_isr_audio.cpp` | `audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX)` del árbol, para llamarlo desde C |
| `tests/qemu/m17_isr_board.cpp` | la capa de placa (doble de `audio_c62.c`: mutex + rutas) y `bb_mark`/`delayMs` |
| `tests/qemu/m17_isr_app/` | la aplicación Zephyr (`CMakeLists.txt`, `prj.conf` con la configuración del firmware) |
| `scripts/m17_isr.sh` | compila las cinco variantes en el contenedor, las corre en QEMU y resuelve el `caller` con `addr2line` |
| `scripts/isr_mutex_graph.py` | el grafo: entradas de interrupción → mutex, sobre el ELF enlazado |
| `reports/m17_isr.md` | este informe |

Reproducir:

    scripts/m17_isr.sh                    # las cinco variantes, ~3 min
    WITH_TIMER=1 scripts/m17_isr.sh       # la interrupción como k_timer
    scripts/isr_mutex_graph.py dist/codec/v011/zephyr.elf --audit \
        --extra-root z_timer_expiration_handler --extra-root dma_irq_handler \
        --extra-root gpadc_irq_handler --extra-root mbx_irq_handler

La suite del host no cambia: no se ha añadido ninguna fuente al `meson.build`
(que lista cada fichero, no hace glob de `tests/qemu`).  En este árbol y con el
compilador de esta máquina (GCC 16) el `meson` del host no compila ninguna de
sus pruebas por dos motivos ajenos a este cambio — `tests/unit/ui_keys.cpp` usa
`operator[]` sobre un `std::map` const que GCC 16 rechaza, y los objetivos
MD-3x0 necesitan el toolchain de miosix —; la misma revisión
(`codec/ns-wiring`, el mismo código) tiene **114/114** en
`build_linux/meson-logs/testlog.txt`.
