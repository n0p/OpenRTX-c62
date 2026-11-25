# Cobertura del tour del simulador

Auditoría hecha por código, no a ojo: la lista de lo que el firmware tiene con
pantalla sale de sus propias tablas (`uiModes[]`, `menu_items[]`,
`settings_items[]`, el enum `uiScreen` y las tablas de quick menu de cada modo),
y se cruza con lo que el tour visita de verdad (los nombres de pantalla del
volcado, `dump json`) y con lo que el propio `tests/unit/sim_coverage.cpp`
alcanza pulsando teclas.

Fecha: 25 de noviembre de 2026. Rama `c62/simcov`, base `c62/all-fixes`
(b836434a).

## Cifras

| | antes | después |
|---|---|---|
| capturas del tour | 194 | 282 |
| pantallas distintas visitadas | **32** | **40** |
| pantallas declaradas | ninguna lista | 40 visitadas + 5 inalcanzables |
| modos recorridos uno por uno | 11 (popup) / 1 (pantalla+página) | 11 (pantalla, popup, página, salida) |
| filas de quick menu declaradas | 0 | 117 (11 modos) |
| filas del menú superior | 9 (visitadas) | 9 (declaradas y verificadas) |
| páginas de Settings | 18 (visitadas, 2 filas) | 18 (declaradas; las de modo, fila a fila) |
| test de garantía | no existía | `Simulator Coverage` (4 casos) |

El inventario del firmware en esta build (la del simulador, con
`CONFIG_APRS`, `CONFIG_KISS`, `CONFIG_DMR_TX`, `CONFIG_KEY_BACKLIGHT`,
`CONFIG_RX_FINE_OFFSET` y `UI_TEST_*`): **11 modos, 117 filas de quick menu, 9
filas de menú superior, 18 páginas de Settings, 45 nombres en `enum uiScreen`**.

## Qué se visita, módulo a módulo

| módulo / grupo | pantallas | qué hace el tour |
|---|---|---|
| principal | `MAIN_VFO`, `MAIN_VFO_INPUT`, `MAIN_MEM` | paso de VFO, entrada de frecuencia (teclear, aplicar, cancelar), pantalla de memoria tras sintonizar un canal |
| menú superior | `MENU_TOP` | las 9 filas dibujadas contra la declaración, y verde en cada una |
| menú superior → codeplug | `MENU_BANK`, `MENU_CHANNEL` | abren y están vacías en el host (no hay imagen de codeplug): se capturan como vacías y se dice |
| menú superior → zonas | `MENU_ZONES` | lista, entradas de la zona R, sintonizar la cuarta (145.675) |
| menú superior → contactos | `MENU_CONTACTS` | agenda vacía, alta (P1), edición de la llamada, guardar (1 contacto), reabrir |
| menú superior → mensajes | `MENU_MESSAGES`, `MSG_SENT`, `MSG_VIEW`, `MSG_COMPOSE`, `MSG_STATUS` | bandeja con un mensaje inyectado, abrir, responder, teclear en TXT, SEND, página de entrega, carpeta de enviados, compositor desde el popup |
| menú superior → GPS / Info / About | `MENU_GPS`, `MENU_INFO`, `MENU_ABOUT` | abren, se recorren y vuelven |
| Settings | 18 páginas (`SETTINGS_*`) | lista completa contra la declaración; dos filas en cada página; Brillo y Squelch cambian de valor; el reloj entra en su pantalla de dígitos (`SETTINGS_TIMEDATE_SET`) |
| Settings → modo | `SETTINGS_MODE`, `SETTINGS_M17`, `SETTINGS_TETRA`, `SETTINGS_FM` | las 11 páginas, todas las filas, un valor cambiado en cada una |
| modos | 11 pantallas principales + 11 popups | uno por uno en orden de registro: pantalla, popup (filas declaradas, todas seleccionables, valor editado), página, salida |
| herramientas (P1 mantenido) | `TOOLS_MENU` | cinco filas recorridas y comprobadas, cursor con wrap y P1/P2, submenú de modos con el TX/RX de los once, aplicar y volver |
| banco de canales | (módulo `channels`, sin pantalla propia) | lista vacía, alta, editar frecuencia, guardar, sintonizar, reabrir |
| APRS | `MENU_APRS`, `APRS_DETAIL`, `APRS_MSGS`, `APRS_MSG_VIEW`, `APRS_MSG_EDIT`, `APRS_CFG` | estación inyectada en la lista, detalle, P2 siguiente, bandeja APRS, mensaje, editor de respuesta, página de configuración con un campo cambiado |
| módem de paquetes | `PKT_CFG`, `PKT_STATUS` | página de configuración (Enable cambia), fila de contadores, página de contadores |
| analizador | `SPECTRUM` | traza, popup propio con todas sus filas, salida |
| batería baja | `LOW_BAT` | `vbat` a 1,0 V, espera, pantalla de emergencia, tecla para salir |

