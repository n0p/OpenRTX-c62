# Banco de medidas: la radio contra un SDR

Este documento describe cómo se mide lo que la C62 hace en el aire desde un PC,
con un receptor de referencia al lado (RTL-SDR, Airspy, o el Anritsu MS2090A
que ya se maneja por SCPI), y qué se puede afirmar de cada número.

La pieza que faltaba no era un analizador: era poder **guionizar** la radio
(retunear, portadora, tono, captura, barrido) con una **marca de tiempo común**
y con el mismo reloj para los dos instrumentos. Eso es el espacio de nombres
*bench* del protocolo PC, `core/pc_proto_bench.c`, y la herramienta
`tools/bench/`.

| fichero | qué hay |
|---|---|
| `openrtx/include/core/pc_proto.h` | mensajes `PC_MSG_BENCH_*`, tags `PC_TAG_BENCH_*`, capacidad `PC_CAP_BENCH`, hook `pc_proto_platformBench()` |
| `openrtx/src/core/pc_proto_bench.c` | la secuencia de los comandos, las marcas de tiempo, el informe |
| `platform/targets/c62/pc_proto_bench_c62.c` | lo que toca la radio: sintonía, PTT, tono, desviación, RSSI |
| `tools/bench/benchproto.py` | cliente del protocolo (reutiliza el tramado de `tools/cps`) |
| `tools/bench/sdr.py` | receptores detrás de una interfaz común (rtl, soapy, anritsu, sim) |
| `tools/bench/correlate.py` | las cuentas: portadora, desviación, potencia, máscara, flancos, estabilidad, relojes |
| `tools/bench/cases.py` | los siete casos de medida, con su procedimiento escrito |
| `tools/bench/bench.py` | la orden de línea de comandos, el informe y los artefactos |
| `tools/bench/simradio.py` | la radio simulada, para probar el flujo sin nada enchufado |

Dos opciones de build, **las dos apagadas por defecto**:

```
-DC62_PC_PROTO=ON -DC62_DEBUG_PROTO=ON    el protocolo con el espacio de depuración
-DC62_BENCH_PROTO=ON                      además el espacio de nombres del banco
```

`C62_BENCH_PROTO` **exige** `C62_DEBUG_PROTO`: comparten el testigo de sesión y
el búfer de captura de audio, y una imagen de banco es una imagen de depuración
con un cronómetro. Una build de producción no anuncia la capacidad, responde
`not_allowed` a los siete mensajes y no enlaza ni una línea de ellos.

---

## 1. Qué se puede medir y qué no

Antes del protocolo, la lista de lo que la radio **no** puede dar, porque es la
que decide qué casos existen:

- **No hay I/Q.** La radio no expone muestras complejas en ninguna parte: lo
  que entrega es el **audio demodulado** (el discriminador, 48 kHz, s16, mono)
  y el **RSSI** del BK4819 (REG_67, pasos de 1 dB, filtrado). Por tanto: no hay
  constelación, no hay EVM, no hay ruido de fase, no hay potencia de canal
  adyacente medida por la radio.
- **No puede medir su propia portadora.** No tiene referencia contra la que
  compararla; el error de frecuencia lo mide el SDR. El tag
  `PC_TAG_BENCH_DELTA` existe y vale 0: es el hueco donde una plataforma que
  sepa más podría poner un número, no un número inventado.
- **Su captura es sólo de recepción.** Con la PA en el aire, el mismo búfer no
  tiene nada útil (la fuente es el micrófono). Las medidas de transmisión las
  hace el SDR; la captura de la radio sirve para comparar lo que *recibe*.
- **No puede transmitir el SDR ni el analizador.** Un RTL-SDR no transmite y el
  MS2090A tampoco: por eso el caso de sensibilidad está escrito como está (el
  instrumento mide el nivel, la radio dice sus propios CRC).

Y lo que sí:

