# Analizador de espectro: barrido del receptor, rejilla y pantalla

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre el analizador que dibuja el espectro de la banda alrededor de la
frecuencia del VFO: el motor puro (`core/spectrum.*`), el servicio que lo lleva
al receptor (`core/spectrum_service.*`), el medio hardware del driver
(`radio_spectrum*()` en `platform/drivers/baseband/radio_C62.cpp`), la pantalla
(`ui/default/ui_spectrum.c`) y el gancho de texto para el protocolo de PC.

---

## 1. Qué hace

Una pantalla propia (`state.ui_screen == SPECTRUM`) que barre el receptor en
torno a la frecuencia del canal y dibuja el nivel de cada punto como una barra.
Se abre desde el menú rápido de FM y de airband (fila `ESPECTRO`) y desde la
propia pantalla:

| tecla | qué hace |
|---|---|
| P1 (F1) | siguiente span de la tabla, cíclico |
| P2 (F2) | menú rápido del analizador; pulsación larga lo enclava |
| ENTER | vuelve a centrar el span en la frecuencia del VFO |
| UP / DOWN | desplaza el centro ±1 paso |
| ESC | sale y devuelve el receptor |

La cabecera lleva `ESPECTRO` y el span en uso (o `PAUSA`, ver §7); el pie, el
centro con el span a la izquierda y el pico con su frecuencia a la derecha. El
resto es la traza.

Se niega a arrancar, dejando la pantalla donde estaba y con un pitido, si el
receptor no está recibiendo (apagado o transmitiendo) o si el modo es digital:
un modo digital toma el squelch del demodulador y necesita el discriminador
continuo, así que barrer por debajo lo rompería mientras la pantalla esté
abierta. El analizador es un instrumento de modos analógicos (FM, airband).

## 2. El receptor: por qué un barrido

El BK4819 no tiene salida I/Q. Lo que se le puede pedir es el RSSI de la
frecuencia a la que está sintonizado, en pasos de 0,5 dB (`REG_67`,
`dBm = valor/2 - 160`, de -160 a -32,5 dBm) y el audio del demodulador. Un
espectro es, por tanto, un **barrido**: sintonizar, dejar asentarse al receptor y
leer el RSSI, un punto cada vez. La resolución la pone el filtro de FI.

**El chip no sabe barrer.** Lo que el BK4819 llama «scan» es otra cosa, y
conviene dejarlo escrito porque es el primer sitio donde uno busca una solución
que no existe:

| función del chip | registros | qué hace | sirve para un espectro |
|---|---|---|---|
| Frequency Scan (`RF_FreqScan()`) | `REG_32<0>` arranca, `<15:14>` elige 0,2/0,4/0,8/1,6 s; resultado en `REG_0D<10:0>`+`REG_0E` y el indicador de ocupado en `REG_0D<15>` | **contador de frecuencia**: mide la frecuencia de *una* portadora en LNAIN (pide más de -40 dBm) y la deja en un registro | no: mide una portadora cada 0,2-1,6 s, no un espectro |
| Squelch por umbral | `REG_78<>` umbrales, `REG_4F`/`REG_4D`/`REG_4E` ruido/glitch, resultado en `REG_0C<1>`, interrupción en `REG_02<3>/<2>` con `REG_3F<3>/<2>` | compara el nivel de la frecuencia sintonizada con dos umbrales y avisa por interrupción o por un GPIO (`REG_34`/`REG_35` tipo 2 = Squelch) | sólo para «parar en la portadora», no para medir dónde está |
| CTCSS/CDCSS Scan | `REG_68`, `REG_69` | busca el tono o el código de la señal sintonizada | no |
| AFC | `REG_73<4>` (deshabilitar), `<13:11>` rango | corrige la frecuencia del PLL para centrar una portadora | **estorba**: mueve el bin bajo medición |