## Huecos encontrados (13 pantallas)

### Inalcanzables con las teclas (5) — declaradas, no visitadas

| pantalla | por qué |
|---|---|
| `MODE_VFO`, `MODE_MEM` | resto del par VFO/memoria: nada escribe `ui_screen` con esos valores; las pantallas principales son `MAIN_VFO`/`MAIN_MEM` |
| `MENU_BACKUP_RESTORE` | el menú de copia/restauración de flash: ninguna tecla ni fila lo abre (no hay fila en `menu_items[]`) |
| `MENU_BACKUP`, `MENU_RESTORE` | sólo se llega desde `MENU_BACKUP_RESTORE` |

`tests/unit/sim_coverage.cpp` las recorre desde `MAIN_VFO` pulsando todas las
teclas y comprueba que siguen sin alcanzarse; si alguien añade un camino, el
test falla y obliga a añadirlas al tour.

### Alcanzables y sin cubrir (8) — cerradas

| pantalla | cómo se llega | qué hacía falta |
|---|---|---|
| `SETTINGS_TIMEDATE_SET` | verde en la página Reloj | escribir dígitos en el reloj |
| `APRS_DETAIL` | verde sobre una estación | una estación recibida |
| `APRS_MSGS` | P1 en la lista de estaciones | un mensaje APRS recibido |
| `APRS_MSG_VIEW` | verde sobre el mensaje | idem |
| `APRS_MSG_EDIT` | verde otra vez (responder) | idem |
| `MSG_VIEW` | verde sobre un mensaje de la bandeja | un mensaje en el almacén |
| `MSG_STATUS` | verde en SEND del compositor | escribir texto y enviar |
| `LOW_BAT` | batería a 0 y un evento que no sea una tecla | `vbat` bajo y esperar |

Las cuatro de APRS y las dos de mensajería no tenían forma de llegar desde el
canal de control: el banco, la agenda y el buzón arrancaban vacíos y no había
manera de meter un paquete. Se añadió el comando `inject` (sólo build de host):

```
inject aprs-station <CALL> <LAT> <LON> <COMENTARIO...>
inject aprs-message <DE> <TEXTO...>
inject message <ORIGEN> <TEXTO...>
```

Los dos primeros construyen una trama AX.25 UI y la entregan a
`aprs_sim_rxFrame()`, que la mete por el mismo camino que el demodulador
(`onFrame`): se descarta lo propio, se actualiza la lista de estaciones, el
mensaje dirigido a nuestra llamada cae en el buzón y se confirma. No se escribe
nada en la UI: las pantallas enseñan lo que el servicio tiene de verdad.

### Modos: lo que faltaba por modo

Antes, `phase_quick_menu` recorría el popup de los 11 modos (filas
seleccionables + un valor editado) y `phase_modes` sólo hacía el ciclo
completo con la pantalla principal; la pantalla **y** la página de Settings de
un modo se recorrían en profundidad para AM únicamente. `phase_settings`
entraba en las 10 páginas de modo pero sólo bajaba dos filas y en M17/FM/TETRA
no recorría todas las filas de la página escrita a mano.

Ahora `phase_mode()` hace, para cada uno de los 11 modos, en el orden del
registro:

1. pantalla principal en ese modo (y que la etiqueta del modo se dibuje);
2. popup de P2: la tabla **declarada** fila a fila, todas seleccionables, y un
   valor editado por tabla (en FM, además, todas las filas editables, en
   estricto);
3. página de Settings del modo (`SETTINGS_MODE` para los 8 genéricos,
   `SETTINGS_M17`, `SETTINGS_TETRA` y `SETTINGS_FM` para los escritos a mano):
   todas las filas, y un valor cambiado;
4. salida a la pantalla principal, mismo modo y la pantalla sigue respondiendo.

Y un ciclo completo de la tecla lateral tiene que devolver exactamente la
secuencia del registro (`FM → M17 → … → WFM → FM`), no sólo volver a FM.

## DMR transmite

El `openrtx_c62_sim` se compila con `-DCONFIG_DMR_TX` (sólo ese ejecutable, no
`linux_default_def`), que es lo que hace la imagen de la C62: el modo sale como
**TX** en el submenú de modos y el PTT no está bloqueado. Como el host no tiene
basebanda, la diferencia visible está en la entrada de frecuencia:

- DMR pide **Rx y después Tx** y conserva las dos frecuencias (`430.500` rx /
  `431.000` tx en el tour);
- TETRA (RX-only) no dibuja el campo Tx y aplica al octavo dígito.

Eso es lo que recorre `phase_dmr_tx()`, y el submenú de modos comprueba el
TX/RX de los once. Que el PTT keye la PA no se puede ver en el simulador: no
hay árbol de transmisión en el host (ver `docs/SIMULATOR.md`).

