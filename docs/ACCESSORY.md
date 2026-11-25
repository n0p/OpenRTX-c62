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
`pinctrl-0 = <&pinctrl_uart2_rx_default &pinctrl_uart2_tx_default>`, así que el
pad queda muxado a UART RX en todos los builds, y el pulsador del accesorio
enchufado pone la línea a nivel bajo cada vez que se aprieta.

El problema es eléctrico y no tiene vuelta: un pulsador cerrado y el bit de
inicio de un byte son **el mismo nivel bajo**. Lo único que los distingue es
cuánto dura:

| lo que baja la línea | cuánto dura el nivel bajo |
|---|---|
| byte `0x00` a 115200 Bd (inicio + 8 bits de datos) | ~78 µs |
| trama normal (media de bytes) | decenas de µs por byte, con paradas entre medias |
| *break* corto del host (al cerrar el puerto) | unos pocos ms |
| dedo en el pulsador | decenas de ms como mínimo |

De ahí la regla del modo `Auto`: **nivel bajo sostenido más de 30 ms = pulsación**
(`PORT_PTT_HOLD_MS`, `core/port_ptt.h`). 30 ms son 345 bytes de `0x00` seguidos a
115200 Bd: ninguna trama del protocolo puede mantener la línea así, y la
pulsación humana más corta es un orden de magnitud mayor.

### El pad sigue siendo UART y se lee como GPIO

El conector de función (IOMUX) elige **quién transmite** por el pad, y la entrada
del bloque GPIO sigue conectada: con el pad en `UART2_RXD` el firmware puede leer
`gpio_pin_get_dt(&button_ptt)` y ver el nivel de la línea. El *pull-up* no vive en
el selector de función sino en el registro de modo del pad
(`IOMuxManager_ModeConfigure`), así que sobrevive al cambio de función: la línea
queda alta cuando no hay nada enchufado, baja cuando el pulsador la lleva a masa,
y el UART la lee igual. Eso es lo que hace posible `Auto`.

## 2. Los tres modos

Se eligen en **Ajustes ▸ Radio ▸ Puerto PC** (la fila va detrás de `TX Power`),
se guardan en `settings_t.pc_port` (`enum portMode`,
`openrtx/include/core/settings.h`) y **se aplican al instante**: el transporte del
protocolo lee el ajuste en cada pasada de su bucle (1 ms) y remuxa el pad, sin
reiniciar. El valor por defecto es `Auto`.

| modo | pad | protocolo serie | pulsador del microaltavoz |
|---|---|---|---|
| **Auto** (por defecto) | UART2 RXD, leído también como GPIO | disponible | sí, con la regla de los 30 ms |
| **Always** | UART2 RXD | disponible, siempre | **no** — es el comportamiento de todas las builds anteriores a este ajuste |
| **Never** | GPIO puro con pull-up | **no disponible** | sí, 100 % fiable (el nivel, sin regla) |

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
lo que entrega es *break* y ceros. El transporte, en esa ventana:

1. **descarta** todo lo que el UART haya recibido (los bytes se leen y se tiran,
   no se parsean),
2. cuenta los bytes y los errores de tramado (*overrun*, paridad, *framing*,
   *break*) que aparecieron en esa ventana,
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

- **El umbral es una ventana de 30 ms**: los bytes que lleguen en esos primeros
  30 ms de la pulsación sí se parsean (y casi siempre se caen solos, por CRC). A
  partir de ahí no se parsea nada.
- **Un chorro de ~345 bytes de `0x00` seguidos parece una pulsación.** Ninguna
  trama del protocolo lo produce (los ceros se escapan), pero una herramienta
  ajena que escriba ceros a saco por el puerto sí puede simularla, y entonces el
  radio puede llegar a transmitir. Es el precio de compartir el pad; si eso
  importa, `Always`.
- **Un cable roto o un corto a masa en el pad** también parece una pulsación
  sostenida, y el radio transmitirá mientras dure (hasta el corte de 90 s de
  aire de `rtx/TxWatch.h`, `docs/TX_SAFETY.md`).
- **La resolución depende del bucle del transporte**: se muestrea cada 1 ms
  porque el transporte corre a 1 ms. Si el transporte se para, la
  supervisión lo dice (`pcproto: STALLED`), pero mientras esté parado el
  pulsador no se ve. En `Never` no depende de nadie: es el nivel del GPIO.
- **Builds que ya se quedaban el pad** (`C62_REMOTE`, `C62_KISS_UART`) siguen
  mandando: muxan GPIOA_15 a UART RX ellos mismos y este ajuste no los toca. El
  menú lo sigue enseñando, pero no tiene efecto en esas builds.

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

**En el log de arranque** hay una línea por modo aplicado:

```
port: mode Auto, pad UART RX, PTT sustained low > 30 ms
port: mode Always, pad UART RX, PTT not read
port: mode Never, pad GPIO, PTT button
```

**En los tests de host** (`tests/unit/port_mode.cpp`, `meson test 'Accessory
Port Test'`):

- 50 ms de nivel bajo cuentan como pulsación; 30 ms exactos no; un byte `0x00`,
  un *break* de 1 ms, uno de 3 ms y un rebote no cuentan.
- Con `Auto`, HELLO antes, durante y después de una pulsación: se contesta, los
  bytes de la ventana se descartan y se cuentan, la media trama se aborta y la
  sesión sobrevive.
- Los tres modos, la tabla de `port_ptt_plan()` y `port_ptt_keyed()`: `Always`
  no lee nunca el pulsador, `Never` no lee nunca el cable, `Auto` las dos cosas
  con la regla.
- La migración del registro de ajustes de la versión 7 a la 8
  (`tests/unit/settings_record.cpp`).