Fuentes: *BK4819 V3 Application Note* (2021-04-28), «Frequency Scan», «Squelch,
RSSI, Ex-Noise, Glitch» y tabla de interrupciones; *BK4819V3 Registers List*
(2020-12-18), `REG_32`, `REG_0D`, `REG_0E`, `REG_02`, `REG_3F`, `REG_67`,
`REG_63`, `REG_73`, `REG_7E`. El datasheet V1.0 §1.2.3 añade el único número de
tiempo que el chip documenta: «On power up or channel reselection, the
synthesizer takes less than 0.3 msec to settle».

Consecuencias que el código respeta:

- **Afina por palabra de 10 Hz** (`REG_39` y luego `REG_38`), así que cualquier
  frecuencia de la rejilla es alcanzable. Un paso de 25 kHz mueve el campo bajo
  de la palabra y casi nunca el alto (`REG_39` cambia cada 655,36 kHz), y de ahí
  que el barrido escriba el alto **sólo cuando cambia** (§3).
- **Dos bandas, no una:** 18–660 MHz y 840–1300 MHz. Un span que cruzase el
  hueco gastaría la mitad de sus bins en frecuencias que el VCO no alcanza y las
  leería como ruido de fondo: `clampToBand()` recorta el span a la banda del
  centro y mete el centro dentro de ella.
- **La LNA se conmuta en 174 MHz.** Un barrido que cruza esa frecuencia cambia
  `LNA_VHF_EN`/`LNA_UHF_EN` al pasar; sólo cuando cruza, así que un span dentro
  de una banda no paga nada.
- **Dos perfiles de chip.** Para barrer: modo de canal de 25 kHz con
  `REG_43 = 0x7e28` (filtro de FI de 4,5 kHz, duplicado a 9 kHz por el modo,
  §4), AFC apagada (`REG_73<4> = 1`; el AGC fijo es opcional, §3.2d) y la salida
  de AF muteada. Para escuchar: el `REG_43`, el `REG_73`, el AGC y la salida de
  AF que el modo tenía, guardados al entrar (`radio_spectrumEnter()`), y la
  frecuencia configurada. Cambiar de uno a otro
  es `radio_spectrumSweep(true/false)`.

Sólo la C62 implementa estas llamadas (`radio_spectrumEnter/Point/Park/Leave`).
En el resto de builds, incluido el de escritorio y el de los tests, el servicio
responde con una banda sintética de tres portadoras a desplazamientos fijos del
centro (con la falda de 15 dB/bin de un filtro de FI): es lo que permite
ejercitar pantalla, rejilla y escalado sin radio.

## 3. Método y coste medido

### 3.1 De qué se compone un punto

```
t_punto = t_bus(escribir el PLL) + t_asentamiento + t_bus(leer el RSSI)
```

Los tres términos se atacan por separado, y la medida de cada variante está en
`tests/platform/c62_spectrum_cost.c`: metal desnudo sobre QEMU (`mps2-an505`,
Cortex-M33), instrucciones contadas con el plugin de TCG
(`work/dsp-probe/common/insnframe.c`) y tiempo real modelado a partir del
driver de bus y del chip. El banco modela el SPI bit-bang de la C62 con la
división del propio driver de Zephyr
(`wait_us = (1000000·2000/f)/2000/2`, que a 200 kHz trunca a 2 µs por semiperiodo
y da 4 µs por bit, 96 µs por acceso de tres bytes), y modela el BK4819 con su
tiempo de asentamiento como parámetro, de modo que puede decir no sólo cuánto
tarda cada variante sino cuál es *correcta*: la traza de cada variante se compara
bin a bin con una de referencia tomada con 2 ms de asentamiento, y la columna
`err` es el peor error en dB.

### 3.2 Las tres palancas

**a) El bus.** El chip admite 8 MHz en SCK (datasheet V1.0 §2.6, tabla 6:
`fSCK` máximo 8 MHz, `tHIGH`/`tLOW` mínimos 25 ns) y la radio le habla a
200 kHz, porque un registro no está en el bucle interior de nadie. Un punto son
tres accesos de 24 bits: **288 µs de bus**, más que el asentamiento que ese bus
pretende cubrir. El analizador usa una segunda configuración del mismo bus
(`spiFast`, `c62_bk4819_fastSpi` en `platform/targets/c62/hwconfig.c`) a
`CONFIG_C62_SPECTRUM_SPI_HZ` (1 MHz por defecto: por encima de 1 MHz la división
del driver trunca a cero y el ritmo lo pone el GPIO, unos 2 MHz, cuatro veces por
debajo del límite del chip). El resto de la radio sigue a 200 kHz: si una placa
no tolerase el reloj rápido, se pierde el analizador y nada más.