## Determinismo

- El tour se da su propio `XDG_STATE_HOME` y su propio `OPENRTX_SIM_STORE` en
  un directorio temporal nuevo por ejecución, y ahora **afirma** que arranca
  vacío: 0 canales, 0 contactos, sin canal de memoria y sin nombre.
- El `ch#0 "\x14g"` de los volcados no era el almacén compartido: era
  `cps_getDefaultChannel()` devolviendo una `channel_t` **sin inicializar**
  (sólo rellenaba modo, ancho de banda, potencia y frecuencias). El nombre (y
  el resto de la unión) era basura del stack. Se corrige con un `memset` en
  `openrtx/src/core/cps.c`; cualquier radio sin registro de VFO en la NVM
  arrancaba con un nombre así.

Comprobación medida: dos ejecuciones completas del tour con el mismo binario y
el código final (dos directorios temporales distintos, horas distintas) dan
**281 de 282 volcados byte a byte idénticos** una vez enmascarada la hora del
reloj. El único que difiere es `messages-status`, y no es del almacén: el estado
de entrega (`QUEUED, try 2 of 3` frente a `FAIL, try 3 of 3`) lo mueve el
temporizador de reintentos del transporte, que cuenta tiempo de pared. El tour
no lo afirma (comprueba la cabecera, el destinatario) precisamente por eso.

## La garantía automática

`tests/unit/sim_coverage.cpp` (objetivo meson `sim_coverage_test`, test
`Simulator Coverage`) se compila con **los mismos `-D` que el simulador** y
compara la declaración del tour (`tour.py --coverage-manifest`, que meson
genera) con:

1. `uiModes[]`: número, orden, título, TX/RX, página de Settings (abriéndola de
   verdad) y etiquetas de las filas de cada quick menu;
2. `menu_items[]` y `settings_items[]`;
3. `enum uiScreen` vía `ui_dump_screenName()`: toda pantalla de la build está
   visitada o declarada inalcanzable, y no hay nombres declarados que el enum
   no tenga;
4. un recorrido en anchura del FSM real (todas las teclas desde todos los
   estados, con los almacenes sembrados como los siembra el tour): **37 de las
   40** pantallas se alcanzan pulsando teclas y todas están en la lista del
   tour. Las tres que el recorrido no alcanza necesitan texto tecleado
   (`MSG_STATUS`), un segundo verde sobre un mensaje cuyo estado interno no
   viaja en la identidad del estado (`APRS_MSG_EDIT`) o un evento de batería
   (`LOW_BAT`); las tres las recorre el tour.

Añadir un modo, una fila o una pantalla sin tocar `tour.py` rompe el test. Está
demostrado con tres cambios de un minuto sobre el árbol, compilando y lanzando
el test (`SIM_TOUR_COVERAGE=… ./build_linux/sim_coverage_test`), y revirtiendo
después:

| cambio temporal | lo que dice el test |
|---|---|
| una entrada más en `uiModes[]` (`QM_MODE(..., "WFM2", ...)`) | `REQUIRE(d.modes.size() == ui_mode_count())` → **`11 == 12`** |
| una fila más en `qm_items_fm[]` (`QM_ITEM_INFO("DEMO", ...)`) | `REQUIRE((size_t)m->count == want.rows.size())` → **`16 == 15`**, `mode 0: firmware "FM", tour "FM"` |
| un valor más en `enum uiScreen` (`SCREEN_DEMO`, con su nombre en `ui_dump_screenName`) | `screen SCREEN_DEMO is in the enum but neither visited nor declared unreachable by the tour` → `false` |

El recorrido de teclas recorre el espacio de estados hasta 40 000 estados
(2,7 s) y alcanza **37 de las 40** pantallas; las tres que no necesitan texto
tecleado (`MSG_STATUS`), un segundo verde sobre un mensaje cuyo estado interno
no viaja en la identidad del estado (`APRS_MSG_EDIT`) o un evento de batería
(`LOW_BAT`), y las tres las recorre el tour. El recorrido no está pensado para
sustituir al tour: está para que una pantalla nueva *alcanzable* no se quede
fuera de la lista sin que nadie lo note.

## Lo que sigue sin cubrir, y por qué

- `MENU_BANK` y `MENU_CHANNEL` (Banks y Channels del menú superior) leen el
  codeplug clásico, que en el host no tiene imagen: abren y no dibujan nada.
  El tour las captura con `allow_blank` y lo dice.
- El PTT no keya en el host (no hay basebanda): las pantallas de transmisión se
  cubren por lo que la UI dibuja (marca TX, entrada Rx/Tx), no por el aire.
- Los modos digitales no demodulan en el host: sus pantallas se recorren con el
  módulo de estado (`platform/targets/linux/emulator/ui_mode_status.c`), que
  informa "sin sincronismo", igual que antes.
