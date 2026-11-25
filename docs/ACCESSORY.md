# El puerto de accesorio: un pad, tres modos

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cuenta por qué el pulsador del microaltavoz y el cable del PC no pueden convivir
como estaban, qué se comparte exactamente, y qué hacen los tres modos que el
operador elige en **Ajustes ▸ Radio ▸ Puerto PC** (`Auto` / `Always` / `Never`).
El registro de ajustes que lo guarda está en `docs/SETTINGS.md`; los campos que
lo enseñan al PC, en `docs/PC_PROTO.md`.

---

## 1. Qué se comparte, y por qué

En la C62 el conector de accesorio y el de programación son **el mismo jack**, y
su contacto de PTT es el mismo pad que la recepción del puerto serie:

| señal | pad | función del pad |
|---|---|---|
| PTT del microaltavoz (activo a bajo, pull-up) | **GPIOA_15** | `UART2_RXD` (ALTER3) en todas las builds |
| Datos del radio al PC (log, protocolo) | GPIOA_18 | `UART2_TXD` (ALTER3) — **no se comparte** |
| Altavoz del accesorio | contacto analógico del jack | siempre |
| Micrófono del accesorio | contacto analógico del jack | siempre |

El altavoz y el micrófono son contactos analógicos del jack y funcionan siempre:
lo que se pisa es **solo el pulsador**. `&uart2` de `c62.dts` lleva
`pinctrl-0 = <&pinctrl_uart2_rx_default &pinctrl_uart2_tx_default>`, y
`pinctrl_uart2_rx_default` (`c62_pinctrl.dtsi`) declara el pad como recepción de
la consola; el firmware lo remuxa según el modo en vigor (`Never` lo devuelve a
GPIO). Con el pad en RX, el pulsador del accesorio enchufado pone la línea a
nivel bajo cada vez que se aprieta.

El problema es eléctrico y no tiene vuelta: un pulsador cerrado y el bit de
inicio de un byte son **el mismo nivel bajo**. Lo único que los distingue es
cuánto dura:

| lo que baja la línea | cuánto dura el nivel bajo | qué ve el receptor |
|---|---|---|
| byte `0x00` a 115200 Bd (inicio + 8 bits de datos + *stop*) | ~78 µs de los 87 del carácter | carácter **correcto**: tiene *stop* |
| trama normal (media de bytes) | decenas de µs por byte, con paradas entre medias | caracteres correctos |
| *break* corto del host (al cerrar el puerto) | 1-3 ms | `FE`/`BI` durante 1-3 ms |
| dedo en el pulsador | decenas de ms como mínimo | `FE`/`BI` en todos los caracteres |

De ahí la regla del modo `Auto`: **la línea sostenida baja más de 30 ms =
pulsación** (`PORT_PTT_HOLD_MS`, `core/port_ptt.h`). 30 ms a 115200 Bd son 345
caracteres: ninguna trama del protocolo puede mantener la línea así, y la
pulsación humana más corta es un orden de magnitud mayor.

### La fuente del nivel: el receptor, no el GPIO

La primera versión de este módulo leía el nivel con `gpio_pin_get_dt()` sobre
el pad muxado a `UART2_RXD`, dando por hecho que la entrada del bloque GPIO
sigue conectada a la línea con la función en ALTER3. **No lo está.** Medido en
hardware con esa build: con el cable del PC puesto y la línea en reposo (alta),
el `DATAIN` de `GPIOA_15` se lee **bajo desde el arranque**, así que el detector
daba la pulsación por hecha a los 30 ms, la radio se ponía a transmitir (LED
rojo encendido, sin RF porque no había frecuencia de TX) y el transporte tiraba
todos los bytes del cliente: ni un `HELLO` contestado.

Que la línea estaba alta no es una suposición: el mismo cable y el mismo jack
llevaban el protocolo en las builds anteriores, y un TX de UART en reposo está
alto por definición. La conclusión es que el `DATAIN` no refleja el pad cuando
la función del pad es la UART, y por eso **el detector no usa el GPIO**: usa el
estado de línea que el propio receptor asigna a cada carácter que entrama:

| bit del `LSR` | qué significa | ¿es prueba de línea baja? |
|---|---|---|
| `FE` (bit 3) | el *stop* se muestreó bajo: la línea estuvo baja desde el bit de inicio hasta el de *stop* | **sí** |
| `BI` (bit 4) | la línea siguió baja durante todo el carácter | **sí** |
| ninguno | carácter correcto, `0x00` incluido | no |

