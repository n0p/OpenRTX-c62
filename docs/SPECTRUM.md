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
espectro es, por tanto, un **barrido**: sintonizar, dejar asentar el PLL y leer
el RSSI, un punto cada vez. La resolución la pone el filtro de FI y el precio,
un asentamiento de PLL por punto.

Consecuencias que el código respeta:

- **Afina por palabra de 10 Hz** (`REG_39` y luego `REG_38`), así que cualquier
  frecuencia de la rejilla es alcanzable.
- **Dos bandas, no una:** 18–660 MHz y 840–1300 MHz. Un span que cruzase el
  hueco gastaría la mitad de sus bins en frecuencias que el VCO no alcanza y las
  leería como ruido de fondo: `clampToBand()` recorta el span a la banda del
  centro y mete el centro dentro de ella.
- **La LNA se conmuta en 174 MHz.** Un barrido que cruza esa frecuencia cambia
  `LNA_VHF_EN`/`LNA_UHF_EN` al pasar; sólo cuando cruza, así que un span dentro
  de una banda no paga nada.
- **Dos perfiles de chip.** Para barrer: modo de canal de 25 kHz con
  `REG_43 = 0x7e28` (filtro de FI de 4,5 kHz, duplicado a 9 kHz por el modo,
  §4) y salida de AF muteada. Para escuchar: el `REG_43` y la salida de AF que
  el modo tenía, guardados al entrar, y la frecuencia configurada. Cambiar de
  uno a otro es `radio_spectrumSweep(true/false)`.

Sólo la C62 implementa estas llamadas (`radio_spectrumEnter/Point/Park/Leave`).
En el resto de builds, incluido el de escritorio y el de los tests, el servicio
responde con una banda sintética de tres portadoras a desplazamientos fijos del
centro (con la falda de 15 dB/bin de un filtro de FI): es lo que permite
ejercitar pantalla, rejilla y escalado sin radio.

## 3. Método y coste medido

El coste se mide con `tests/platform/c62_spectrum_cost.c`: metal desnudo sobre
QEMU (`mps2-an505`, Cortex-M33), contando instrucciones con el plugin de TCG y
modelando el SPI bit-bang real de la C62 (4 µs por bit, 3 bytes por acceso =
96 µs de bus) y el registro del BK4819. Lo que se mide es un punto completo:
`bk4819_setFrequency()` (dos escrituras), el asentamiento, `bk4819_getRssi()`
(una lectura) y `spectrum_store()`.

Números medidos:

| magnitud | valor |
|---|---|
| coste de un punto (todo incluido) | **694 µs** |
| puntos por segundo | **1440** |
| barrido por defecto (1 MHz / 25 kHz = 41 bins) | **28,5 ms** |
| parte del punto que es el asentamiento del PLL | **58 %** (400 µs) |

El asentamiento es el 58 % del coste porque los 400 µs se duermen
(`k_usleep()`), no se gastan en trabajo: es el único número que no está medido
aquí sino tomado del datasheet («menos de 0,3 ms en la reselección de canal»,
BK4819 V1.0 §1.2.3) con un tercio de margen. El resto del punto son dos
escrituras de PLL, la lectura de RSSI y la aritmética del motor.

Dos presupuestos acotan al barrido en el hilo del RTX:

- **Pasada de 30 ms** (`SPECTRUM_PASS_MS`): si el barrido no ha terminado, el
  servicio devuelve el hilo y sigue en la pasada siguiente. Con 41 bins cabe
  entero (28,5 ms); con 160 bins hacen falta dos pasadas y el RTX no se queda
  nunca más de 30 ms.
- **Ventana de escucha de 40 ms** (`SPECTRUM_LISTEN_MS`) entre barrido y
  barrido, con el receptor aparcado en el centro (§7): un ciclo completo son
  ~68 ms, unas 14 trazas por segundo.

El coste real del equipo se puede leer en caliente: el menú rápido del
analizador muestra `MS/PTO` (promedio de `pointUs` desde que se abrió la
pantalla) y la pantalla marca el bin en curso bajo la traza.

Tamaño de lo añadido, con los flags de la rama: **+5164 B de flash y +728 B de
RAM** sobre la base `a1e4d304` (motor, servicio, pantalla, el gancho de protocolo
y la corrección de la alineación a la derecha). La imagen queda en 485 080 B de
flash (46,26 % de 1 MB) y 167 576 B de RAM (63,93 % de 256 KB).

## 4. La rejilla: span, paso y resolución real

Dos tablas en el servicio, elegidas desde el menú rápido:

| span (`SPAN`) | paso (`PASO`) |
|---|---|
| 200k, 500k, 1M, 2M, 5M, 10M, 20M, 50M | 12,5k, 25k, 50k, 100k, 250k |

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
receptor barrido, no un defecto del dibujo.

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
- **Refresco a 20 fps** (`UI_SPEC_FRAME_MS = 50`), aunque el barrido pueda dar
  35/s: se redibuja sólo cuando ha llegado un barrido nuevo o ha cambiado la
  pausa. El plot se compone entero en el framebuffer y se vuelca una vez por
  frame, así que nunca se ve medio barrido en pantalla.
- Menú rápido propio (`qm_spectrumMenu()`): `SPAN`, `PASO`, `MAX RET`
  (retención de máximos), `BORRAR MAX` (acción, no valor), `PICO` (dBm del
  último barrido) y `MS/PTO` (coste medido por punto).
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
- **Entre barridos:** el receptor se aparca en el centro con **su** filtro y
  **su** audio, y el camino normal del RTX corre entero. El squelch del modo
  decide si hay portador en el centro exactamente como fuera del analizador, y
  el audio se oye si lo hay.
- **Pausa:** si durante la ventana de escucha el squelch del modo está abierto,
  el barrido no arranca. La pantalla pone `PAUSA` en la cabecera y el receptor
  se queda en el centro con el audio abierto: el operador escucha lo que está
  midiendo. Al caer el portador, el barrido retoma solo.
- **Al cerrar:** se restaura el `REG_43` y la salida de AF del modo, se vuelve
  a la frecuencia configurada y se pide al RTX que reconstruya el perfil
  (`ui_rtxSyncRequest`). Durante la sesión, cualquier
  `radio_updateConfiguration()` marca el perfil de barrido como sucio y el
  siguiente barrido lo vuelve a poner.
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
- `tests/platform/c62_spectrum_cost.c`: el banco de coste de §3, en QEMU.

## 9. Lo que no hace

- **No hay I/Q**: no demodula, no mide fase ni calidad de modulación; no es un
  analizador vectorial.
- **No es un medidor de potencia calibrado**: el nivel es el RSSI del chip
  (±unos dB, sin calibración por banda). Sirve para comparar y para encontrar un
  portador, no para certificar una potencia.
- **No barre en digital** (DMR, TETRA, M17, …) ni transmitiendo.
- **No hay cascada ni promediado en frecuencia**; el suavizado es en tiempo.
- **Una pasada no puede durar más de 30 ms**: un span de 160 bins tarda dos
  pasadas (~57 ms) y el refresco se queda en 20 fps.
- **No cruza el hueco 660–840 MHz**: el span se recorta a la banda del centro.
