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
| dedo en el pulsador | decenas de ms como mínimo | `FE`/`BI` en el primero y después **nada**: el receptor se queda esperando a que la línea suba |

De ahí la regla del modo `Auto`: **la línea sostenida baja más de 30 ms =
pulsación** (`PORT_PTT_HOLD_MS`, `core/port_ptt.h`). 30 ms a 115200 Bd son 344
caracteres: ninguna trama del protocolo puede mantener la línea así, y la
pulsación humana más corta es un orden de magnitud mayor.

### La fuente del nivel: una ventana en el pad

Con el cable puesto, lo que el receptor entrega **no basta**: con la línea sujeta
a cero por el pulsador, el receptor entrama *un* break y después no entrega nada
más (se queda esperando a que la línea suba), así que una regla alimentada por
caracteres ve un solo milisegundo de nivel bajo y nunca los 30 ms de una
pulsación. Eso es exactamente el síntoma que reportó el operador desde el banco:
**en `Auto` la radio no clavea, en `Never` sí**, con la misma radio y el mismo
cable (ver §2, "Lo que se sabe de `Auto`").

Tampoco sirve leer `DATAIN` con el pad muxado a `UART2_RXD`: la primera versión
de este módulo lo hizo y con el cable puesto y la línea en reposo ese bit se lee
**bajo desde el arranque**, así que el detector daba la pulsación por hecha a
los 30 ms, la radio se ponía a transmitir y el transporte tiraba todos los bytes
del cliente.

Lo que sí funciona es mirar el pad **como GPIO durante unos microsegundos**:
`c62_portPadSample()` muxa `GPIOA_15` a su función GPIO, lee `DATAIN` dos veces
y lo devuelve a `UART2 RXD` con el mismo valor de registro que tenía (pull-up
incluido). La ventana es de unos pocos cientos de nanosegundos a un microsegundo
—el firmware la mide con el contador de ciclos del núcleo y la imprime—, o sea
**una fracción de bit**: un bit a 115200 Bd son 8,68 µs.

| qué hace la ventana | valor | por qué |
|---|---|---|
| duración | 1 µs como mucho (`PORT_PTT_SAMPLE_WINDOW_MAX_US`) | 1/8 de bit: el receptor puede perder un bit que estaba muestreando, nunca el tramado de un carácter |
| periodo | 4 ms (`PORT_PTT_SAMPLE_PERIOD_MS`) | 250 ventanas por segundo: ciclo de trabajo 1/4000, y la pulsación se declara entre 32 y 36 ms después del flanco |
| cuándo **no** se toma | cuando el receptor está entramando caracteres **correctos** | un carácter con su *stop* demuestra que la línea estaba alta: no hay nada que mirar, y así el cliente no paga nada mientras habla |

La puerta ("no se toma ventana mientras hay caracteres correctos") no esconde la
pulsación, porque una línea sujeta a cero **no puede** producir caracteres
correctos: todo lo que entrama tiene `FE` (el *stop* muestreado bajo) o `BI`. Es
decir, el pulsador *abre* la puerta en vez de cerrarla. Y como los dos sentidos
se sostienen, la evidencia de liberación también sale gratis: dos pasadas
seguidas con caracteres correctos son prueba de que la línea está alta
(`PORT_PTT_STREAM_PASSES`), así que soltar el PTT desclavea en 4 ms como mucho
aunque el CPS esté pidiendo `DBG_INFO` en ese momento. Dos pasadas y no una,
porque una ventana tomada sobre una línea sujeta puede ser justo lo que deje al
receptor entramar **un** carácter de aspecto correcto: creerlo desclavearía una
transmisión que el operador sigue sujetando. Una sola pasada no se tira, eso sí:
**arma** el detector (`port_ptt_seen_high()`, un "alto débil": la línea no está
sujeta, pero no es prueba bastante para desclavear). Sin eso, un cliente cuyas
peticiones cayeran siempre en la pasada en la que toca ventana dejaría al
detector sin armar y el pulsador no clavearía nunca; lo encontró el test de host
de la sesión con el CPS pidiendo cada 4 ms.

Cuando una ventana toca dentro de un carácter (el cliente está hablando y la
ventana cae en medio de un byte) el receptor pierde como mucho un bit de ese
carácter: la trama se cae por CRC y el cliente la reintenta. Por eso el modo
`Auto` **no** toma ventanas mientras hay tráfico: el precio es cero en el caso
normal y una trama reintentada en el caso raro.


