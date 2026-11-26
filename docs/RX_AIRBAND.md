# Recepción AM en banda aérea

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Modo de recepción
para 118.000-136.975 MHz: AM con doble banda lateral y portadora (DSB-FC), los
dos rasters de canales de la banda (25 kHz y 8,33 kHz), control automático de
ganancia y squelch por portadora.

Es **solo recepción por defecto**: un transmisor que modula en frecuencia no
puede producir AM, así que no hay camino de TX en el modo,
`opmode_canTransmit()` devuelve `false` y `radio_enableTx()` sale sin tocar el
PA. Con la opción de compilación `C62_AIR_TX` el modo también transmite, pero no
por aquí: la amplitud se modula en el PA con la línea del APC y el BK4819 emite
una portadora desnuda. Ese camino está en **docs/TX_AIRBAND.md**, y todo lo que
sigue en este documento describe el receptor, que no cambia.

- `platform/drivers/baseband/BK4819.c` — el camino de AM del chip.
- `platform/drivers/baseband/radio_C62.cpp` — el modo en el driver de radio: el
  perfil del BK4819, el lazo grueso de ganancia y el enganche del procesado de
  audio.
- `openrtx/include/core/agc.h`, `openrtx/src/core/agc.c` — el AGC y el
  limitador de audio.
- `openrtx/include/core/airband.h`, `openrtx/src/core/airband.c` — los rasters,
  los designadores de 8,33 kHz y el ajuste a canal.
- `openrtx/include/rtx/OpMode_Airband.hpp`, `openrtx/src/rtx/OpMode_Airband.cpp`
  — el modo: estado, squelch y camino de audio.
- `platform/drivers/audio/csk6_codec.c` — el enganche por el que el audio del
  canal de radio se procesa antes del altavoz.
- `tests/platform/c62_airband_agc_sim.c` — el banco de medidas del AGC.
- `tests/unit/agc.cpp`, `tests/unit/airband.cpp` — las pruebas.

---

## 1. De dónde sale la amplitud en el BK4819

El BK4819 es un transceptor de FM: su lista de registros V3
(`datasheets/BK4819V3_Registers_List_20201218.pdf`) no tiene modo AM, ni
detector de envolvente, ni nada que se le parezca. Lo que sí tiene es un
selector de salida de audio, REG_47<11:8>, con seis valores documentados (mudo,
audio normal, tono, beep, CTCSS/CDCSS, FSK) y el resto «reserved».

**El valor 7 es el detector de AM.** No es una invención de este puerto: el
firmware de fábrica de la C62 recibe en AM la banda aérea con él. La función
`radio_rx_tune()` en `0x18019648` (reconstruida en
`forks/c62-analysis/reconstructed/radio/bk4819.c`) hace:

```c
if (freq_hz - 100000000u <= 34000000u) {   /* 100..134 MHz: AM */
    bk4819_write_reg(0x43, 0x0048);        /* 12.5 kHz, filtro IF 1.7 kHz */
    bk4819_write_reg(0x47, 0x6740);        /* AF = AM: <11:8> = 7 */
    bk4819_write_reg(0x13, 0x03FF);        /* AGC, escalón superior a 0 dB  */
    bk4819_write_reg(0x4A, 0x5448);
    return;
}
```

La consecuencia de diseño es que **la envolvente se produce dentro del chip** y
sale por el mismo camino de audio que el discriminador de FM: no hay que leer
el RSSI para reconstruir la señal, ni muestrear la IF antes del limitador, ni
tocar el ADC. Todo lo que hay aguas arriba —LNA, mezclador, filtro de IF, el
AGC de RF, el RSSI de REG_67 y el squelch por RSSI de REG_78— sigue funcionando
igual, y lo que llega al códec (CSK6, 48 kHz, canal derecho) es la
**amplitud de la portadora multiplicada por la voz**. Es exactamente lo que un
receptor de AM tiene que entregar, y es también el problema: el nivel de audio
ya no es constante, sigue a la intensidad de campo, y por eso el modo necesita
el AGC de la sección 3.

### Registros del perfil de recepción AM

Valores de `bk4819_setAmReceive()` y `bk4819_setModulation(false)`; la columna
«fuente» dice de dónde sale cada uno.

| Registro | FM | AM | Qué es | Fuente |
|---|---|---|---|---|
| REG_47<11:8> | 1 | **7** | selector de salida de audio: audio normal / detector AM | stock `0x18019648` (`0x6740`); lista V3 para el 1 |
| REG_47<14:12>, <6> | 6, 1 | 6, 1 | sin documentar, iguales en las dos | stock, init del driver (`0x6040`) |
| REG_43 | 0x4048 / 0x3028 | **0x0048** (estrecho) / 0x3628 (ancho) | modo de BW y filtro de IF | stock (`0x0048`); V3 §REG_43 para los campos |
| REG_13 | 0x03BE | **0x03FF** | escalón superior de la tabla de AGC de RX | stock |
| REG_4A | 0x5450 | **0x5448** | sin documentar, difiere en el bit 3 | stock |
| REG_2B<8> | 0 | **1** | desactiva el de-énfasis de AF de RX | V3 §REG_2B |
| REG_28<15:14> | 01 (1:2) | **00 (off)** | expansor de AF de RX | V3 §REG_28 |
| REG_2B<10>, <9> | 0 | 0 | HPF 300 Hz y LPF 3 kHz **activos** | V3 §REG_2B |
| REG_48<11:10> | 0 | 0..3 | ganancia de AF de RX, escalón grueso del AGC | V3 §REG_48 |

Dos matices que importan y que el firmware de fábrica no toca:

- **El de-énfasis se apaga.** Un transmisor de AM no pre-énfatiza, así que un
  receptor que de-énfatiza apaga los agudos y deja la voz sorda. El HPF de
  300 Hz se queda: es lo que quita la continua de la portadora detectada, que si
  no llegaría al altavoz como un golpe de continua en cada apertura de squelch.
- **El expansor se apaga.** El 1:2 del REG_28 deshace parte de lo que hace el
  AGC (sube lo fuerte y baja lo débil), que es justo lo contrario de lo que
  necesita una señal que se mueve 40 dB.

El resto de la cadena de recepción no cambia: `bk4819_enableRx()` enciende los
mismos caminos, y al volver a FM o a un modo digital `bk4819_setModulation(true)`
re-escribe REG_13, REG_4A, REG_2B<8> y REG_28 para que no hereden el perfil AM.

### Por qué el AGC del chip no basta

El chip tiene su propio AGC de RF: una tabla de cinco escalones
(REG_10..REG_14, índices 3, 2, 1, 0, -1), dos umbrales en REG_49<13:7>/<6:0>
(sin documentar, 1 dB/LSB) y el modo fijo o automático en REG_7E<15>, con el
índice en REG_7E<14:12>. En AM el firmware de fábrica deja el escalón superior a
0 dB por etapa (REG_13 = 0x03FF) y el AGC en automático.

Ese lazo funciona —está medido por la comunidad del UV-K5 en
`https://github.com/egzumer/uv-k5-firmware-custom/wiki/Development-notes`— pero
en AM tiene **cinco escalones** y tiende a oscilar entre dos de ellos, así que
deja saltos de varios dB en la señal. Lo que el AGC del chip no puede hacer es
lo que hace falta aquí: el nivel de **audio** no lo controla él, porque la
amplitud que entrega el detector es proporcional a la portadora. De ahí los dos
lazos que sí están en el firmware:

1. **Lazo grueso, en el chip** (`amAfGainStep()` en `radio_C62.cpp`): mueve el
   escalón de ganancia de AF de RX, REG_48<11:10>, 6 dB por escalón, mirando el
   pico que el códec ha digitalizado (`csk6codec_getRxPeak()`, retención de
   100 ms), una vez cada 300 ms. Su trabajo no es nivelar, es que una torre
   fuerte no meta el ADC del códec en recorte.
2. **Lazo fino, por muestra** (`core/agc.c`): el que nivela. Sección 3.

### Por qué el squelch es de portadora y no de ruido

En FM, sin portadora el receptor entrega ruido a todo volumen y con portadora
entrega lo que haya modulado: el «squelch de ruido» (REG_4F, umbral de
ex-noise) funciona porque el ruido *desaparece* al aparecer la portadora. En AM
es al revés: la amplitud de audio es la de la portadora, de modo que un AGC que
nivela hace subir el ruido cuando no hay nada y lo baja cuando llega la señal.
Un squelch de ruido en AM se abre con el silencio y se cierra con la emisora.

Lo que sí es la portadora es el **RSSI** (REG_67, 0,5 dB/escalón, convertido a
dBm en `bk4819_getRssi()`), y ese es el que usa el modo, igual que OpMode_FM.

---

## 2. Arquitectura del modo

```
BK4819 (detector AM)  ->  CSK6 códec 48 kHz  ->  AGC + limitador  ->  altavoz
        ^                        ^                      ^
        |                        |                      |
  radio_setOpmode()      csk6codec_setRxProcessor()   core/agc.c
  bk4819_setAmReceive()  (lo instala radio_C62)       (lo llama el worker)
        ^
        |
  OpMode_Airband: estado RX + squelch por RSSI + audioPath_request()
```

- `OpMode_Airband` es **política**: estado del receptor, squelch y abrir o
  cerrar el camino `SOURCE_RTX -> SINK_SPK`. No toca registros ni muestras.
- `radio_C62.cpp` es **camino de señal**: configura el chip al entrar y salir
  del modo y corre el lazo grueso de ganancia desde `radio_getRssi()`, que ya se
  llama desde el hilo de RTX 33 veces por segundo.
- El AGC por muestra cuelga del **worker de audio** del códec, a través de
  `csk6codec_setRxProcessor()`, que solo se aplica al camino radio→altavoz. Se
  instala antes de abrir el camino y se quita después de cerrarlo, así que el
  worker nunca llama a un modo que no está corriendo.

## 3. AGC y limitador

`core/agc.c`, 48 kHz, punto fijo Q16 (65536 = 0 dB). Dos etapas en un solo
recorrido por muestra:

- **Detector**: seguidor de pico sobre la salida (antes del limitador), con
  subida instantánea y bajada de 5 ms. Que mire la salida y no la entrada es lo
  que hace que un escalón fuerte baje la ganancia aunque el limitador ya esté
  recortando.
- **Ganancia**: cae una fracción de sí misma por muestra (`gain -= gain >> 9`,
  **10 dB en 12 ms**) o sube (`gain += gain >> 15`, **10 dB en 780 ms**). Al ser
  multiplicativa, la constante de tiempo es la misma en dB/ms en cualquier
  ganancia; con un paso aditivo, la subida desde +24 dB sería seis veces más
  lenta en dB que desde 0 dB.
- **Ventana con histéresis**: la ganancia se queda quieta entre 0,25 y 0,35 de
  fondo de escala (−12 a −9 dBFS); por encima ataca, por debajo libera. Son 3 dB
  de histéresis, los mismos que evitan que una señal en el umbral bombee.