| magnitud | quién la mide | instrumento mínimo |
|---|---|---|
| frecuencia de portadora y su error | SDR | I/Q, o traza de espectro |
| desviación FM | SDR | I/Q |
| potencia relativa (y absoluta) | SDR / analizador | I/Q (relativa), traza (dBm) |
| máscara espectral | SDR / analizador | I/Q o traza |
| sensibilidad (BER/CRC contra nivel) | la radio, nivel el instrumento | I/Q o traza |
| tiempo de conmutación TX/RX | SDR | I/Q (ventana con flancos), o zero-span |
| estabilidad de portadora | SDR | I/Q, o trazas repetidas |
| lo que la radio recibe (audio, RSSI) | la radio | — |

---

## 2. El protocolo

Bloque `0x0600-0x060F` de `enum pcProtoMsg`, tags en `0x0100-0x011F`, capacidad
`PC_CAP_BENCH` (bit `0x4000`). Todos los mensajes que mueven la radio, la PA o
entregan audio piden el **testigo de sesión** (`PC_TAG_TOKEN`) que da `HELLO`.

### Mensajes

| id | nombre | petición | respuesta |
|---|---|---|---|
| 0x0601 | `BENCH_ARM` | token | `EPOCH`, `TICK`, `RATE`, `LENGTH` (ms máximos de captura) |
| 0x0602 | `BENCH_TUNE` | `RX_FREQ`, `TX_FREQ`?, `MODE`?, `BENCH_BW`?, token | `RX_FREQ`, `TX_FREQ`, `MODE`, `BENCH_BW`, `BENCH_DELTA`, `TICK` |
| 0x0603 | `BENCH_PTT` | `TX_STATE` (1/0), `BENCH_TONE`?, `BENCH_LEVEL`?, token | `TX_STATE`, `BENCH_TONE`, `BENCH_LEVEL`, `TICK` + evento `EVT_BENCH_EDGE` |
| 0x0604 | `BENCH_CAPTURE` | `BENCH_WINDOW` (ms) + token, o `OFFSET`+`LENGTH` + token | `T0`, `RATE`, `WRITTEN`, `BLOCK`, `BLOCKS`, `RUNNING`, `OVERFLOW`, `DATA` |
| 0x0605 | `BENCH_RSSI` | `COUNT` (1..32) + token; sin `COUNT` lee la serie en curso | `BENCH_RSSI_TS` (n × `u32 tick, i16 dBm`), `WRITTEN` |
| 0x0606 | `BENCH_SWEEP` | `BENCH_F0`, `F1`, `STEP`, `DWELL`?, token; sin tags lee los puntos | `BENCH_F0/F1/STEP/DWELL/POINTS`, `RUNNING`, `BENCH_POINTS_TS` (n × `u32 Hz, i16 dBm`) |
| 0x0607 | `BENCH_REPORT` | `OFFSET`? | `TEXT` (trozo), `LENGTH` (total) |

Evento `0x0086 EVT_BENCH_EDGE`: `TX_STATE` (1 pulsado, 0 soltado) + `BENCH_TICK`.
Va **detrás** de la respuesta de `BENCH_PTT`, con la misma marca: el PC sella su
llegada y esa es la medida de latencia más fina que el cable puede dar.

El `TX_STATE` de la respuesta de `BENCH_PTT` es el estado que el banco acaba de
fijar (2 transmitiendo, 1 recibiendo), no el que la PA tiene en ese
milisegundo: el hilo rtx tarda hasta un periodo (25 ms) en pulsar, y el momento
en que la portadora aparece lo mide el SDR.

### Semántica que importa

- **`BENCH_TUNE` es asíncrono.** Escribe `state.channel` y levanta
  `ui_rtxSyncRequest`, la misma puerta que usa el scanner: el hilo rtx (bucle de
  40 Hz) lo convierte en `rtxStatus` y el driver del modo programa el chip. La
  respuesta lleva la marca del momento en que se aceptó el comando, no la del
  cambio en el aire; el cambio llega hasta 25 ms después. El barrido no adivina:
  espera a que el estado aplicado (`POLL`) coincida con la frecuencia pedida.
- **`BENCH_BW` en el cable es `enum bandwidth + 1`** (1 = 12,5 kHz, 2 = 25 kHz,
  3 = 6,25 kHz) para que 0 pueda significar "la que tenga".