## 2. Los tres modos

Se eligen en **Ajustes ▸ Radio ▸ Puerto PC** (la fila va detrás de `TX Power`),
se guardan en `settings_t.pc_port` (`enum portMode`,
`openrtx/include/core/settings.h`) y **se aplican al instante**: el transporte del
protocolo lee el ajuste en cada pasada de su bucle (1 ms) y remuxa el pad, sin
reiniciar. El valor por defecto es `Never`.

**`Auto` no está en el menú.** La fila de `Puerto PC` ofrece sólo `Always` y
`Never`: `Auto` se selecciona desde el CPS (`PC_TAG_SET_PC_PORT`, o el campo
`pc_port` de `SETTINGS_SET`) y se aplica en la pasada siguiente, así que una
sesión de banco puede probarlo sin menú. Y no sobrevive a un reinicio: un
registro guardado con `Auto` se lee como `Never` al arrancar
(`platform/drivers/NVM/nvmem_C62.c`), porque hasta que el pulsador no se confirme
en hardware no puede ser el modo con el que se entrega una radio.

| modo | pad | protocolo serie | pulsador del microaltavoz |
|---|---|---|---|
| **Auto** | UART2 RXD, con una ventana GPIO de ~1 µs cada 4 ms | disponible | **sí, con la regla de 30 ms** (latencia 32-36 ms al apretar, ≤ 4 ms al soltar; pendiente de confirmar en banco) |
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
`FE`/`BI` que nadie provocó. Con él, en reposo no llega ni un carácter. La
ventana de `Auto` escribe **sólo** los bits 0..4 del registro del pad (el
selector de función) y lo restaura leyendo antes el valor que había, así que el
*pull-up* sigue en pie dentro de la ventana y después de ella.

### El arranque: sin liberación no hay pulsación

Una pulsación se declara sólo si la línea **se ha visto liberada al menos una
vez** desde que el detector se resetea (arranque, o cambio de dueño del pad).
Si al arrancar la línea ya está baja —el pulsador sujeto, un cable roto, un
corto a masa, un pad sin *pull-up*— ese estado es **desconocido**, no una
pulsación: no se cuenta el tiempo, no se clavea y **no se descarta nada del
protocolo**. Es la misma regla que aplica `rtx/KeyGate.h` a los flancos: sin
flanco de liberación no hay concesión. En el log sale una vez:

```
port: PTT line held at start-up (pad 1): unknown, ignored until it is released
```

y el detector empieza a contar en cuanto la línea se libere y se vuelva a bajar.

Con `Always` el menú lo avisa mientras se está eligiendo el valor: la fila
enseña `Always, no PTT`. Con `Never` el protocolo no puede funcionar porque su
línea de recepción ya no está conectada al UART; el transporte cierra la sesión
y devuelve el log a la consola, de modo que el terminal serie sigue viendo el
firmware como siempre (solo deja de responder a tramas).

`Never` es el valor de fábrica y la elección de quien use el radio con
microaltavoz: el pad es GPIO, el nivel se lee sin regla y no hay nada que
interpretar. `Always` es la elección para un banco de pruebas o un CPS
permanente, donde el pulsador del accesorio no se usa.

### Lo que se sabe de `Auto`, y lo que falta por medir

En el banco, con el puerto en `Auto`, el pulsador **no clavea**; en `Never` sí,
con la misma radio y el mismo cable. Las dos fuentes que se probaron antes de la
ventana fallan por motivos distintos y medidos:

1. **El `DATAIN` con el pad en la UART**: se lee bajo siempre, así que la versión
   que lo usó claveaba la radio desde el arranque y tiraba los bytes del cliente.