**b) La mitad alta de la palabra del PLL.** `bk4819_tuneCached()` escribe
`REG_39` sólo cuando el campo alto cambia y `REG_38` siempre (es la escritura que
el chip aplica). A 25 kHz de paso eso son dos accesos por punto en lugar de tres
en 25 de cada 26 bins.

**c) El asentamiento.** Aquí está el 58 % del coste original, y el datasheet sólo
da el peor caso (0,3 ms «on power up or channel reselection»). Un barrido no es
una reselección de canal: el sintetizador sigue enganchado y sólo se mueve el
divisor fraccional. En vez de creerle a un número, el driver **le pregunta al
chip**: `radio_spectrumPoint()` sondea `REG_63` (indicador de *glitch*, que se
satura mientras la FI se recupera del retune) cada 20 µs y lee el RSSI en cuanto
baja de 255, con un tope de `CONFIG_C62_SPECTRUM_GATE_POLLS` sondeos (400 µs, el
peor caso del datasheet) para que un registro que no se comporte así no pueda
parar el barrido; si se agota tres puntos seguidos, la puerta se apaga y el
barrido espera **400 µs** —el peor caso del datasheet, lo que hacía antes—: una
puerta que no funciona tiene que costar velocidad, nunca exactitud. Un valor fijo
(`CONFIG_C62_SPECTRUM_SETTLE_US`) desactiva la puerta y duerme; la fila `ESPERA`
del menú rápido recorre `AUTO/50/100/200/400 µs` para comparar las dos cosas en
la propia radio.

**d) El perfil.** El AFC se apaga (`REG_73<4>`) porque arrastraría el PLL a la
portadora más próxima y movería el bin bajo medición; eso vale para cualquier
barrido y va siempre. El AGC se puede fijar (`REG_7E<15>`) pero **está apagado
por defecto** (`CONFIG_C62_SPECTRUM_AGC_FIX=n`): el bucle automático no puede
seguir un bin cada 50 µs, así que la traza ya es una medida a ganancia
constante —la que el receptor tuviera al empezar el barrido—, pero esa ganancia
cambia mientras el receptor está aparcado entre barridos y la traza parpadea
entre pasadas. Fijarla lo quita y es lo que hace egzumer (`BK4819_SetAGC()`,
`REG_7E<15>` con `<14:12> = 011`, el máximo), pero `<14:12>` es un índice fijo,
no la ganancia viva: fijarlo mueve el nivel absoluto de la traza y una portadora
local fuerte podría recortarla. Eso sólo lo puede decir la radio, de ahí que sea
una opción y no el comportamiento por defecto.

### 3.3 Números medidos

Mismo span por defecto que antes (1 MHz / 25 kHz = 41 bins), mismo banco.
`wait` es el tiempo que el punto pasa esperando (bus + asentamiento), `cpu` el
trabajo del bus rápido (una estimación: 90 ciclos por bit de GPIO y 450 por
transferencia, porque el driver de Zephyr no está en esta imagen), `err` el peor
error de la traza contra la referencia de 2 ms. El chip del modelo asienta en
150 µs (filtro de FI de 9 kHz, ver §3.5) y en 300 µs (el peor caso del
datasheet):