- **`BENCH_PTT` pulsa el mismo botón que el operador**: `platform_pttOverride()`
  hace que `platform_getPttStatus()` responda "pulsado", y el camino de
  transmisión del modo enciende la PA con su audio, su guardián y su salida.
  Nada llama a `radio_enableTx()` por detrás. Una vez arriba la PA, en el
  siguiente *tick*, se silencia el micrófono (REG_7D a 0, para que una portadora
  sin modular esté de verdad sin modular) y, si se pidió, se escala la
  desviación escribiendo REG_40 sobre el valor calibrado que dejó el modo.
- **El tono es el generador CTCSS del chip**, por los campos normales del canal:
  llega al modulador como llegaría un subtono de operador. Su rango es la tabla
  estándar 67,0-254,1 Hz; un tono que la tabla no tiene **se rechaza** (vuelve 0)
  en vez de redondearse a otra cosa.
- **`BENCH_LEVEL` es un porcentaje de la desviación calibrada** (100 = la que el
  modo programó), porque el valor absoluto en Hz depende de la calibración de
  fábrica de cada unidad. Lo que dice el número de desviación es el SDR, no la
  radio.
- **`BENCH_CAPTURE` y `BENCH_SWEEP` son excluyentes**, y la captura lo es con la
  del espacio de depuración (`DBG_CAP_*`): un búfer, un usuario. Retunear en
  medio de una captura o de un barrido responde `busy` en vez de invalidar en
  silencio lo recogido.
- **El informe** (`BENCH_REPORT`) es texto generado en la radio, determinista,
  troceado en bloques de 300 bytes: dice lo que el run hizo y lo que **no**
  sabe, incluida la línea `note no carrier reference...`.

### Ejemplo de sesión (cliente Python)

```
$ tools/bench/bench.py --device /dev/ttyUSB0 --sdr rtl run carrier stability
bench: radio c62, firmware OpenRTX c62 | RTL-SDR (uncalibrated: relative levels only) | 145.500000 MHz
  carrier      ok
  stability    ok
...
  error_hz               112.4
  power_dbfs             -18.7
  occupied_bw_hz         2 918.0
...
```

---

## 3. La marca de tiempo común

Aquí no hay GPS ni RTC fiable: el reloj de la radio son milisegundos desde el
arranque (`k_uptime_get()`), monótonos pero con su cristal. El mecanismo es
explícito y se apoya en tres cosas:

1. **La radio sella**. Cada respuesta y cada flanco TX/RX lleva
   `PC_TAG_BENCH_TICK`. La captura lleva además `T0` (tick de la muestra 0) y su
   índice de muestra a 48 kHz, así que la muestra *i* está en
   `T0 + i·1000/48000` ms de reloj de radio, sin ambigüedad.
2. **El PC sella la llegada**. Cada respuesta se fecha con
   `time.monotonic_ns()` y se guarda el viaje de ida y vuelta. Un ajuste por
   mínimos cuadrados de los pares `(tick radio, hora PC)`, ponderado por
   `1/rtt`, da el **desplazamiento** entre los dos relojes y su **deriva en
   ppm** (`correlate.fit_clock()`), más la dispersión del ajuste, que es lo que
   dice cuánto vale el resultado.
3. **El flanco de PTT es el cero de eventos**. Es lo único que ven los dos
   instrumentos a la vez: la PA sube en el aire (lo ve el SDR) y la radio sabe
   cuándo se lo pidieron y cuándo subió. La diferencia entre el comando y el
   flanco medido *es* la latencia de subida de la cadena: hilo rtx (25 ms de
   periodo), ruta de audio y PA. Se mide, no se supone.

### Precisión que se puede esperar