- **Rango**: +24 dB a −30 dB.
- **Limitador**: lineal hasta 0,82 de fondo de escala y 4:1 por encima, con tope
  en fondo de escala. Lo que el lazo no llega a tiempo de bajar se comprime, no
  se recorta.

Números medidos con `tests/platform/c62_airband_agc_sim.c` (voz sintética de
tres tonos con envolvente de sílaba, ruido del receptor a −42 dBFS):

**Curva de transferencia** (voz constante, nivel ya asentado, tras 2 s):

| portadora | entrada | salida | ganancia |
|---|---|---|---|
| −50 dBFS | −40,9 | −18,1 | +22,8 |
| −40 dBFS | −38,2 | −15,5 | +22,8 |
| −30 dBFS | −33,6 | −11,0 | +22,5 |
| −20 dBFS | −26,2 | −8,0 | +18,1 |
| −10 dBFS | −16,8 | −8,4 | +8,4 |
| 0 dBFS | −6,9 | −8,4 | −1,5 |

De −25 a 0 dBFS de portadora (25 dB) la salida se mueve **0,6 dB**. Por debajo
de −30 dBFS el AGC está en su techo de +24 dB y la salida sigue a la portadora:
ahí ya no hay nada que nivelar, lo que falta es señal.

**Captura con desvanecimientos** (6 s, subida de 48 dB, un desvanecimiento de
25 dB en 40 ms y otro de 40 dB en 120 ms, con 2 s de pre-roll para que el lazo
esté asentado):

```
  0 s: portadora -38.7 dBFS, altavoz -14.6 dBFS
  1 s: portadora -34.9 dBFS, altavoz -10.8 dBFS
  2 s: portadora -31.1 dBFS, altavoz  -8.4 dBFS
  3 s: portadora -23.5 dBFS, altavoz  -7.7 dBFS
  4 s: portadora -15.5 dBFS, altavoz  -5.5 dBFS
  5 s: portadora  -7.4 dBFS, altavoz  -7.9 dBFS
dispersión (portadora > -30 dBFS, 24 ventanas): entrada 22.3 dB, salida 9.4 dB
nivel medio de salida -9.9 dBFS, desviación típica 2.5 dB
rango de ganancia -1.5..+24.1 dB
muestras recortadas: entrada 0, salida 0 de 288000
desvanecimiento de 40 dB: la salida cae a -20.8 dBFS y vuelve con la portadora
```

Los 9,4 dB que quedan no son del lazo: son los primeros segundos de la captura,
donde la portadora sube 8 dB/s y el AGC va con el techo puesto. Con el nivel ya
en rango, la curva de transferencia es la que manda: 0,6 dB de variación.

**Audio real.** Con la misma cadena, sobre una grabación de la banda que publica
Signal Identification Wiki (SDR#, 135.900 MHz, 2,99 s, convertida a 48 kHz
mono; el fichero no está en el repositorio):

```
29 ventanas de 100 ms: entrada -15.7..-10.8 dBFS (5.0 dB)
                       salida  -14.1..-4.7 dBFS (9.4 dB), 0 muestras recortadas
correlación normalizada entrada/salida 0.94
```

La dispersión de salida *crece* porque la grabación ya venía niveleada: el AGC
solo tiene que hacer algo en los huecos entre frases, donde la liberación sube
la ganancia (+5,6 dB al final del fichero), y la primera sílaba de la frase
siguiente entra 5 dB alta. El cuerpo de la transmisión se queda en -8,5 a -9,5
dBFS, que es su ventana. Con un escalón de 30 dB en medio y un desvanecimiento
de 40 dB/120 ms añadidos al mismo audio: la mitad débil sube 19 dB en 1,3 s
(liberación); el escalón de 30 dB recorta 8 muestras (0,17 ms) a fondo de escala
mientras el ataque baja la ganancia, y se asienta en -8,7 dBFS 200 ms después;
el desvanecimiento baja la salida 4 dB y vuelve con la portadora.

Lo que hay que repetir con una torre de verdad está en la sección 7.

## 4. Squelch

`OpMode_Airband.cpp`, sobre el RSSI que el hilo de RTX ya filtra (constante de
60 ms a 33 Hz):

- umbral = el nivel de S que el operador elige (`state.channel.sqlLevel`,
  convertido con `sLevelToRssi()`), **el mismo mando que en FM**;
- abre 1 dB por encima del umbral, cierra 3 dB por debajo: 4 dB de histéresis,
  más que los 2 dB de FM porque el RSSI de AM no lleva la información de FM y en
  la banda hay portadoras que aparecen y desaparecen;
- **retención de 400 ms** (`SQL_HANG_MS`): un avión que se desvanece un instante
  no corta la sílaba. Es el mismo motivo por el que el AGC libera despacio.

El camino de audio se abre y se cierra con `audioPath_request()` /
`audioPath_release()`, así que al cerrar el squelch el propio driver de audio
pone la salida de AF del chip en mudo.

## 5. Canales: 25 kHz, 8,33 kHz y nomenclatura

- Raster de **25 kHz**: 760 canales de 118.000 a 136.975 MHz, el que imprimen
  casi todas las cartas. Un canal se llama por su frecuencia, «118.025».
- Raster de **8,33 kHz**: 2278 canales, separación exacta de 25/3 kHz = 8333,33
  Hz, obligatorio en Europa por encima del FL195 desde 2018. Sus portadoras no
  son kilohercios decimales (118.00833, 118.01667), así que el canal se nombra
  con un **designador** cuyos dos últimos dígitos solo pueden ser 00, 05 o 10 en
  cada hueco de 25 kHz: **«118.005» es el canal cuya portadora es 118.00833**, y
  «118.010» el de 118.01667. Sintonizar el designador tal cual deja el receptor
  3,3 o 6,7 kHz fuera del canal.