Es la misma distinción que antes, pero medida donde la línea de verdad se ve, y
con una ventaja: un chorro de `0x00` ya no puede imitar una pulsación, porque
son caracteres correctos y no cuentan. El nivel de reposo depende del *pull-up*
del pad (ver abajo): sin él la línea flota y el receptor entrama basura.

## 2. Los tres modos

Se eligen en **Ajustes ▸ Radio ▸ Puerto PC** (la fila va detrás de `TX Power`),
se guardan en `settings_t.pc_port` (`enum portMode`,
`openrtx/include/core/settings.h`) y **se aplican al instante**: el transporte del
protocolo lee el ajuste en cada pasada de su bucle (1 ms) y remuxa el pad, sin
reiniciar. El valor por defecto es `Auto`.

| modo | pad | protocolo serie | pulsador del microaltavoz |
|---|---|---|---|
| **Auto** (por defecto) | UART2 RXD, leído por el receptor (`FE`/`BI`) | disponible | sí, con la regla de los 30 ms |
| **Always** | UART2 RXD | disponible, siempre | **no** — es el comportamiento de todas las builds anteriores a este ajuste |
| **Never** | GPIO puro con pull-up | **no disponible** | sí, 100 % fiable (el nivel, sin regla) |

### El nivel de reposo: el *pull-up* del pad

Una línea sin nada enchufado tiene que leerse **alta**, y eso no lo da el
conector de función sino el registro de modo del pad: `GPIOA_15` lleva
`PULL_UP` en el devicetree (`button_ptt`), y `c62_portPadToUart()` lo vuelve a
afirmar **después** de muxar el pad a ALTER3, con
`IOMuxManager_ModeConfigure(..., HAL_IOMUX_PULLUP_MODE)`. Los bits de modo
(`PULL_DN`/`PULL_UP`/`PULL_FRC`, bits 16..18) comparten registro con el selector
de función (bits 0..4) y `IOMuxManager_PinConfigure()` sólo reescribe 0..4, así
que el *pull-up* sobrevive; se afirma el último por nombre para que el nivel de
reposo no dependa de que una escritura ajena no lo pise. El binding de pinctrl
del CSK6 sólo lleva el selector de función, de ahí que el *pull-up* esté en
código y no en el `.dtsi` (que sí declara ya el pad, `pinctrl_uart2_rx_default`).

Sin ese *pull-up* la línea flota: el receptor entrama basura y el detector ve
`FE`/`BI` que nadie provocó. Con él, en reposo no llega ni un carácter.

### El arranque: sin liberación no hay pulsación

Una pulsación se declara sólo si la línea **se ha visto liberada al menos una
vez** desde que el detector se resetea (arranque, o cambio de dueño del pad).
Si al arrancar la línea ya está baja —el pulsador sujeto, un cable roto, un
corto a masa, un pad sin *pull-up*— ese estado es **desconocido**, no una
pulsación: no se cuenta el tiempo, no se clavea y **no se descarta nada del
protocolo**. Es la misma regla que aplica `rtx/KeyGate.h` a los flancos: sin
flanco de liberación no hay concesión. En el log sale una vez:

```
port: PTT line held at start-up (uart break, gpio 0): unknown, ignored until it is released
```

y el detector empieza a contar en cuanto la línea se libere y se vuelva a bajar.

Con `Always` el menú lo avisa mientras se está eligiendo el valor: la fila
enseña `Always, no PTT`. Con `Never` el protocolo no puede funcionar porque su
línea de recepción ya no está conectada al UART; el transporte cierra la sesión
y devuelve el log a la consola, de modo que el terminal serie sigue viendo el
firmware como siempre (solo deja de responder a tramas).

`Never` es la elección para quien use el radio con microaltavoz y no quiera
saber nada del CPS. `Always` es la elección para un banco de pruebas o un CPS
permanente, donde el pulsador del accesorio no se usa. `Auto` es lo que quiere
casi todo el mundo.

## 3. Qué cuesta `Auto`: las tramas que llegan durante la pulsación

Mientras el pulsador mantiene la línea a masa, el UART no puede distinguir nada:
lo que entrega son caracteres con `FE`/`BI`. El transporte, en esa ventana:

1. **descarta** todo lo que el UART haya recibido (los caracteres se leen y se
   tiran, no se parsean),
2. cuenta los caracteres y los errores (*overrun*, *framing*, *break*) que
   aparecieron en esa ventana,