2. **El estado de trama del receptor (`FE`/`BI`)**: con la línea sujeta a cero el
   receptor entrama *un* break y se queda esperando a que suba, así que la regla
   de 30 ms nunca acumula. Es el síntoma exacto del operador ("no clavea en
   `Auto` y sí en `Never`") y no hace falta ninguna captura más para explicarlo:
   con la línea a cero el receptor no tiene nada que entregar.

La ventana es lo que queda por confirmar en el banco, y es una sola cosa: **que
con la función del pad en GPIO su `DATAIN` sí siga a la línea**. Es la hipótesis
del diseño —el mismo pad muxado a GPIO funciona en `Never`— pero con la UART
mirando desde el otro lado; el firmware la imprime en cada arranque:

```
port: mode Auto, pad UART RX, PTT pad sampled as GPIO > 30 ms, window <1 us every 4 ms, gpio-while-on-uart 0
port: pad window 38 cycles (~127 ns), level 1, every 4 ms
port: pad level 1 -> 0, window 36 cycles
port: accessory PTT down, UART discarded until release
port: accessory PTT up, held 51ms, 0 bytes/0 errors dropped (0 since boot), windows 12/0/0, max 152 ns
```

La segunda línea es la ventana: ciclos del núcleo (300 MHz, un ciclo son 3,3 ns)
entre la escritura que quita el pad de la UART y la que lo devuelve, y el nivel
que leyó. La mide el propio firmware con `DWT->CYCCNT` en la radio, así que en
este árbol **todavía no hay una medida de banco**: lo que sí está comprobado es
el recuento estático sobre el `objdump` de `c62_portPadSample()` —dos escrituras
al registro del pad, dos lecturas de `DATAIN` y dieciséis `nop` de asentamiento:
20 instrucciones entre las dos escrituras, dos de ellas accesos al periférico,
o sea unas decenas de ciclos, una décima de microsegundo—, que cabe de sobra en
el presupuesto de 1 µs y en el octavo de bit. Si el núcleo no tuviera contador de ciclos la línea diría
`not measured` y el nivel se leería igual: el contador no está en el camino de la
detección.

Y una advertencia sobre el arranque: si el `gpio-while-on-uart` sale `0` con el
cable puesto y la línea en reposo, eso no invalida nada, es el valor **con la
UART como dueña del pad**, que es justo el que no sirve. Lo que dice si la
ventana funciona es el `level` de la segunda línea con el pulsador apretado (`0`)
y suelto (`1`).

Si en un banco la ventana no ve el nivel, el orden de las alternativas es:
separar el PTT del RX en el cableado, o muestrear con el receptor en una ventana
de silencio forzada (bajar la línea, esperar un hueco de trama). Las dos cuestan
más que esto, y por eso `Auto` sigue fuera del menú hasta que haya una sesión de
banco que lo confirme.


## 3. Qué cuesta `Auto`: las tramas que llegan durante la pulsación

Mientras el pulsador mantiene la línea a masa, el UART no puede distinguir nada:
lo que entrega (si entrega algo) son caracteres con `FE`/`BI`. El transporte, en
esa ventana:

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

Y al revés, que es lo que la ventana arregla: **la pulsación no le cuesta nada al
cliente que no está transmitiendo en ese momento**. La puerta del muestreador deja
el pad en paz mientras el receptor entrama caracteres correctos, así que una
sesión de CPS en reposo (pidiendo `DBG_INFO` cada 250 ms) no pierde ni un byte
por el hecho de que `Auto` esté mirando el pad.

Cuánto se perdió se puede leer en tres sitios:

| dónde | qué |
|---|---|
| `PC_TAG_PORT_LOST` en `status` y en `DBG_INFO` | bytes y errores descartados, acumulados desde el arranque |
| `PC_TAG_PORT_MODE` / `PC_TAG_PORT_PTT` | modo en vigor y si el pulsador está abajo ahora mismo |
| log del firmware | `port: accessory PTT down, UART discarded until release` y, al soltar, `held Nms, M bytes/E errors dropped (T since boot), windows N/S/U, max W us` |
| línea `pcproto: STALLED ...` | añade `lost=` y `mode=` para saber si el silencio lo causó el pulsador |

Un cliente que vea crecer `portLost` mientras él no ha cambiado nada ya sabe que
fueron sus propias tramas las que se tiraron, en vez de culpar al cable.

Los tres números de `windows` en la línea de soltar son **ventanas tomadas /
ventanas no tomadas porque había tráfico / ventanas descartadas porque las dos
lecturas no coincidían**. El tercero es el único que delata un problema de
temporización del mux: si crece, el pad no se estaba asentando dentro de la
ventana y el nivel no se está usando (esa pasada cuenta como no observada).

### Límites conocidos

- **El umbral es una ventana de 30 ms**: los caracteres que lleguen en esos
  primeros 32-36 ms de la pulsación sí se parsean (y casi siempre se caen solos,
  por CRC). A partir de ahí no se parsea nada.
- **La pulsación se declara entre 32 y 36 ms** después del flanco: la regla es
  "más de 30 ms" y las muestras están a 4 ms, así que el umbral se redondea a la
  muestra siguiente (32 ms) y a eso se le suma hasta un periodo de fase (la
  primera ventana puede caer 4 ms después del flanco). El peor caso está acotado
  por `PORT_PTT_HOLD_MS + 2 * PORT_PTT_SAMPLE_PERIOD_MS`. Soltar desclavea en
  4 ms como mucho (la siguiente ventana, o dos pasadas de tráfico seguidas).
- **Mientras un cliente escribe sin parar no hay ventanas**: si el receptor está
  entregando caracteres correctos el pad no se toca. No es un problema, porque en
  ese estado la línea no está sujeta: si el operador aprieta, el tráfico se corta
  (la línea a cero no entrama caracteres correctos), la puerta se abre en la
  pasada siguiente y la pulsación se detecta.
- **Un break largo del host (> 30 ms) parece una pulsación**: es
  eléctricamente lo mismo y no hay forma de distinguirlo; un host que mantiene la
  línea a cero durante 30 ms ha hecho, a todos los efectos, lo que hace el
  pulsador. Un break corto (1-3 ms, el que produce un puerto al cerrarse) no
  puede llegar a dos ventanas seguidas y no clavea.
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
  consumir la FIFO del UART y el único que toma ventanas: el pad se mira cada
  4 ms y el bucle corre a 1 ms. Si ese hilo se para, el `Auto` se queda sin
  pulsador además de sin protocolo, y la supervisión lo dice (`pcproto:
  STALLED`, con `rx=`, `lost=` y `mode=`). Leer el pad desde otro hilo robaría
  bytes al cliente (y podría pisar una ventana a medias), así que no se hace:
  `platform_getPttStatus()` sólo lee el veredicto. En `Never` no depende de
  nadie: es el nivel del GPIO.
- **Una ventana puede costar un carácter**: si cae dentro de un byte que el
  cliente está mandando, el receptor pierde como mucho un bit y ese carácter
  llega mal; la trama se cae por CRC y el cliente la reintenta. La puerta hace
  que esto no ocurra con tráfico normal (no se toca el pad mientras hay
  caracteres correctos), pero un cliente que empiece a hablar justo en la ventana
  puede perder un byte de la primera trama.
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
2. Aprieta el PTT del microaltavoz: `port_ptt` pasa a 1 en la siguiente lectura
   (32-36 ms después del flanco) y el radio transmite con la potencia del canal.
3. Suelta: `port_ptt` vuelve a 0 en 4 ms y `port_lost` ha crecido sólo si el CPS
   mandó algo en esa ventana.

**En el log de arranque** hay una línea por modo aplicado, y dice **de dónde
saca el nivel**:

```
port: mode Auto, pad UART RX, PTT pad sampled as GPIO > 30 ms, window <1 us every 4 ms, gpio-while-on-uart 0
port: pad window 38 cycles (~127 ns), level 1, every 4 ms
port: mode Always, pad UART RX, PTT not read
port: mode Never, pad GPIO, PTT button level, gpio 1
```

El `gpio-while-on-uart` es el `DATAIN` leído **con la UART como dueña del pad**,
y es el testigo que cerró el diagnóstico anterior: sale `0` (bajo) con la línea
en reposo, mientras el receptor no entrama nada, así que ese camino no sirve. La
línea siguiente es la primera ventana de verdad: ciclos del núcleo que el pad
estuvo fuera de la UART, en microsegundos, y el nivel que leyó. Con el pulsador
apretado el `level` tiene que ser `0` y con el pulsador suelto `1`; si no cambia
nunca, la ventana no ve el pad y `Auto` no puede funcionar (ver §2).

**En los tests de host** (`tests/unit/port_mode.cpp`, `meson test 'Accessory
Port Test'`):

- 50 ms de línea sostenida baja son una pulsación; 30 ms exactos no; un carácter
  `0x00` correcto (con *stop*) no es prueba de nada, y un *break* de 1 ms, uno de
  3 ms y un rebote tampoco.
- **Arranque con la línea baja**: una línea sujeta desde el primer muestreo no es
  una pulsación (`presses == 0`, `unknown`), no cuenta ni como rebote, y el
  `HELLO` que llega después de soltar **se contesta**.
- La línea liberada una vez y luego sujeta sí clavea, y la duración de la
  pulsación se mide desde el flanco.
- Con `Auto`, HELLO antes, durante y después de una pulsación: se contesta, los
  caracteres de la ventana se descartan y se cuentan, la media trama se aborta y
  la sesión sobrevive.
- Un chorro de 120 `0x00` correctos durante 2 s no declara pulsación ni rebote.
- **El calendario de ventanas**: una ventana cada 4 ms y nunca dos en el mismo
  milisegundo (250 por segundo, no 1000); un bucle atascado 100 ms toma **una**
  ventana al volver, no una ráfaga; el reloj puede dar la vuelta.
- **La puerta**: medio segundo de tráfico continuo no toma ni una ventana; la
  primera pasada con un carácter correcto no desclavea (`WAIT`, sólo arma), la
  segunda sí (`RELEASE`); un único carácter correcto sobre una línea sujeta
  **no** desclavea; y una sesión en la que el cliente pide justo cada 4 ms (o
  sea, cada vez que toca ventana) sigue armando el detector y el pulsador sigue
  claveando.
- **Un hueco no es una línea baja larga**: dos niveles bajos separados por una
  pasada no observada no son 30 ms, y una pulsación ya declarada no la termina un
  hueco (sólo ver la línea alta).
- **La aritmética**: la ventana es 1/8 de bit como mucho a 115200 Bd, el ciclo de
  trabajo es 1/4000, la pulsación se declara a 30 + 2·4 ms como mucho, y 30 ms de
  línea sujeta son 344 caracteres.
- Las tres transiciones de modo: `Auto→Always` (el detector sale del camino y no
  clavea aunque diga pulsado), `Always→Auto` (el cambio de dueño del pad resetea
  el detector: la línea sujeta en ese momento es desconocida y hace falta una
  liberación), `Auto→Never` (el nivel del GPIO manda y el protocolo no existe).
- La tabla de `port_ptt_plan()`, `port_ptt_keyed()` y `port_ptt_held_char()`.
- La migración del registro de ajustes de la versión 7 a la 8
  (`tests/unit/settings_record.cpp`).

## 6. Coste

Medido en la imagen completa (todas las banderas de modo y `C62_PC_PROTO`,
`C62_DEBUG_PROTO`, `C62_BENCH_PROTO`, `C62_AIR_TX`), contra `c62/all-fixes
1d7d3d47` (la imagen `final-20261126k`, 816 456 B), mismo juego de banderas y
mismo contenedor:

| región | base 1d7d3d47 | con la ventana | diferencia |
|---|---|---|---|
| FLASH | 816 456 B (77,86 %) | **817 276 B (77,94 %)** | +820 B |
| RAM | 188 112 B (71,76 %) | **188 136 B (71,77 %)** | +24 B |
| PSRAM | 658 778 B (35,90 %) | **658 778 B (35,90 %)** | 0 |

Por secciones del ELF: `.text` +656 B, `.data` +4 B, `.bss` +20 B; `.ramfunc`,
`noinit`, `.dtcm_bss`, `.psram_dmr` y `psram_section` no cambian (el muestreador
no toca PSRAM).

Los +656 B de `.text` son casi todos texto: el juego de líneas de log de este
modo (la de aplicación del modo, la de la primera ventana con sus ciclos, la de
los flancos del pad y la de soltar, que ahora lleva los tres contadores de
ventanas), más el código nuevo —`c62_portPadSample()` 192 B,
`port_ptt_sample_step()` 88 B, `c62_cyclesToNs()` 18 B,
`port_ptt_sampler_reset()` 16 B, `port_ptt_seen_high()` 16 B y
`port_ptt_unobserved()` 12 B, 342 B en total—; a cambio desaparecen la línea de
testigos por pasada y la lectura del `LSR` en cada vuelta. Los +20 B de `.bss`
son `portPttSampler_t` (12 B) y los dos contadores de la ventana (8 B), y los
+4 B de `.data` son `padLevelPrior`, que arranca en −1 para que la primera
ventana no cuente como un flanco.

Nada de esto cae en el camino de audio: la ventana son 20 instrucciones cada
4 ms y, contando los accesos al periférico, unas decenas de ciclos; 250 ventanas
por segundo son ~7 000 ciclos de 300 millones, un 0,002 % de la CPU, y ni siquiera
se ejecuta mientras el cliente está transmitiendo.