| qué | cuánto | de dónde sale |
|---|---|---|
| resolución del tick de radio | 10 ms | `pc_proto_tick()` corre a 10 Hz |
| resolución dentro de una captura | 20,8 µs | índice de muestra a 48 kHz |
| sellado de una respuesta en el PC | 1-3 ms típico | lectura bloqueante + planificador |
| ajuste de relojes (10+ pares) | dispersión típica < 1 ms; se cita **10 ms** por el tick | `fit_clock()` |
| alineación radio ↔ SDR | **±10 ms** (peor caso ±25 ms si el retune manda) | tick + periodo del hilo rtx |
| latencia de subida de la PA | ±6 ms | ventana de 0,5 ms en la envolvente del SDR |
| error de portadora | 0,1-1 Hz | FFT + interpolación parabólica, 0,5 s a 240 kHz |
| desviación | ±3 % | percentil 99 de la frecuencia instantánea |
| tono medido | ±1 Hz | FFT de 0,5 s a 48 kHz |

Lo que **no** se puede afirmar: que la radio y el SDR estén alineados mejor que
el periodo del tick. Cualquier figura por debajo de 10 ms en la comparación
entre los dos instrumentos es ruido del protocolo, no de la radio. Los números
que sí son exactos son los de dentro de una captura, porque no dependen del
reloj: nada.

Si el ajuste no tiene al menos dos respuestas, el informe lo dice
(`pairs 0`, `no clock fit`) en vez de inventar un desplazamiento.

---

## 4. Los casos de medida

Cada caso es una función de `tools/bench/cases.py` con su procedimiento escrito
allí y aquí. El runner rechaza un caso cuyo instrumento no pueda con él, en vez
de producir un informe lleno de huecos.

### 4.1 Portadora: programada contra real

- **Procedimiento**: `arm`, `tune(f)`, `ptt(on, tone=0)`, capturar 0,5 s,
  medir el pico. La diferencia con `f` es el error de la radio: referencia del
  sintetizador, paso y calibración, todo en un número.
- **En el SDR**: una línea estrecha. Positivo = la radio transmite por encima.
- **Se espera**: un error comparable al paso de sintetizado; un valor menor que
  medio paso de canal es indistinguible del propio paso.
- **Instrumento**: I/Q (mejor) o traza. Con el analizador el nivel es absoluto
  (dBm), pero su referencia de frecuencia pone el suelo del error (0,1 ppm =
  15 Hz a 145 MHz).

### 4.2 Desviación (FM)

- **Procedimiento**: `ptt(on, tone=88, level=n)` para n en 30/65/100; capturar
  0,5 s; desmodular y medir la frecuencia instantánea (percentil 99 de la
  componente alterna, con la continua aparte como `dc_hz`).
- **En el SDR**: una portadora que oscila simétricamente ±desviación, y el tono
  medido tiene que ser el pedido (la comprobación de que modula lo que se le
  pidió).
- **Se espera**: 2,5 kHz de pico para 12,5 kHz de canal, 5 kHz para 25 kHz.
  `dc_hz` debe coincidir con el error del caso 4.1; si no, el transmisor se
  mueve al modular.
- **Instrumento**: sólo I/Q. Con traza o zero-span el caso no corre.

### 4.3 Potencia relativa

- **Procedimiento**: medir el canal (12,5 kHz) con la PA arriba y con la PA
  abajo; con I/Q, además, la potencia total en dBFS.
- **En el SDR**: un escalón. Con RTL-SDR es **relativa** (no hay calibración);
  con el analizador es dBm en su puerto, que siguen sin ser vatios en la antena
  hasta que se apunte la pérdida del cable.
- **Se espera**: 40 dB o más entre transmitir y no transmitir; un valor menor
  significa que lo que se está midiendo no es la PA.

### 4.4 Máscara espectral

- **Procedimiento**: `ptt` con el modulador del modo (aquí el tono), capturar
  0,5 s con 480 kHz de ancho y medir la traza contra una máscara **relativa**
  (`MASK_12K5`: 0 dB en el centro, −60 dB a ±12,5 kHz, −70 a ±25, −80 a ±50).
- **En el SDR**: la energía dentro del canal y el faldón cayendo con el offset.
- **Se espera**: margen positivo en toda la máscara. Ojo: la máscara de este
  documento es de banco; pasar no dice que la emisión sea legal, dice que el
  transmisor está sano.

### 4.5 Sensibilidad (CRC contra nivel)