| variante | wait/pto | cpu/pto | **µs/pto** | µs/barrido | pto/s | err 150 µs | err 300 µs |
|---|---|---|---|---|---|---|---|
| V0 base: 200 kHz, 400 µs de sleep | 694 | 0 | **694** | 28 454 | 1 440 | 0 | 0 |
| V1 bus rápido, 400 µs | 406 | 3 | **409** | 16 771 | 2 444 | 0 | 0 |
| V2 + mitad alta cacheada | 404 | 2 | **406** | 16 653 | 2 462 | 0 | 0 |
| V3 + 200 µs | 204 | 2 | **206** | 8 453 | 4 850 | 0 | **22 dB** |
| V4 + 100 µs | 104 | 2 | **106** | 4 353 | 9 418 | **22 dB** | 22 dB |
| V5 + 50 µs | 54 | 2 | **56** | 2 303 | 17 802 | 22 dB | 22 dB |
| V6 sin espera | 4 | 2 | **6** | 253 | 162 055 | 22 dB | 22 dB |
| V7 puerta, pasos de 10 µs | 162 | 16 | **178** | 7 317 | 5 603 | 0 | 0 |
| V8 puerta, pasos de 20 µs — **la que va compilada** | 160 | 10 | **170** | 6 984 | 5 870 | 0 | 0 |

Lo que dice la tabla, en una línea: **con el bus rápido y la mitad cacheada el
punto baja a 406 µs sin suponer nada del chip (1,7×), y con la puerta baja a lo
que el chip tarde de verdad, con la traza correcta en cualquier caso (4,1× si
asienta en 150 µs, 2,1× si necesita 300)**. Las variantes de 100, 50 y 0 µs son
más rápidas y están **mal**: leen el bin anterior en los 8 bins de las
portadoras, que es justo donde el operador mira.

Dos presupuestos acotan al barrido en el hilo del RTX:

- **Pasada de 30 ms** (`SPECTRUM_PASS_MS`): si el barrido no ha terminado, el
  servicio devuelve el hilo y sigue en la pasada siguiente. La rejilla más ancha
  (160 bins) cabe en **una** pasada cuando la puerta encuentra un chip de
  150 µs (160 × 170 µs = 27 ms) y en dos en el peor caso del datasheet
  (160 × 331 µs = 53 ms); sin la puerta son 65 ms, dos pasadas. Antes eran
  111 ms y cuatro pasadas a 694 µs, y por eso un span ancho refrescaba a 4
  trazas/s.
- **Ventana de escucha de 40 ms** (`SPECTRUM_LISTEN_MS`) entre barrido y
  barrido, con el receptor aparcado en el centro (§7): un ciclo completo son
  ~48 ms y el refresco lo pone la pantalla (20 fps, `UI_SPEC_FRAME_MS`), no el
  receptor. La ventana no se ha tocado: es lo que mantiene el audio y la pausa
  por portadora, y ya no es el cuello de botella.

El coste real del equipo se puede leer en caliente: el menú rápido del
analizador muestra `MS/PTO` (promedio de `pointUs` desde que se abrió la
pantalla) y la pantalla marca el bin en curso bajo la traza. Con `ESPERA` en
`AUTO`, `MS/PTO` **es** el tiempo de asentamiento real de esa unidad más el bus:
es el número que hay que apuntar.

### 3.4 Cómo se cierra el número en la radio

El dato que falta —cuánto asienta de verdad esta unidad— se lee en la propia
pantalla, y es lo primero que hay que hacer al probar el analizador:

1. Abrir el analizador en una banda con al menos una portadora conocida (un
   repetidor local vale) y dejar `ESPERA` en `400us`: es el comportamiento
   antiguo. Anotar `MS/PTO` del menú rápido y mirar la traza.
2. Poner `ESPERA` en `AUTO`. `MS/PTO` pasa a ser el tiempo que el chip pide más
   el bus. Si la traza es la misma (mismo pico, mismas faldas, sin mesetas de
   dos bins) y `MS/PTO` ha bajado, `AUTO` es correcto y ése es el asentamiento
   real de la unidad.
3. Si con `AUTO` la traza sale más ancha o el pico se mueve de bin, el
   indicador de *glitch* no sirve en esa unidad: el barrido lo detecta solo
   (tres puntos seguidos sin que baje) y vuelve a 400 µs, pero conviene además
   fijar `C62_SPECTRUM_SETTLE_US` al menor valor de la tabla que dé la traza
   buena, que es lo que se hace con los pasos 1-2 en las cuatro entradas fijas.

### 3.5 Dónde está el límite físico