`core/airband.c` tiene la aritmética de los dos rasters, la conversión
frecuencia → designador (`airband_channelName()`) y designador → portadora
(`airband_nameToFreq()`), y `airband_snap()`, que lleva una frecuencia al canal
más cercano.

En el receptor:

- el modo sintoniza **con ajuste al raster de 8,33 kHz**
  (`radio_enableRx()`): el paso del dial es 8333 Hz enteros, así que tras muchos
  pasos la frecuencia se va una fracción de hercio por paso, y un canal tecleado
  por su designador no está en el raster. El ajuste es inofensivo fuera de la
  banda, y dentro lleva a 118.005 a 118.00833, que es su portadora;
- el paso de 8,33 kHz está en `freq_steps[]` (`state.c`) **al final de la
  tabla**, porque el índice del paso elegido se guarda en los ajustes;
- la zona `ZONE_AIRBAND` ofrece los 760 canales de 25 kHz con el nombre de su
  frecuencia, en modo AM y **solo recepción**; el almacén de canales acepta
  `OPMODE_AIRBAND` y lo muestra como «AM»;
- el filtro de IF sigue al espaciado del canal: **1.7 kHz** en modo 12,5 kHz
  para 8,33 kHz de separación (el valor de fábrica) y **6 kHz** (3 kHz en modo
  25 kHz, doblados por REG_43<5>) para canales de 25 kHz.

## 6. Coste

Medido en QEMU (`mps2-an505`, Cortex-M33, plugin de conteo por instrucción de
`work/dsp-probe/common/libinsnframe.so`, marcador cada 12000 muestras y nada más
entre marcadores que `agc_process()`):

| build | instrucciones/muestra | por trama de 1 ms (48) | a 48 kHz sobre 200 MHz |
|---|---|---|---|
| `-O2` | **41,7** | 2002 | ≈ 1,0 % de la CPU |
| `-Os` | 44,5 | 2136 | ≈ 1,1 % |

Memoria, con `-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y` (la misma
configuración que la referencia):

| | flash | RAM |
|---|---|---|
| `master` `44ece688` | 518 104 B (49,41 %) | 196 688 B (75,03 %) |
| rama `c62/rx-airband` | 519 480 B (49,54 %) | 196 752 B (75,05 %) |
| **diferencia** | **+1 376 B** | **+64 B** |

Los 64 B de RAM son el estado del AGC (44 B), el contador de escalón y su marca
de tiempo, y los cinco campos del objeto `OpMode_Airband`. No hay reservas
dinámicas ni buffers nuevos: el AGC trabaja sobre el bloque que le pasa el
worker de audio.

## 7. Lo que falta por probar en radio

Todo lo de arriba está medido en host y en QEMU, con señal sintética. En la
radio falta:

1. **Que el detector AM del chip sea el que dice el firmware de fábrica.** Se
   comprueba en un minuto: modo aéreo en 118.000, un generador de AM al 30 % y
   mirar si hay voz en el altavoz. Si el valor 7 de REG_47 no diera envolvente,
   el sospechoso siguiente es REG_4A (0x5448 frente a 0x5450) y después REG_31,
   que en el BK4829 del UV-K1 sí es «AM demodulation enable».
2. **El punto de trabajo de REG_48.** El lazo grueso empieza en −6 dB y mueve el
   escalón cada 300 ms; con una torre fuerte y el AGC del chip en su escalón
   superior, ¿se queda corto? Se ve en el pico del códec con el volcado de
   banda base (`stats ... rxpk=`) y se ajusta `AM_RX_AUDIO_GAIN1`.
3. **El filtro de 1,7 kHz para 8,33 kHz.** Es el de fábrica, pero corta las
   bandas laterales de una AM que ocupe ±2,7 kHz; si la voz sale sorda, el
   ajuste alternativo ya está escrito (`BK4819_AM_WIDE`, 6 kHz). Se comparan las
   dos con la misma torre.
4. **El squelch con AGC en banda real**: si con una portadora fuerte sin
   modulación se abre y deja pasar ruido, el umbral está bajo; si una torre
   cercana no lo abre, el RSSI de esta banda (REG_78 de fábrica es 0x4846) puede
   necesitar la calibración por banda que tiene el firmware original.
5. **El ajuste a 8,33 kHz** contra un canal conocido: sintonizar «118.005» y
   comprobar con un segundo receptor o un analizador que la portadora cae en
   118.0083.
6. **Cómo suena con una torre de verdad.** El nivel está medido (sección 3), pero
   lo que hay que escuchar es si el AGC se oye («pumping») entre sílabas, si la
   primera sílaba tras un hueco entra demasiado alta, y si la liberación de 780
   ms deja pasar el ruido de un desvanecimiento. Si el ataque de 12 ms se queda
   corto con portadoras muy fuertes, `gainAttackShift` en `core/agc.c` es el
   número que se toca (9 → 10 lo dobla en tiempo, y con él el riesgo de modular
   la ganancia al ritmo de la voz).
7. **La pantalla con plazo** (sección 9). Con la radio en marcha, `diag` tiene
   que enseñar `screen ... 0 dropped` y un `last` de ~16-20 ms; el `worst` no
   debería acercarse a 100. Después de la conmutación a AM, otro `diag`: si
   aparece un descarte, el descriptor dice qué transferencia se perdió (comando
   o píxeles, y cuántos bytes) y la migaja `DISP_DROP` del registro siguiente
   dirá cuándo. Ese número es el que falta: dice si el fallo es del arranque de
   la DMA (una transferencia pequeña, un comando) o del completado de la grande
   (los 40 960 B del frame).