- **Procedimiento**: sintonizar, poner una portadora modulada en la frecuencia
  (otro transmisor, o la propia radio con carga fantasma y atenuador), bajar el
  nivel a pasos y leer los contadores de recepción de la radio en cada paso. El
  instrumento mide el **nivel**; la radio mide su **decodificación**.
- **En el SDR**: nada más que un nivel. La tasa de error es de la radio y es la
  única fuente honesta: este banco no demodula la PHY del modo en el PC.
- **Se espera**: una curva de escalón (con un modo digital, los CRC empiezan a
  fallar en algún punto concreto). Sin contadores en la build
  (`CONFIG_C62_MODE_STATS`), el caso lo dice y sólo queda el nivel.

### 4.6 Tiempo de conmutación TX/RX

- **Procedimiento**: arrancar una captura de 1,2 s, pulsar PTT a los 0,3 s,
  soltar a los 0,75 s, medir dónde sube y baja la envolvente.
- **En el SDR**: un escalón de subida y otro de bajada con la silenciosa antes y
  después, que es lo que hace medibles los dos flancos en una sola toma.
- **Se espera**: 0-30 ms de latencia de subida (manda el periodo de 40 Hz del
  hilo rtx) y una bajada por el camino de salida del guardián, no simétrica.
- **Instrumento**: I/Q. Con zero-span no se puede pulsar dentro de la ventana
  (el barrido del analizador necesita un trigger que esta herramienta no
  configura), así que mide el escalón de nivel y **dice que no mide la
  latencia**.

### 4.7 Estabilidad de portadora

- **Procedimiento**: mantener la PA 3 s y medir la frecuencia por bloques de
  100 ms (`frequency_series`), con la media, la desviación y la deriva.
- **En el SDR**: una línea que se mueve decenas de hercios en el primer segundo
  y luego se asienta. En ppm: 2 ppm es un cristal normal, 20 ppm uno barato.
- **Se espera**: un `std_hz` bajo y una deriva que se pueda atribuir al
  calentamiento de la PA. El primer segundo es calentamiento y se informa
  aparte.

### 4.8 Alineación de las dos capturas (`align`)

No es un caso de medida de la radio sino del **banco**: graba a la vez la
captura de la radio (discriminador, 48 kHz) y la del receptor, demodula la del
receptor, y las correlaciona. Devuelve el retardo entre las dos, la calidad del
parecido (correlación normalizada 0..1: por debajo de 0,2 no son la misma
señal) y el tono medido en cada una. Con eso se comprueba que la marca de tiempo
común sirve para algo: si las dos capturas no se solapan como dice el reloj, el
`lag` y el ajuste no cuadran y el informe lo enseña.

---

## 5. La herramienta

```
tools/bench/bench.py [--device /dev/ttyUSB0] [--sdr rtl|soapy|anritsu|sim]
                     [--freq 145.5M] [--tone 88] [--level 0] [--out DIR]
                     run [caso ... | all]
tools/bench/bench.py --sim run all          # nada enchufado
tools/bench/bench.py --sim cases            # qué casos hay
tools/bench/bench.py --device ... info      # qué sabe hacer la radio del cable
```

Cada ejecución escribe en `--out` (por defecto `tools/bench/runs/<fecha>`):

```
report.txt        lo que se ha leído en pantalla
report.json       lo mismo, para un script
captures/*.npy    cada captura, para rehacer una figura sin la radio
```

y guarda la captura de la radio (`align_radio.npy`) con su `t0`, su tasa y la
forma de sus bloques (`BLOCK`/`BLOCKS`: si los bloques no son todos iguales, la
captura no es contigua y el número `block` vale 0 para decirlo).

### Receptores

| backend | qué da | notas |
|---|---|---|
| `rtl` | I/Q hasta 2,4 MS/s | `rtl_sdr` por línea de órdenes; 8 bits, **sin calibración**: niveles relativos |
| `soapy` | I/Q | cualquier equipo que SoapySDR conozca, si están sus bindings de Python |
| `anritsu` | traza (dBm) y zero-span | MS2090A por SCPI en `10.205.52.71:9001`; **no hay I/Q** en nuestra unidad (opción 0124 no comprada) |
| `sim` | los tres | banco sintético: el flujo entero sin hardware |