Con el bus fuera del camino, el suelo lo pone el filtro de FI: un filtro de
9 kHz tarda del orden de `1/BW` en llenarse con el bin nuevo (110 µs, una o dos
veces eso con el rizado del filtro), y por debajo de eso el RSSI es el del bin
anterior por muy rápido que se lea. El AFC también tiene que estar apagado o el
PLL persigue a la portadora. De ahí el suelo:

```
t_punto ≥ t_bus(≈ 5 µs a 2 MHz) + 1/BW_FI(≈ 110 µs) + t_RSSI(≈ 3 µs) ≈ 120 µs
```

o sea **~8 300 puntos/s, 5,8× el original (694 µs)**. Para pasar de ahí habría
que ensanchar la FI, que es exactamente lo que se pierde en resolución: con el
filtro de 4,5 kHz (sin `REG_43<5>`) el suelo se dobla, y un factor 20 exigiría
unos 45 kHz de FI, que este chip no tiene. **Cinco veces es el límite de este
receptor, no del código**, y el número exacto de una unidad se mide con `ESPERA`
en `AUTO` (§3.4).

Tamaño de lo añadido en esta rama sobre `86c3b7dc`, con
`-DCONFIG_C62_DMR_RX=n`: **+760 B de flash y +16 B de RAM** (imagen de
492 480 B de flash, 46,97 % de 1 MB, y 152 424 B de RAM, 58,15 % de 256 KB;
la base `86c3b7dc` con los mismos flags son 491 720 B y 152 408 B).
Las opciones nuevas son de la placa: `C62_SPECTRUM_SPI_HZ` (1 MHz),
`C62_SPECTRUM_SETTLE_US` (0 = puerta del chip), `C62_SPECTRUM_GATE_POLLS` (20)
y `C62_SPECTRUM_AGC_FIX` (n, §3.2d).

### 3.6 Lo que hacen otros firmwares del mismo chip

No hay que inventarlo: el BK4819 está en el Quansheng UV-K5 y sus forks, y tres
implementaciones libres de analizador dicen cómo lo resolvieron. Se cita fichero
y función; el método y los valores de registro son lo que se ha tomado, no el
código.

- **egzumer/uv-k5-firmware-custom** (GPL-3.0, `app/spectrum.c`; el analizador es
  de fagci, Apache-2.0). Es el más parecido a lo que hace esta rama:
  - `SetF()` escribe la frecuencia y después **reenciende `REG_30`** (`0` y de
    nuevo el valor guardado) para forzar la reactivación del camino de RX tras
    el retune, y conmuta el filtro de entrada por banda
    (`BK4819_PickRXFilterPathBasedOnFrequency`).
  - `GetRssi()` **no duerme**: `while ((BK4819_ReadRegister(0x63) & 0xFF) >= 255)
    SYSTICK_DelayUs(100);` con un `// SYSTICK_DelayUs(800);` comentado encima.
    Es decir: sustituyeron 800 µs fijos por el sondeo del indicador de *glitch*,
    que es el mismo método que usa `radio_spectrumPoint()` aquí (§3.2c), pero sin
    tope de sondeos.
  - `LockAGC()`/`RADIO_SetupAGC()` (`radio.c`) y `BK4819_SetAGC()` en
    `driver/bk4819.c` ponen `REG_7E<15> = 1` con `<14:12> = 3` (índice máximo)
    para el analizador: la ganancia fija, igual que aquí pero al máximo en vez de
    al índice en uso.
  - `BackupRegisters()`/`RestoreRegisters()` guardan y devuelven `REG_30`, `37`,
    `3D`, `43`, `47`, `48` y `7E`; `ToggleAFDAC()` (`REG_30<9>`) y
    `ToggleAFBit()` (`REG_47<8>`) silencian la salida durante el barrido.
  - `UpdateListening()` baja el filtro a `REG_43` estrecho para medir el bin de
    escucha y lo devuelve al ancho para barrer: dos perfiles, como aquí.