8. **La recuperación del panel.** Provocarla a mano es difícil sin tocar el
   driver; la comprobación mínima es que después de un descarte la pantalla
   vuelve sola en el frame siguiente (el `SPI_Control` forzado por la segunda
   `spi_config`). Si no vuelve, la radio sigue usable y es lo que hay que
   apuntar: la pantalla se quedó congelada, no la radio.
9. **El teclado con hilo propio.** Que el PTT y el botón de encendido sigan
   respondiendo con la pantalla congelada, y que la latencia del PTT no haya
   empeorado de forma audible (el escaneo es de 20 ms; el filtro de la puerta
   del PTT es de 25 ms). Se mide con el `tx` del protocolo de depuración.


## 8. La UI del modo

El modo está enganchado desde `c62/ui-modes`: es una entrada del registro de
modos (`uiModes[]` en `openrtx/src/ui/default/ui_quickmenu.c`, ver
`ui/ui_modes.h` y `docs/UI.md` §«Añadir un modo»), así que se alcanza con la
tecla lateral, el editor de canales lo ofrece (un canal de la banda aérea guarda
su modo y su filtro de FI) y la tecla lateral lo recorre como a los demás.

Lo que la UI muestra y dónde está el dato:

| pieza de UI | fuente |
|---|---|
| etiqueta «AM» en la cabecera de la pantalla principal | `chstore_modeName(OPMODE_AIRBAND)` |
| línea 2: `AM 8.33 118.005` (modo, raster y designador de canal) | `airband_channelName()` + `settings.am_step` |
| línea 1: `AGC sql7` | `settings.am_agc`, `settings.sqlLevel` |
| menú rápido y Ajustes → AM | tabla de filas `qm_items_airband[]` |
| teclear un designador (`airband_nameToFreq("118.005")` da la portadora) | todavía no: la entrada de frecuencia son dígitos de frecuencia, y `SNAP` lleva cualquier frecuencia a la portadora del canal más cercano |
| indicar que el canal no emite | `opmode_canTransmit(OPMODE_AIRBAND) == false` (con `C62_AIR_TX`, `TX-LOCK` / `TX-ARM` en la línea 1, ver docs/TX_AIRBAND.md) |

Filas del menú rápido / página de ajustes (la misma tabla):

| fila | qué hace | ajuste persistente |
|---|---|---|
| `SQL` | squelch **de portadora** (0..15), el mismo nivel que FM pero midiendo el RSSI | `settings.sqlLevel` |
| `AGC` | AGC de audio del camino AM encendido/apagado | `settings.am_agc` |
| `STEP` | raster de 25 kHz o de 8,33 kHz; mueve el paso global del VFO | `settings.am_step` |
| `SNAP` | ajusta cada paso y cada frecuencia tecleada a la portadora del canal 8,33 más cercano | `settings.am_snap` |
| `CH` | designador del canal en el que está el receptor (solo lectura) | — |
| `IF` | filtro de FI ancho (25 kHz) o estrecho (12,5 kHz) del detector AM | `state.channel.bandwidth` |
| `FINE`, `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG` | la cola común a todos los modos | — |

Queda fuera, a propósito, un mando de ganancia manual: el AGC del BK4819 y el de
audio son automáticos y el operador solo los enciende o los apaga. Guardar una
frecuencia de la banda en el banco desde el navegador de zonas sigue sin estar
(`zones_toStore()` + `chstore_add()`), pero el editor de canales ya la escribe.

## 9. La conmutación al modo: qué hace y dónde está su límite

Entrar en AM desde FM, en un VFO cualquiera, dejó una vez la radio con el teclado
muerto y la pantalla congelada —y con el receptor, el protocolo y el reloj de
actividad funcionando, que fue lo que permitió localizarlo—. Esta sección es la
ruta completa de la conmutación, la traza que dio la caja negra, lo que se
arregló y el límite que queda.

### La causa, medida en la radio

La caja negra lo dijo sin ambigüedad: `STALL [ui]` con el hilo de RTX **vivo**,
girando en `sys_clock_cycle_get_32`, y el de UI en estado *queued* —en la cola de
listos, sin que nadie lo elija—. No era un semáforo: era el planificador.

`rtx_threadFunc()` es un bucle sin espera: llama a `rtx_task()` una y otra vez y
solo cede cuando algo de dentro se bloquea. El hilo de UI corre **a la misma
prioridad** (14), y la imagen venía con `CONFIG_TIMESLICE_SIZE=0`, que en Zephyr
significa que dos hilos de igual prioridad no se rotan nunca: el que está
corriendo sigue corriendo. En FM el camino del modo se bloquea en algún punto y
la UI recupera el CPU; en AM el `update()` de la banda aérea no se bloquea, así
que el RTX se queda el procesador para siempre. Lo que se veía como «la radio se
cuelga en AM» era la pantalla y el teclado sin turno.

El volcado con plazo y la caja negra no eran el arreglo de esto —el arreglo es
`CONFIG_TIMESLICE_SIZE=5`: el kernel rota dos hilos de la misma prioridad, el RTX
conserva su ritmo entre turnos y la UI recibe CPU cada 5 ms como mucho—, pero sin
ellos no se habría sabido: el registro nombra al hilo parado y su estado, que es
lo que descartó el panel como culpable.

### La secuencia

La pide la UI (`state.channel.mode`) y la ejecuta el hilo de RTX, que es el dueño
del chip. `rtx_task()` ve la configuración nueva y llama a `switchMode()`
(`openrtx/src/rtx/rtx.cpp`), que hace, en este orden:

1. **vetar el PTT** y olvidar la pulsación que hubiera visto (`KeyGate.h`), para
   que el modo que se va a construir no pueda transmitir por un nivel viejo;
2. **apagar el PA y leerlo** (`forceTxOff()` → `radio_disableRtx()` y REG_33);
3. **decirle al driver el modo nuevo** (`radio_setOpmode()`): en AM escribe el
   perfil AM del BK4819 (REG_43/13/4A/47/2B/28), **instala el AGC de audio del
   camino AM** con `csk6codec_setRxProcessor(amAudioProcess)` y deja el driver
   en `OPMODE_AIRBAND`;
4. **desmontar el modo viejo**: `disable()` (que cierra su camino de audio) y el
   destructor; el bloque de la arena se devuelve;
5. **construir y habilitar el nuevo**, y **esperar a que reciba**: como mucho
   `RTX_MODE_START_MS` (300 ms) de pasadas de `update()`, hasta que el *driver*
   diga `RX`. Si no llega, se vuelve al modo anterior con una línea en el log
   (`MODE %u: not receiving after 300 ms, reverting`) y el estado vuelve con él;
   si no había modo anterior, la radio queda inactiva con el PA apagado;
6. **reabrir la puerta del PTT** sólo con una liberación posterior y una pulsación
   de al menos 25 ms (`KEYGATE_GUARD_MS`).

El modo, por su parte, abre el camino de audio cuando su squelch de portadora se
abre (`audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX)`) y lo cierra al
cerrarse. Ese camino y el del aviso de voz o el beep con el que la UI anuncia el
modo (`SOURCE_MCU -> SINK_SPK`, `PRIO_PROMPT`) **son la misma tabla**
(`openrtx/src/core/audio_path.cpp`) y los piden **dos hilos a la vez** justo en
el cambio de modo: el de RTX al abrir el receptor del modo nuevo y el de UI al
sonar el anuncio.

### La traza del post-mortem

La caja negra (docs/BLACKBOX.md) existe justo para esto, y esta vez fue ella la
que puso el dedo en el sitio. Entrando en AM desde FM, con el registro ya
escrito en flash, el arranque siguiente imprimió:

```
=== C62 post-mortem from the previous boot ===
reason: STALL detail 0x00000001 [ui]
uptime: 17264 ms (0 min 17 s), reset cause 0x00000000
radio: rtx=AM(10) ui=AM(10) flags 0x01
  rx 118000000 Hz, tx 118000000 Hz
threads:
  ui      pr 14 st pended  free  544/2048 pc 0x18047d58 lr 0x18053e15
  rtx     pr 14 st ready   free 8448/9216 pc 0x1804bc8a lr 0x1804bc69
  pcproto pr  6 st ready   free 3000/4096 pc 0x00000000 lr 0x00000000
```

`arm-none-eabi-addr2line` sobre `build_c62/zephyr/zephyr.elf` da, para los dos
valores del hilo de UI: **PC en `arch_swap`** (el marco de la conmutación, lo que
un hilo *pended* deja apilado) y **LR en `z_impl_k_sem_take`
(`kernel/sem.c:155`)**. Traducido: el hilo de UI no estaba girando en ningún
sitio, estaba **esperando un semáforo**; el estado `pended` de la tabla lo dice
igual, y su pila casi intacta (544 B libres de 2048) descarta que se hubiera
hundido en una recursión.

`k_sem_take` desde el hilo de UI acota mucho el sitio:

* los candados del audio son `k_mutex` (`boardMutex` en `audio_c62.c`,
  `audioMutex` en `csk6_codec.c`), y los dos se toman **con plazo** de 50 y
  250 ms;
* el único `k_sem` del códec (`workerWake`) lo toma su worker, no la UI;
* queda el camino de dibujo: `display_write()` → driver ST7735R del SDK →
  `spi_write_dt()` → `transceive()` → `spi_context_wait_for_completion()` →
  `spi_context_wait()` → `k_sem_take(&ctx->sync, K_FOREVER)`, y también
  `spi_context_lock()`, que es `k_sem_take(&ctx->lock, K_FOREVER)`.

Es decir: el hilo de UI quedó parkado esperando que **terminara una transferencia
del panel**, y nadie la iba a terminar.

### La causa raíz: un volcado de pantalla sin plazo

El semáforo que espera el hilo de UI lo da una sola cosa: el camino de
completado del driver SPI del CSK6, que corre en la interrupción. Y ese camino
puede no llegar nunca, por una razón que está escrita en el propio driver y en
la configuración de la build:

1. **El panel va por DMA.** `CONFIG_SPI_TX_DMA=y` y `CONFIG_SPI_RX_DMA=y`
   (`csk/drivers/spi/Kconfig.csk6`, con `select DMA`) en el `.config` de la
   build; el panel cuelga de `spi1` (`listenai,csk-spi`), y el driver arranca
   cada transferencia con el HAL del vendedor.
2. **El canal se elige en marcha.** `spi.c` del HAL pide un canal libre con
   `dma_channel_select()` sobre el DMAC del AP (seis canales, `dma0` en el DTS),
   lo configura con `dma_channel_configure()` y espera el evento de fin. Si no
   hay canal o la configuración falla, el HAL devuelve `CSK_DRIVER_ERROR`.
3. **El driver de Zephyr se come ese error.** En `spi_csk6.c`,
   `transceive()` hace `csk_spi_hal_transceive(dev);` **sin mirar el valor de
   retorno** (`spi_context_wait_for_completion()` viene justo después). El
   comentario del propio driver lo admite en el otro sitio: `/* TODO: add
   transfer error processing */`. Un arranque fallido deja
   `spi->info->status.bit.busy = 1`, no hay ENDINT, no hay
   `spi_context_complete()` y el que espera no se entera.