El analizador se maneja con las trampas ya conocidas: las trazas vuelven como
bloque IEEE `#A<lon>…` sin salto de línea (un `readline()` muere por timeout),
`:CALC:MARK:Y?` devuelve `nan` (se usa `:FETCh:PEAK?`), y una traza en MAX-hold
de otra sesión se lee como un fantasma congelado (se fuerza `:TRAC1:TYPE NORM`
al conectar).

### Modo simulado

`--sim` levanta una radio simulada (`simradio.py`) que habla el mismo protocolo
—mismos mensajes, mismos TLV, codificados y decodificados con `tools/cps`— y un
receptor sintético (`sdr.Sim`) cuyo aire está **guiado por lo que la radio
hace**: si el caso pulsa con tono 88 y nivel 50, en el aire hay una portadora
modulada a 88 Hz con la mitad de la desviación del modelo. Así los números que
se leen de vuelta son los que el modelo puso, y los tests pueden exigirlos.
El informe lleva `simulated yes` y todas las notas lo repiten: **es un modelo,
no una medida**.

### Tests

```
tests/host/pc_proto_host/run.sh          # construye los tres "radios" y corre todo
  tools/bench/tests/test_bench_proto.py  # 10 casos: el protocolo por un pty, contra el C real
  tools/bench/tests/test_correlate.py    # 20 casos: señales sintéticas con el daño conocido
  tools/bench/tests/test_bench_e2e.py    #  7 casos: el CLI entero, simulado, incluido el
                                         #            camino sin I/Q (analizador)
./b/pc_bench_test                        # 11 casos: el módulo C del banco en el host
./b/pc_bench_nobench_test                #  1 caso: una build sin banco no mueve la radio
```

Lo que cubren: `ARM` con su época, el testigo obligatorio, `TUNE` con lo que
aceptó, `PTT` con el flanco y su evento, la captura llenándose por ticks con su
`t0` y su forma de bloques, la serie de RSSI sellada, el barrido recorriendo la
banda y parando solo, la exclusión captura/retune, el informe troceado, y que
una build de producción contesta `not_allowed` a los siete sin filtrar un byte.
En el lado de las cuentas: portadora, desviación, potencia, anchos, flancos,
estabilidad, ajuste de relojes y correlación cruzada, con tolerancias que son
las que el documento promete.

---

## 6. Ejemplo de informe