- **OneOfEleven/uv-k5-firmware-custom** (`panadapter.c`, GPL-3.0) hace lo
  contrario en velocidad y es la referencia de lo que *no* hay que hacer: un
  punto por tick de 10 ms (`PAN_process_10ms()`, `panadapter_delay = 3`) y una
  pausa de 100 ms en el VFO una vez por barrido, para no comerse el hilo. Su
  `PAN_set_freq()` llama a `BK4819_set_rf_frequency(freq, true)` con el comentario
  «don't bother» para el filtro de entrada: no lo conmuta dentro del span.
- **fagci/qs-uvk5-firmware-modder** es el origen del analizador de egzumer (mismo
  `spectrum.c`, misma licencia Apache-2.0) y de los mods que lo portaron
  (`armel`, `spm81`, `losehu`, `kamilsss655`).
- **OpenGD77** no sirve de referencia aquí, aunque se cite a menudo: el
  MD-UV380/MD-390 no lleva BK4819. Su árbol (`open-ham/OpenGD77`,
  `firmware/source/`) no contiene una sola referencia al chip — usa el
  **AT1846S** como transceptor y el **HR-C6000** para el 4FSK de DMR
  (`functions/calibration.c`, comentarios de `calibration[]`). Tampoco tiene
  analizador de espectro; su «scan» es de canales.
- **DualTachyon/uv-k5-firmware** tiene el driver `driver/bk4819.c` canónico del
  que salen los demás, sin analizador.

El chip no barre en ninguna de ellas: todas son bucles de sintonizar-leer, y la
diferencia entre unas y otras es exactamente cómo resuelven el asentamiento (un
sleep, un tick del planificador o el sondeo del *glitch*). De ahí que esta rama
haga del asentamiento lo único que se mide y se muestra.

## 4. La rejilla: span, paso y resolución real

Dos tablas en el servicio, elegidas desde el menú rápido:

| span (`SPAN`) | paso (`PASO`) |
|---|---|
| 200k, 500k, 1M, 2M, 5M, 10M, 20M, 50M, 96M | 12,5k, 25k, 50k, 100k, 250k |

El último span es el máximo del motor y sólo tiene sentido desde que el barrido
es rápido: 96 MHz en 160 bins son 600 kHz por píxel, una vista de ocupación de
banda entera en la que una portadora es una joroba a la que acercarse, no algo
que medir. Antes costaba 111 ms por barrido y cuatro pasadas del hilo del RTX;
ahora cabe en una pasada y se refresca como cualquier otro span.

El motor (`spectrum_configure()`) convierte la petición en una rejilla:

- `bins = span/paso + 1`, con tope **160** (el ancho del panel en píxeles: más
  bins que píxeles no dan detalle, sólo barras de menos de un píxel).
- Si la pareja pedida no cabe, el paso **sube** por la escalera 10, 12,5, 25,
  50, 100, 250, 500 kHz, 1, 2,5, 5 MHz hasta que quepa. El paso realmente usado
  lo dice `spectrumService_effectiveStep()`, y el span del pie es el barrido de
  verdad (`(bins - 1) * paso`), no el pedido.
- La rejilla es **absoluta**: cada bin cae en un múltiplo del paso y el centro
  del span siempre es un bin. Así dos barridos de la misma frecuencia se
  comparan bin a bin (lo necesita la retención de máximos) y la frecuencia de un
  bin se lee sin saber dónde empezó el barrido.
- Un span menor que dos pasos se ensancha a tres bins; un paso por debajo de
  10 kHz se sube a 10 kHz (lo más fino que afina el receptor).

**La resolución no es el paso, es el filtro de FI.** El perfil de barrido pone
el filtro de 4,5 kHz en modo de 25 kHz, o sea **~9 kHz de FI**: un portador se
dibuja como una meseta de uno o dos bins con faldones de ~15 dB por bin, sea
cual sea el paso. Con 12,5 kHz entre bins dos bins no se solapan, pero bajar de
ahí no daría más detalle (el chip no tiene filtro más estrecho que 1,7 kHz y el
analizador no lo usa: el paso mínimo de la tabla es 12,5 kHz). Al revés, un span
grande cuyo paso se ha forzado a 250 kHz o más **submuestrea** la FI: un
portador puede caer entre dos bins y aparecer atenuado. Es el compromiso de un
receptor barrido, no un defecto del dibujo. El mismo 9 kHz es el que pone el
suelo de tiempo de §3.5: la resolución y la velocidad salen del mismo filtro.