4. **Y el que espera espera para siempre**, porque el camino de dibujo usa el
   `K_FOREVER` de la API síncrona.

El mecanismo está instrumentado en el host (`tests/unit/display_flush.cpp`):
con un bus que submita y no completa, el camino viejo no podía volver nunca
—esperaba con `K_FOREVER`—, y con el plazo que introduce este arreglo la función
de volcado vuelve **exactamente** al vencerlo, deja el frame contado como
descartado y el bucle que dibuja sigue con la pasada siguiente (50 frames
descartados, 50 pasadas completas en el test).

Lo que **no** es: no es una disputa por el motor de DMA con el códec. Los
streams de audio del CSK6 no usan el DMAC del AP — son memoria compartida con el
DSP (`ICStream`/`AudioTrack` a través de LSF), y el DSP tiene su propio DMAC. El
otro SPI de la placa, el del BK4819, es *bit-bang* sobre GPIO (`zephyr,spi-bitbang`,
PA13/PA07), y el PWM de la retroiluminación es PWM normal del GPT, no LEDC por
DMA. El panel es el **único** usuario de ese DMAC, así que no hay canal que
robar: lo que falla es un arranque (o un completado) dentro del driver del SDK,
y el driver lo tapa.

**Por qué AM y no FM.** Nada del camino del panel distingue un modo de otro: el
volcado es el mismo en todos, y por eso el defecto no es de AM. Lo que AM aporta
es el *momento*: es el cambio de modo en el que coinciden el redibujado de la
pantalla del modo nuevo, la ráfaga de doce registros del BK4819 por el otro SPI
y la instalación del procesador de audio por muestra en el worker del códec. Es
la conmutación con más cosas pasando a la vez, y es la que el operador estaba
usando. La conclusión honesta: **el defecto es independiente del modo; AM es
donde se dio**. El contador de `diag` dirá si vuelve a darse en otro sitio.

### Lo que se arregló, pieza a pieza

| pieza | qué se hizo |
|---|---|
| `core/display_flush.c` (nuevo) | el volcado tiene **plazo**: cada transferencia se envía en asíncrono y se espera con un tiempo máximo; la que no llega se abandona, el frame se descarta y se cuenta. Ni un `K_FOREVER` en el camino de dibujo |
| `platform/drivers/display/display_c62.c` | el frame se envía desde aquí (CASET, RASET, RAMWR y los píxeles), no por `display_write()`; al vencer el plazo se suelta el bus (`spi_release()`), se marca `DISP_DROP` una vez por racha y el frame siguiente fuerza un `SPI_Control()` del driver (con la segunda `spi_config`) para que el periférico se reinicie y se le quite el `busy` al HAL |
| `keyboard_c62.c` | el teclado pasa a tener **hilo propio** (`kbd_thread`, 20 ms): un solo hilo toca la matriz, los demás leen su última pasada, y su latido entra en la caja negra (`BB_BEAT_KBD`) |
| `core/blackbox.c`, `blackbox_c62.c` | un atasco se informa **una vez**; el reinicio es PA fuera + DSP parado + reset + bucle sin dormir, y el supervisor deja de alimentar el perro guardián en cuanto hay atasco (docs/BLACKBOX.md §3.1) |
| `platform.c` | el perro guardián ya no se pide con `WDT_OPT_PAUSE_IN_SLEEP`: el último recurso no puede ser ciego a un núcleo dormido |
| `core/audio_path.cpp` | un mutex para toda la tabla; `open()`/`close()` **fuera** del candado (el driver puede tardar 250 ms); `routes.at()` sustituido por búsquedas con comprobación, porque sin excepciones un `at()` fallido aborta la radio |
| `platform/drivers/audio/audio_c62.c` | `boardMutex` con plazo (`BOARD_LOCK_TIMEOUT_MS`, 50 ms) en vez de `K_FOREVER`; la ruta sólo se marca abierta si el códec la aplicó, así que un fallo se reintenta en el siguiente flanco de squelch |
| `platform/drivers/audio/csk6_codec.c` | el candado del códec se toma con plazo (`CSK6_LOCK_TIMEOUT_MS`, 250 ms) en `setRoute`, `start`, `data`, `sync`, `halt` y `stop`: un DSP que no contesta cuesta esa ruta o ese stream, nunca el hilo que llama. `csk6codec_setRoute()` devuelve el resultado |
| `rtx/rtx.cpp` | la conmutación tiene plazo y es reversible: si el modo nuevo no recibe en `RTX_MODE_START_MS`, se vuelve al anterior y se dice en el log |
| `core/threads.c` | el perro guardián se alimenta **sólo mientras el hilo de UI avanza** (`UI_THREAD_STALL_MS`, 3 s): un hilo de UI parado deja de alimentarlo y la radio se reinicia en recepción en vez de quedarse tiesta |

### Qué se ve en `diag` cuando se descarta un frame

El camino de la pantalla publica sus contadores en `DBG_INFO`
(docs/PC_PROTO.md, tags `DISP_*`), así que la pregunta «¿se congeló la pantalla
o se colgó la radio?» se contesta por el cable:

```
$ tools/cps/cps.py -d /dev/ttyUSB0 diag
...
screen      4128 frames, 0 dropped, last 17 ms, worst 24 ms
```

Con la transferencia perdida, la misma línea dice cuántos frames se tiraron y
cuál fue el primero que no llegó:

```
screen      97 frames, 1184 dropped, last 16 ms, worst 16 ms
            last loss: chunk 5, 320 bytes of pixel data
```