3. **aborta la media trama** que el parser tuviera a medias
   (`pc_proto_rxResync()`), para que el trozo de antes y el de después no se
   unan nunca en una trama que nadie envió, y
4. al soltar, vuelve a atender con normalidad y **resincroniza el tramado**: el
   parser espera al siguiente delimitador de fin y sigue.

Consecuencia práctica: **el cliente pierde las tramas que envió durante la
pulsación** y no recibe respuesta a ellas. No se pierde la sesión: si el cliente
sigue hablando, la siguiente trama se contesta normalmente (verificado en
`tests/unit/port_mode.cpp` con tramas HELLO intercaladas).

Cuánto se perdió se puede leer en tres sitios:

| dónde | qué |
|---|---|
| `PC_TAG_PORT_LOST` en `status` y en `DBG_INFO` | bytes y errores descartados, acumulados desde el arranque |
| `PC_TAG_PORT_MODE` / `PC_TAG_PORT_PTT` | modo en vigor y si el pulsador está abajo ahora mismo |
| log del firmware | `port: accessory PTT down, UART discarded until release` y, al soltar, `held Nms, M bytes/E errors dropped (T since boot)` |
| línea `pcproto: STALLED ...` | añade `lost=` y `mode=` para saber si el silencio lo causó el pulsador |

Un cliente que vea crecer `portLost` mientras él no ha cambiado nada ya sabe que
fueron sus propias tramas las que se tiraron, en vez de culpar al cable.

### Límites conocidos

- **El umbral es una ventana de 30 ms**: los caracteres que lleguen en esos
  primeros 30 ms de la pulsación sí se parsean (y casi siempre se caen solos, por
  CRC). A partir de ahí no se parsea nada.
- **Un cable roto o un corto a masa en el pad** parece una pulsación sostenida
  *si la línea se había liberado antes* (o sea: en marcha). Al arrancar, ese
  mismo estado es desconocido y se ignora. Si acaba pareciendo pulsación, el
  radio transmitirá mientras dure (hasta el corte de 90 s de aire de
  `rtx/TxWatch.h`, `docs/TX_SAFETY.md`).
- **El detector no clavea**: entrega un nivel a `platform_getPttStatus()` como lo
  haría el pulsador físico, y la concesión la dan `rtx_keyingAllowed()` y
  `rtx/KeyGate.h` (una liberación desde el último cambio de modo, 25 ms de
  pulsación, guarda de 150 ms). En `Always` y en `Never` el detector no
  interviene: `port_ptt_plan()` dice quién manda y `platform_getPttStatus()` lo
  respeta.
- **La fuente es el bucle del transporte**, que es el único contexto que puede
  consumir la FIFO del UART: se muestrea cada 1 ms porque el transporte corre a
  1 ms. Si ese hilo se para, el `Auto` se queda sin pulsador además de sin
  protocolo, y la supervisión lo dice (`pcproto: STALLED`, con `rx=`, `lost=` y
  `mode=`). Leer el pad desde otro hilo robaría bytes al cliente, así que no se
  hace: `platform_getPttStatus()` sólo lee el veredicto. En `Never` no depende de
  nadie: es el nivel del GPIO.
- **Builds que ya se quedaban el pad** (`C62_REMOTE`, `C62_KISS_UART`) siguen
  mandando: muxan GPIOA_15 a UART RX ellos mismos y este ajuste no los toca. El
  menú lo sigue enseñando, pero no tiene efecto en esas builds. En una build sin
  protocolo PC (`C62_PC_PROTO` apagado) nadie muxa el pad, así que el `Auto` usa
  el nivel del GPIO como prueba, con la misma regla de los 30 ms.

## 4. Cómo se comporta con el CPS conectado

- Con `Auto`, el CPS funciona y el microaltavoz también. Mientras el operador no
  apriete, todo son tramas; mientras apriete, el CPS ve silencio y sus peticiones
  de esa ventana se pierden. El CPS puede reintentar (la radio sigue viva: el
  `status` sigue contestando en cuanto se suelta).
- Con `Always`, el CPS es dueño del pad: el pulsador del microaltavoz no hace
  nada. Es el modo de un banco de pruebas.
- Con `Never`, el CPS **no puede hablar** con el radio: no hay HELLO que
  contestar porque la línea no llega al UART. El log sigue saliendo por TXD
  (GPIOA_18), así que el terminal serie ve el arranque y los avisos como
  siempre.