## 5. La pantalla

- **Ventana de dB fija: -30 dBm arriba, -130 abajo**, lineal en dB. El plot son
  90 filas, así que **1,12 dB por píxel**; la rejilla marca cada 25 dB, lo que
  permite leer un nivel a ±3 dB. La ventana cubre casi todo el rango del
  `REG_67` (-160 a -32,5 dBm). No se autoescala a propósito: una traza que se
  reescala esconde un desvanecimiento detrás de una escala que se mueve.
- Cada bin es una barra amarilla desde su nivel hasta el fondo, con 1 px de aire
  entre barras cuando el raster deja más de un píxel por bin (41 bins en 158 px:
  ~4 px por bin, 3 de barra y 1 de aire). La retención de máximos se dibuja en
  blanco sobre la traza, y el pico del último barrido lleva un marcador blanco de
  2 px en el borde superior de su columna.
- La columna del **bin en curso** se marca en gris bajo la traza: es lo que
  distingue un analizador que barre de una foto congelada.
- **Refresco a 20 fps** (`UI_SPEC_FRAME_MS = 50`), y ahora es el refresco el que
  manda: el barrido de un span de 41 bins son 7 ms y el de 160 bins cabe en una
  pasada (§3.3), con lo que el ciclo es barrido + 40 ms de escucha y el techo
  pasa a ser la pantalla. Un span de 160 bins pasó de cuatro pasadas y 4
  trazas/s a una o dos pasadas y 10-20 trazas/s. Se redibuja sólo cuando ha
  llegado un barrido nuevo o ha cambiado la pausa. El plot se compone entero en
  el framebuffer y se vuelca una vez por frame, así que nunca se ve medio barrido
  en pantalla.
- Menú rápido propio (`qm_spectrumMenu()`): `SPAN`, `PASO`, `ESPERA`
  (`AUTO`/50/100/200/400 µs, §3.2c), `MAX RET` (retención de máximos), `BORRAR
  MAX` (acción, no valor), `PICO` (dBm del último barrido) y `MS/PTO` (coste
  medido por punto).
- **La cabecera dice cómo espera el barrido**, que es lo que hace falta para
  cerrar el número de §3.4 sin abrir el menú: `AUTO` cuando la puerta del
  *glitch* está en uso, `50us`/`100us`/`200us`/`400us` cuando se ha fijado una
  espera, `400us` cuando la puerta se ha abandonado (tres puntos seguidos sin que
  `REG_63` baje) y `PAUSA` cuando hay una portadora en el centro. El span sigue
  en el pie, junto a la frecuencia central (`145.5000 +-1M`), así que la cabecera
  no quita información de la pantalla.
- La retención guarda el máximo por bin desde que se activó (o desde `BORRAR
  MAX`), y el suavizado opcional del motor es una media exponencial de 2, 4 u 8
  barridos en dB: quita el parpadeo del fondo sin retrasar más de un barrido la
  subida de un portador.

## 6. El gancho `sp` del protocolo de PC

`spectrumService_traceLine(buf, len)` serializa la traza que se está viendo:

```
sp <centro_Hz> <paso_Hz> <bins> <dBm> <dBm> ...
```

Un nivel por bin, en el orden de la rejilla, los mismos números que dibuja la
pantalla (traza suavizada y con retención no aplicada: `spectrum_level()`, no el
hold). Devuelve `false` si todavía no hay un barrido completo en pantalla o si
el buffer no cabe; para 41 bins la línea ocupa unos 190 B.

No hay nada que lo llame todavía: es el enganche que el control remoto llevaría
en su canal serie. Para cablearlo hacen falta dos cosas:

1. un comando en la tabla de `platform/drivers/audio/c62_remote.c` (los `rr` y
   `rw` son el modelo), que llame a `spectrumService_traceLine()`;
2. la respuesta por `baseband_dump_send_text()`, que es como el driver ya
   devuelve texto al PC.

Requiere un build con `C62_REMOTE=ON`, como el resto de comandos remotos.