```
==============================================================================
                                 BENCH REPORT
==============================================================================
  radio          radio c62, firmware OpenRTX c62
  receiver       RTL-SDR (uncalibrated: relative levels only)
  frequency      145.500000 MHz
  tone           88 Hz
  simulated      no

------------------------------------------------------------------------------
CARRIER  Carrier frequency against the programmed one
------------------------------------------------------------------------------
 procedure
   . tune 145.500000 MHz -> radio says 145.500000 MHz
   . key with no modulation, radio tick 12847
   . unkey
 numbers
  error_hz               112.4
  power_dbfs             -18.7
  occupied_bw_hz         2918.0
  source                 iq
 reading
   * the I/Q centre is the frequency the radio was told: the offset is the
     radio's error, positive means high
   * compare against the channel raster: an error under half a step is
     indistinguishable from the tuning step itself
 artifacts
   carrier_iq.npy

------------------------------------------------------------------------------
DEVIATION  FM deviation against the level setting
------------------------------------------------------------------------------
 procedure
   . level 30: tone 88 Hz, deviation 742.0
   . level 65: tone 88 Hz, deviation 1381.5
   . level 100: tone 88 Hz, deviation 2094.0
 numbers
   level=30  tone_hz_applied=88  p99_hz=742.0  peak_hz=791.0  dc_hz=112.0  tone_measured_hz=88.0
   level=65  tone_hz_applied=88  p99_hz=1381.5  peak_hz=1420.0  dc_hz=112.0  tone_measured_hz=88.0
   level=100  tone_hz_applied=88  p99_hz=2094.0  peak_hz=2130.0  dc_hz=112.0  tone_measured_hz=88.0
  hz_per_level_step      20.76
 reading
   * the measured tone frequency is the cross-check that the modulation is
     the one that was asked for
   * dc_hz is the carrier offset as the discriminator sees it: it should match
     the carrier case
   * 12.5 kHz channels want 2.5 kHz peak deviation, 25 kHz want 5 kHz

------------------------------------------------------------------------------
SWITCHING  Transmit and receive switching times
------------------------------------------------------------------------------
 procedure
   . keyed at 0.303 s and unkeyed at 0.752 s of a 1.2 s capture
 numbers
  key_command_s          0.303
  unkey_command_s        0.752
  carrier_on_s           0.317
  carrier_off_s          0.766
  key_up_latency_ms      14.0
  key_down_latency_ms    14.0
  floor_db               -68.2
  burst_s                0.449
 reading
   * the latency is dominated by the rtx thread's 40 Hz period
   * the two latencies are not symmetric

------------------------------------------------------------------------------
STABILITY  Carrier stability over a keyed period
------------------------------------------------------------------------------
 numbers
  mean_hz                109.8
  std_hz                 6.4
  drift_hz_s             -1.2
  span_s                 2.9
  ppm_mean               0.75
  ppm_std                0.04
  ppm_per_s              -0.01
  blocks                 30

------------------------------------------------------------------------------
ALIGN  Radio capture against the receiver's, on one clock
------------------------------------------------------------------------------
 numbers
  lag_s                  0.214
  lag_samples            10272
  match                  0.71
  radio_tone_hz          999.9
  sdr_tone_hz            1000.0
  radio_t0_ms            13120
  radio_rate             48000
  radio_blocks           29
  radio_block_len        480

------------------------------------------------------------------------------
TIME BASE
------------------------------------------------------------------------------
  pairs                  38
  radio_booted_s_ago     18342.6
  ppm                    -3.41
  scatter_ms             0.62
  alignment_ms           10.0
  tick source            k_uptime_get(), ms since boot
  resolution             10 ms (protocol tick) / 20.8 us (48 kHz sample)
==============================================================================
```

Nota: `lag_s` de 0,214 s en `ALIGN` es correcto y esperado — la ventana de la
radio se armó antes de que el receptor empezara a capturar, y ese retardo es
justo lo que mide la correlación.

---

## 7. Coste

Medido con `c62-build` sobre esta rama, `CONFIG_C62_DMR_RX=n` y cambiando sólo
las opciones del protocolo:

| build | FLASH | RAM |
|---|---|---|
| `C62_PC_PROTO=ON` (sin depuración ni banco) | 502 392 B | 156 888 B |
| `C62_PC_PROTO=ON C62_DEBUG_PROTO=ON C62_BENCH_PROTO=ON` | 511 072 B | 158 848 B |

Es decir **+8 680 B de flash y +1 960 B de RAM** por las dos capas juntas, de
los que el banco son unos 1,6 kB de RAM: el estado del run (32 muestras de
RSSI, 96 puntos de barrido de 6 B, el búfer del informe de 768 B) y el código
de los siete handlers. En una build sin `C62_BENCH_PROTO` no se enlaza nada de
esto: los handlers quedan en stubs que devuelven `not_allowed`.

---

## 8. Añadir un caso nuevo

1. Escribir la función en `tools/bench/cases.py` con su docstring de
   procedimiento y **qué se espera ver en el SDR**.
2. Declarar qué capturas necesita (`needs_*`): el runner se niega a correrlo
   con un instrumento que no las dé.
3. Registrarlo en `CASES`.
4. Si necesita un mensaje nuevo, añadirlo al protocolo con el procedimiento de
   `docs/PC_PROTO.md` §7 (id del bloque libre, fila en `pc_table[]`, handler).
5. Añadir su caso a `tools/bench/tests/test_bench_e2e.py` con lo que el modelo
   debe devolver, y su tolerancia a este documento.

El criterio para que un caso entre: **el número tiene que poder desmentirse**.
Si no hay una forma de que salga mal y se note, no es una medida.