`chunk` es la posición dentro del frame (0 CASET, 1 sus parámetros, 2 RASET,
3 los suyos, 4 RAMWR, 5 los píxeles) y el tamaño dice si se perdió un comando o
la transferencia grande. En el registro de la caja negra, la primera racha de
descartes deja además una migaja `DISP_DROP`, de modo que el post-mortem de un
cuelgue posterior enseña si la pantalla ya estaba fallando antes.

Lo que **no** cambia con un frame descartado: el hilo de UI sigue su pasada
(teclas, FSM, reloj), el receptor sigue, y el frame siguiente se intenta. La
pantalla se queda con lo último que llegó; la radio no.

### El primer diagnóstico, corregido

La primera versión de esta sección culpaba a la tabla de rutas de audio, que
efectivamente no tenía candado (`activePaths`, `routes`, `pathCounter` bajo dos
hilos) y que en el host corrompía el `std::map` **6 de 6 veces**. Ese defecto era
real y está arreglado, pero **no era este**: un hilo girando dentro de una
búsqueda de libstdc++ no deja el marco de `arch_swap` ni un `pended` en la tabla
de hilos, y su pila estaría llena de marcos de la búsqueda. La traza de la radio
dice semáforo, y el semáforo es el del panel. Las dos cosas convivían en el mismo
cambio de modo; la que produjo este registro es la segunda.

### Límites, dichos sin adornos

- **El error del driver del SDK sigue ahí.** `csk_spi_hal_transceive()` devuelve
  un código que `transceive()` tira, y el HAL no deja rastro de por qué no
  arrancó la transferencia. No se ha parcheado a ciegas: se ha acotado. Lo que
  se arregla aquí es la consecuencia (el hilo no espera), no la causa última;
  los contadores de `diag` y la migaja son la instrumentación que la próxima vez
  dirá cuál de los dos caminos del HAL falló y con qué transferencia.
- **Una transferencia perdida puede costar el frame y algo más.** La
  recuperación (segunda `spi_config` → `SPI_Control`) reinicia el periférico y le
  quita el `busy` al HAL; si aun así el panel no vuelve, la pantalla se queda
  congelada hasta el siguiente reinicio. La radio sigue usable y `diag` lo dice:
  es el intercambio acordado, pantalla perdida antes que radio perdida.
- **El plazo son 100 ms por transferencia** (`DISP_FLUSH_TIMEOUT_MS`). Un frame
  completo a 20 MHz son ~16 ms, así que el plazo es seis veces el caso normal:
  un bus lento no cuenta descartes. Con el bus atascado, cada frame cuesta el
  plazo entero, y el bucle de UI baja de 40 a ~10 vueltas por segundo mientras
  dure; las teclas siguen entrando porque el escaneo es de otro hilo.
- **El arranque del panel sigue siendo síncrono.** `display_blanking_off()`
  ejecuta la secuencia de init del driver del SDK con la API síncrona, en el
  arranque, cuando no hay nada más en el bus. Si se atascara ahí la radio no
  arrancaría; es el riesgo que ya existía y no lo cubre este arreglo.
- **La prueba es de host y de una radio.** El mecanismo está demostrado en el
  host y la traza es de la radio; lo que falta por medir en hardware está en la
  sección 7.

### Cómo reproducirlo

En la radio: FM en un VFO con una señal presente (por ejemplo 145,500 MHz con un
repetidor), tecla lateral y `5` para recorrer los modos hasta «AM», con el nivel
de avisos de voz en beep o superior. Antes de este arreglo: la pantalla se queda
como está, ninguna tecla responde, y `status`/`diag` por el cable siguen
contestando con el modo en `airband` y el receptor funcionando. Después: o no
pasa nada, o `diag` enseña descartes en la línea `screen` y el resto de la radio
sigue respondiendo.

En el host, sin radio:

```bash
meson setup build_linux && meson compile -C build_linux
./build_linux/audio_path_test        # la tabla, con dos hilos
./build_linux/mode_switch_am_test    # FM -> AM -> FM, y el modo que no arranca
./build_linux/display_flush_test     # un bus que no completa: frame fuera, hilo vivo
./build_linux/blackbox_hang_test     # registro, reinicio y un solo aviso
```

## Referencias

- Lista de registros del BK4819 V3: `datasheets/BK4819V3_Registers_List_20201218.pdf`
  (REG_2B, REG_28, REG_43, REG_47, REG_48, REG_78, REG_7E).
- Firmware de fábrica de la C62, `radio_rx_tune()` en `0x18019648`: AM en
  100-134 MHz con REG_47 = 0x6740, REG_43 = 0x0048, REG_13 = 0x03FF,
  REG_4A = 0x5448 — reconstruido en `forks/c62-analysis/reconstructed/radio/bk4819.c`
  y resumido en `forks/c62-analysis/stock-firmware-report.md` §«RX».
- AGC del BK4819 medido por la comunidad del UV-K5, con los umbrales de REG_49 y
  el comportamiento en AM: <https://github.com/egzumer/uv-k5-firmware-custom/wiki/Development-notes>.
- Valores de REG_43 y de la selección de AF del firmware del UV-K5:
  <https://github.com/DualTachyon/uv-k5-firmware/blob/6f8afac8864e0349ecf8b10f91ce8f69273013db/driver/bk4819.c>.
- 8,33 kHz: EUROCONTROL, *8.33 kHz Voice Channel Spacing* (el espaciado de
  25/3 kHz y la regla de designadores con 00/05/10 en cada hueco de 25 kHz);
  ICAO Annex 10 Vol. III para la banda y la modulación DSB-FC.