- Si se cambia el modo desde el CPS (`PC_TAG_SET_PC_PORT`, o el campo
  `pc_port` de `SETTINGS_SET`), el cambio se aplica en la siguiente pasada del
  transporte. Pasar a `Never` cierra la sesión: es el cliente el que se queda
  sin cable, por diseño.

## 5. Cómo se comprueba

**En el banco.** `DBG_INFO` (namespace de depuración, `docs/PC_PROTO.md`) lleva
`PC_TAG_PORT_MODE`, `PC_TAG_PORT_PTT` y `PC_TAG_PORT_LOST`. Con el cable puesto y
el protocolo enlazado:

1. `port_mode = 0` (auto), `port_ptt = 0`, `port_lost = 0` en reposo.
2. Aprieta el PTT del microaltavoz: `port_ptt` pasa a 1 en la siguiente lectura y
   el radio transmite con la potencia del canal.
3. Suelta: `port_ptt` vuelve a 0 y `port_lost` ha crecido con lo que el CPS
   mandara en esa ventana.

**En el log de arranque** hay una línea por modo aplicado, y dice **de dónde
saca el nivel** (la primera es el `Auto` de esta versión):

```
port: mode Auto, pad UART RX, PTT sustained break > 30 ms (uart LSR FE|BI), gpio 0
port: PTT line held at start-up (uart break, gpio 0): unknown, ignored until it is released
port: mode Always, pad UART RX, PTT not read
port: mode Never, pad GPIO, PTT button level, gpio 1
```

El `gpio N` de las dos primeras es el testigo que cerró el diagnóstico: con el
pad muxado a `UART2_RXD` y la línea en reposo, `DATAIN` se lee bajo (`gpio 0`)
mientras el receptor no entrama nada, y por eso el detector no lo usa. Si en una
unidad concreta ese número sale `1` con el cable puesto, la conclusión no cambia:
la fuente sigue siendo el receptor, que es el único camino que ve la línea de
verdad.

**En los tests de host** (`tests/unit/port_mode.cpp`, `meson test 'Accessory
Port Test'`):

- 50 ms de línea sostenida baja son una pulsación; 30 ms exactos no; un carácter
  `0x00` correcto (con *stop*) no es prueba de nada, y un *break* de 1 ms, uno de
  3 ms y un rebote tampoco.
- **Arranque con la línea baja**: 2 s de `FE`/`BI` desde el primer muestreo no
  son una pulsación (`presses == 0`, `unknown`), no cuentan ni como rebote, y el
  `HELLO` que llega en ese estado **se contesta**.
- La línea liberada una vez y luego sujeta sí clavea, y la duración de la
  pulsación se mide desde el flanco.
- Con `Auto`, HELLO antes, durante y después de una pulsación: se contesta, los
  caracteres de la ventana se descartan y se cuentan, la media trama se aborta y
  la sesión sobrevive.
- Un chorro de 120 `0x00` correctos durante 2 s no declara pulsación ni rebote.
- Las tres transiciones de modo: `Auto→Always` (el detector sale del camino y no
  clavea aunque diga pulsado), `Always→Auto` (el cambio de dueño del pad resetea
  el detector: la línea sujeta en ese momento es desconocida y hace falta una
  liberación), `Auto→Never` (el nivel del GPIO manda y el protocolo no existe).
- La tabla de `port_ptt_plan()`, `port_ptt_keyed()` y `port_ptt_held_char()`.
- La migración del registro de ajustes de la versión 7 a la 8
  (`tests/unit/settings_record.cpp`).

## 6. Coste

Medido en la imagen completa (todas las banderas de modo y `C62_PC_PROTO`,
`C62_DEBUG_PROTO`, `C62_BENCH_PROTO`, `C62_AIR_TX`), contra el commit base de la
rama (`c62/all-fixes e99172ce`), mismo juego de banderas:

| región | base | con el detector corregido | diferencia |
|---|---|---|---|
| FLASH | 793 044 B (75,63 %) | **793 340 B (75,66 %)** | +296 B |
| RAM | 180 472 B (68,84 %) | **180 472 B (68,84 %)** | 0 |
| PSRAM | 653 910 B (35,64 %) | **653 910 B (35,64 %)** | 0 |

Los 296 B son la clasificación por carácter (`uartDrain`) y las dos líneas de
log nuevas; la RAM no crece porque los dos bits de estado que añade el detector
(`armed`, `unknown`) caben en el relleno de `portPttState_t`. El detector no
añade ni un byte de PSRAM.