## 7. Pausa, audio y squelch

El barrido corre **en el hilo del RTX** (`rtx_task()` llama a
`spectrumService_poll()` antes que nada). Es el único hilo que manda sobre el
chip: hacerlo desde la interfaz sería retunear el receptor bajo el RTX, que lee
el RSSI del mismo chip en cada pasada y vería cada bin como una apertura de
squelch.

- **Mientras barre:** el servicio devuelve `true` y el RTX se salta la lectura
  de RSSI y la actualización del modo. El AF está muteado y el squelch del modo
  no se alimenta, así que ni chilla con cada bin ni se oye el barrido.
- **Entre barridos:** el receptor se aparca en el centro con **su** filtro, su
  AFC, su AGC y **su** audio, y el camino normal del RTX corre entero. El
  squelch del modo decide si hay portador en el centro exactamente como fuera
  del analizador, y el audio se oye si lo hay.
- **Pausa:** si durante la ventana de escucha el squelch del modo está abierto,
  el barrido no arranca. La pantalla pone `PAUSA` en la cabecera y el receptor
  se queda en el centro con el audio abierto: el operador escucha lo que está
  midiendo. Al caer el portador, el barrido retoma solo.
- **Al cerrar:** se restauran `REG_43`, `REG_73`, `REG_7E` y la salida de AF del
  modo, se vuelve a la frecuencia configurada y se pide al RTX que reconstruya
  el perfil (`ui_rtxSyncRequest`). Durante la sesión, cualquier
  `radio_updateConfiguration()` marca el perfil de barrido como sucio y el
  siguiente barrido lo vuelve a poner; también invalida la palabra del PLL
  cacheada, porque otra cosa ha podido sintonizar.
- **PTT:** la pantalla pone `state.txDisable` mientras está abierta, igual que
  las listas: desde el analizador no se transmite.
- Los modos digitales quedan fuera (§1): su squelch y su demodulador necesitan
  el discriminador continuo, que es justo lo que el barrido se lleva por delante.

## 8. Tests

- `tests/unit/spectrum.cpp`, 8 casos: rejilla absoluta y centrada en un bin,
  paso que se eleva para caber, centro que engancha a la rejilla, portador
  sintético en su bin, retención de máximos, suavizado, mapeo nivel→fila y
  descarte de la traza al cambiar de raster.
- `tests/unit/ui_spectrum.cpp`, 7 casos: la pantalla abre en el VFO y devuelve el
  receptor, las teclas hacen lo que dicen las pistas, el barrido sintético deja
  el portador en el bin correcto, el menú rápido del span y el paso, el acceso
  desde el menú del modo, el rechazo en digital y el dibujo (píxeles reales del
  framebuffer).
- `tests/unit/ui_layout.cpp`: alineación a la derecha (el margen derecho y la
  cabecera terminan dentro de la pantalla).
- `tests/platform/c62_spectrum_cost.c`: el banco de coste de §3.3, en QEMU, con
  la tabla de variantes y el chip modelado con su tiempo de asentamiento como
  parámetro.

## 9. Lo que no hace

- **No hay I/Q**: no demodula, no mide fase ni calidad de modulación; no es un
  analizador vectorial. El chip tampoco tiene salida I/Q ni captura de banda
  ancha, y su «Frequency Scan» es un contador de frecuencia de una portadora
  (§2), no un barrido.
- **No es un medidor de potencia calibrado**: el nivel es el RSSI del chip
  (±unos dB, sin calibración por banda) y con el AGC fijo es una medida a
  ganancia constante, comparable entre bins y entre barridos pero no absoluta.
  Sirve para comparar y para encontrar un portador, no para certificar una
  potencia.
- **No barre en digital** (DMR, TETRA, M17, …) ni transmitiendo.
- **No hay cascada ni promediado en frecuencia**; el suavizado es en tiempo.
- **El suelo es el filtro de FI**, no el código: ~120 µs por punto con 9 kHz de
  FI (§3.5), y un span de 160 bins ya no necesita más de una pasada.
- **No cruza el hueco 660–840 MHz**: el span se recorta a la banda del centro.
