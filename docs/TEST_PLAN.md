# Plan de pruebas del firmware de la C62

**Para qué sirve este documento.** Fija qué se puede probar **sin la radio**, con
qué herramienta, y qué queda para el banco. La regla que lo ordena es la del
dueño del proyecto: *no perder tiempo con pruebas manuales en la radio*. Todo lo
que se pueda decidir en el host se decide en el host; al banco sólo va lo que
depende del aire, del silicio o de un reloj real.

## 0. Los tres niveles que hay

| nivel | qué ejecuta | dónde | qué demuestra |
|---|---|---|---|
| **unit** | una pieza aislada con dobles (`tests/unit/*.cpp`, meson) | host, `meson test -C build_linux` | lógica, FEC, vocoders, máquinas de estado, presupuestos de memoria |
| **host harness** | la cadena casi entera con un doble de verdad: radio simulada por PTY, audio a fichero | host, `tests/host/*/run.sh` | protocolo PC completo, modulador→demodulador, TX de punta a punta, TNC |
| **simulador** | el firmware real compilado para Linux (`openrtx_c62_sim`), con pantalla y teclado | host, `tools/simulator/*.py` por el canal `sim_control` | UI, menús, navegación con teclas, dibujo y desbordes de layout |
| **banco** | la radio con SDR y atenuadores | mesa | RF: potencia, desviación, ancho de banda, sensibilidad, BER real |

Lo que el simulador **no** puede ver, y por eso no se le pide: pilas (corre con
megas de pila), temporización real, nada que dependa de `DmrStatus`/`TetraStatus`
de verdad (están sustituidos por `ui_mode_status.c`), ni el DSP.

## 1. Matriz por modo

Leyenda: ✅ cubierto por tests automáticos · ⚠️ cubierto a medias · ❌ sin cubrir.

| modo | lo que hay que poder afirmar | hoy | dónde |
|---|---|---|---|
| **FM** | squelch y su histéresis; tonos CTCSS TX/RX y su anuncio; paso de frecuencia y límites; TX/RX de voz; APRS sobre FM | ⚠️ | `unit/fm_squelch` (histéresis de 1 dB, nivel vivo, tono que decide, rutas RX/TX sin ruta colgada), `unit/fm_ctcss` (tabla EIA, selección canal→rtx, anuncio leído de la cola de voz), `unit/fm_step` (tabla de pasos, raster del airband, snap, límites de banda), `unit/spectrum`, `unit/slevel`, `unit/audio_path`, `host/pc_proto_host` (pty), APRS sólo por `unit/aprs_config`; **niveles de audio y desviación: banco** |
| **AM / Airband** | canal, raster, profundidad de modulación, TX de AM, cambio de modo sin clavear | ✅ | `unit/airband` (plan de banda, designadores, snap, compuerta TX), `unit/airband_mode` (squelch con margen 1/2 dB y colgante de 400 ms, diálogo de claveado con `CONFIG_C62_AIR_TX`, cambio de modo que no clavea), `unit/fm_step` (raster 25k/8.33k), `unit/am_tx` (profundidad, ventana, rampas), `unit/agc`, `unit/mode_switch_am`; **profundidad y potencia reales: banco** |
| **DMR** | sincronismo, CC/TS, CACH, BPTC, LC con origen>destino, calidad; vocoder AMBE+2 (decodifica y sintetiza); SMS (formato, cola, ACK); TX (ráfaga, terminador, gate) | ✅/⚠️ | `unit/dmr_rx_status` (**doble del receptor**: basebanda de repetidor del modulador propio → `dmr_getStatus()`: CC/TS/CACH/LC/calidad, y las negativas —silencio, LC con checksum roto, CC mal corregido, CRC-32 roto—), `host/dmr_tx_host` (modulador→demod, bit-exacto), `host/dmr_pkg_host`, `unit/dmr_glue`, `unit/dmr_lc_dest`, `unit/dmr_sms_format`, `unit/imbe_*`; quedan fuera el camino BK4819 (REG_7E/REG_43), la pila de 7 KiB y el flujo MS/simplex: `reports/dmr_rx_standin.md` |
| **NXDN** | 4FSK 2400 Bd, FEC, RAN, SACCH (origen>destino), TX | ✅/⚠️ | `unit/nxdn_rx` (captura, ruido, rechazo de la FEC), `unit/nxdn_tx` (TX bit a bit, sincronismo sobre stream sintético con fase y deriva, RAN aceptado/rechazado, SACCH VCALL/TX_REL/DISC, estado que lee la UI); la voz hasta el altavoz, no |
| **dPMR** | 4FSK 2400 Bd, CCH, permutación de voz, TX, estado publicado (sync, canal, llamada, END) | ✅/⚠️ | `unit/dpmr_rx` (vectores, permutación exhaustiva, rechazo de la FEC), `unit/dpmr_tx` (TX bit a bit, layout de la ráfaga, sincronismo con fase y deriva, squelch publicado), `unit/dpmr_status` (**doble del receptor**: cabecera, supertramas y trama END del transmisor propio → demodulador real → `DpmrStatus.c` → `ui_mode_state_read()`: "CC0DD", "000135>00393D", VCALL/TX_REL/DISC, estado del END, y el estado vacío como "dPMR SRCH"); la voz hasta el altavoz, no |
| **P25** | C4FM, sync por sub-fase, TSBK, HDU/TDU, IMBE 7200, NAC | ✅/⚠️ | `unit/p25_phy`, `host/p25_phy_host`, `host/p25_tx`, `unit/imbe_robust` |
| **YSF** | 4FSK, FICH/DGID, modo V/D, voz AMBE+2, TX | ✅ | `unit/ysf_frame` (FICH roto, bloque DCH roto, voz FR no soportada, bit fijo de V/D 2, DG-ID), `unit/ysf_phy` (sync: encuentra y no inventa), `host/ysf_host`, `host/ysf_tx` |
| **D-STAR** | GMSK 4800, AMBE 3600x2400 propio, header, TX | ✅/⚠️ | `unit/dstar_header` (P_FCS, cabecera codificada y su rechazo, MY/UR/RPT, oídos, texto), `unit/dstar_phy` (transmisión completa, cortada en cabecera o en tramas, ruido, recuentos de error por palabra AMBE), `host/dstar`, `unit/` (vocoder por `lib/dstar_vocoder`); sin decodificador AMBE 2400: el modo calla |
| **M17** | Golay, Viterbi, RRC, demodulador, callsign, packet mode | ✅ | `unit/M17_*`, `unit/m17_replay` |
| **TETRA** | demodulación, CRC de bloques, SDS (D-SDS/U-SDS/UDD) de PDU a texto, uplink y registro, celda/MCC-MNC/LA, vecinas y reloj de red, ACC/REJ tal como se dibujan | ✅/⚠️ | `unit/tetra_demod`, `unit/tetra_uplink`, `unit/tetra_status` (doble de `TetraStatus.c` con PDUs reales), `unit/tetra_sds` (encoder → receptor real); la voz y los píxeles, no |
| **WFM (BK1080)** | init del chip, tune, mute del BK4819, conmutación de fuente de audio, coste | ✅/⚠️ | `unit/wfm`, `unit/wfm` + `tests/platform/c62_wfm_cost.c` |

**Lo que queda del analógico para el banco:** el host demuestra la lógica (histéresis,
selección, rutas, raster, claveado), no el aire. Siguen siendo del banco los
niveles de audio y la desviación de FM, la escala real de RSSI del chip (el
emulador da los dBm que se le piden, sin ruido ni filtros), la decodificación del
tono CTCSS —en el host `radio_linux_setDigitalSquelch()` dice si el chip lo ha
oído— y la profundidad de AM realmente radiada.

**El hueco común a los digitales:** ningún modo tiene un *doble del receptor*
que alimente `OpMode_*::update()` con muestras sintéticas y compruebe el estado
que se publica (sync, identificadores, calidad). Es exactamente la clase de
hueco por la que un CC inestable se ve en la radio y no en los tests. TETRA lo
tiene desde `unit/tetra_status`, y no por el `OpMode` sino por su estado: un
`tetra_tmo_rx` de verdad recibe PDUs de verdad (la grabación de bajada que trae
libtetra, sus PDUs de MM capturados, un D-NWRK-BROADCAST y una ráfaga de uplink
del propio encoder) y lo que se comprueba es `TetraStatus.c` y el texto que
`ui_mode_state.c` le da a la pantalla principal. NXDN y dPMR lo tienen por el
lado del PHY (`unit/nxdn_tx`, `unit/dpmr_status`): una llamada del transmisor
del árbol pasa por el demodulador real y llega al módulo de estado. Queda para
P25, YSF y D-STAR el mismo molde.

**P25: el hueco de la trama cortada está cerrado, el del doble no.** Una trama
cuyo final no llegó (la portadora cae después del NID) se publicaba como un LDU
completo: los símbolos que faltaban eran la línea de continua del discriminador,
cada palabra IMBE salía a cero y el cero es una palabra válida del Golay(23,12) y
del Hamming(15,11), así que el vocoder recibía 180 ms de voz "limpia" que nadie
transmitió. Ahora el PHY mide los últimos 10 ms de la trama antes de decodificar
nada (`frameEndOnAir()`, `p25Frame::truncated`) y de una trama cortada sólo
publica lo que sí estuvo en el aire: NAC y DUID. Lo fija `unit/p25_phy` en los
dos sentidos —corte en cinco puntos de la trama, y una trama débil (20 % de
amplitud) que sí publica sus nueve palabras—, y el arnés `host/p25_phy_host`
cuenta las truncadas por fichero. Lo que sigue faltando es el doble del receptor
para `OpMode_P25`, y la voz hasta el altavoz.

**DMR ya lo tiene** (`unit/dmr_rx_status`, `reports/dmr_rx_standin.md`): genera
las franjas de un repetidor con el modulador propio, las mete por
`dmr_demod_feed_disc()` en el tamaño de bloque de `OpMode_DMR::feed()` y afirma
lo que sale por `dmr_getStatus()`, incluidas las negativas. Ya encontró dos
cosas: un CC que sólo salía de una corrección se publicaba (corregido en
`dmr_proto.c`), y con el otro timeslot ocupado los identificadores de la
llamada parpadean en pantalla porque `DmrStatus.c` los toma de la última
ráfaga y no del último LC. P25, YSF y D-STAR siguen sin doble, y el mismo molde
sirve: los tres tienen su demodulador y su estado como los de DMR.

## 1bis. La configuración del chip, por modo

Lo que el firmware le pide al BK4819 al entrar en cada modo —canal y filtros de
FI (`REG_43`), audio (`AF bypass` frente a de-énfasis y compander), ganancias de
RX y TX (`REG_47`/`REG_48`/`REG_31`), DC y AGC (`REG_7E`), desviación de TX,
portadora de AM— es hoy una secuencia de **efectos** repartida por
`radio_C62.cpp`, y por eso no se puede comprobar sin radio. Ahí vivió el fallo de
DMR: 25 kHz y filtros de 9 kHz donde el firmware de fábrica usa 12,5 kHz y 4,5.

La regla que se adopta: esa configuración es **dato**, no efecto. Un plan por
modo —`{registro, máscara, valor}`— que el driver recorre, y un test en el host
que compara el plan con una tabla de oro por modo. Un cambio de ancho de banda,
de filtro DC o de ganancia rompe el build en vez de aparecer en el aire.

Se prueba, por modo: campo de canal y filtros de `REG_43`; modo de audio; bits
de DC/AGC de `REG_7E` (DMR y M17 con el paso-alto **apagado**, TETRA encendido);
ganancias; desviación de TX; y el camino de AM. `docs/RADIO_CONFIG.md` lleva la
tabla con el porqué de cada valor.
NXDN y dPMR ya tienen la mitad que importa de ese doble, sin radio: una llamada
construida con el transmisor del árbol pasa por el demodulador de verdad y llega
al módulo de estado que lee la UI (`nxdn_tx.cpp`, grupos `[status]`, `[ran]`,
`[layout]`, `[timing]`; `dpmr_status.cpp`, que además afirma los textos de
`ui_mode_state_read()`). Lo que queda fuera del alcance del host es lo que
`OpMode_*::update()` toca del resto del firmware — flujos de audio, PTT,
`nxdnSpeech_push()`/`dpmrSpeech_push()` — porque el build de host no instancia
los modos (`CONFIG_NXDN`, `CONFIG_DPMR`); su pegamento está compilado a mano
contra los mismos fuentes, no ejecutado.

## 2. Menús y ajustes

Todo lo de esta sección se decide en el host; nada de aquí necesita el banco.

- **Ninguna fila muerta, en ninguna pantalla.** `tools/simulator/menu_audit.py`
  recorre las 54 pantallas fila a fila (239 filas) y termina en error si una fila
  no contesta a ninguna tecla (P2, P1, verde) ni cambia un valor, una pantalla,
  el registro de ajustes o el receptor. Es el test `Simulator Menu Audit`. Las
  filas de las pantallas de datos (Info, Zonas, Contactos, Mensajes, APRS, PKT,
  Espectro) las mide el recorrido a nivel de pantalla y las cubre fila a fila
  `tests/unit/ui_menu_audit.cpp`.
- **Un ajuste, un sitio.** El mismo recorrido atribuye cada fila a los campos de
  `settings_t` que escribe (diff del bloque `settings` del dump, que ahora lleva
  el registro entero) y falla si un campo se cambia dos veces en una pantalla,
  desde dos páginas fijas de Ajustes, o desde dos familias de pantallas que no
  sean el bloque de acceso rápido declarado en el propio `menu_audit.py`
  (`SHARED_SETTINGS`: brillo, luz de teclas, squelch y ajuste fino de RX). El
  puerto de accesorio tiene además su aserción estructural en
  `unit/ui_menu_audit` ("The accessory port is one row, in the tools menu").
- **Persistencia.** `tools/simulator/settings_persist.py` (test `Simulator
  Settings Persistence`): un cambio en Ajustes ▸ Pantalla y el puerto de
  accesorio, cambiado en el menú de P1, siguen ahí tras apagar y encender.
- **Guardado diferido.** El mismo test: medio segundo después del cambio el
  registro del disco todavía tiene el valor viejo (un corte de corriente lo
  pierde, y así se comprueba: `powercut` sale sin secuencia de apagado) y a los
  4,5 s tiene el nuevo, que sobrevive al corte. Y un valor que el firmware
  corrige al cargar (brillo > 100) se escribe de vuelta en el registro.
- **Migración.** `unit/settings_record`: un registro de la versión 7 se migra
  byte a byte (memcmp de todo el prefijo, con todos los bloques que se añadieron
  después rellenos), el campo añadido vuelve a su valor de fábrica y el registro
  que se escribe después ya es de la versión 8.
- **P1/P2 cambian el valor en toda edición de Ajustes**; las flechas también
  (`unit/ui_menu_audit`, "P1 and P2 change the value in a Settings edit"); el
  `Simulator Menu Audit` lo vuelve a medir fila a fila.
- **El banco de canales desde el menú**: P1 largo abre Herramientas y su última
  fila abre la lista de canales, que tunea al canal como canal de memoria
  (`unit/ui_bank_tune`). Lo que hace cada tecla dentro de la lista y del editor
  —y que la lista se abra en el canal que estaba en uso— lo fija
  `unit/ui_channels_editor` (sección 3).
- **Lo que no cubre:** los campos de canal que editan las páginas de modo
  (CTCSS, tono, ancho de banda de FM) no son `settings_t` y quedan fuera de la
  regla de duplicados; los ajustes que se escriben con el teclado (indicativo,
  DMR ID) no cambian con una tecla y no aparecen en el mapa; y las pantallas de
  datos se miden a nivel de pantalla, como se ha dicho arriba.

## 3. Lógica de UI/UX

- **Una tecla por pantalla**: cada pantalla declara qué hace cada botón y el tour
  lo comprueba; ninguna tecla sin efecto, ningún efecto sin pista en la fila de
  ayuda.
- **Layout**: ningún texto se sale de su fila ni pisa otro (`ui_layout`,
  `gfx_text`, y el `dump` del simulador da `x`+`w` para medirlo).
- **Máquinas de estado**: entrada de VFO (dígitos, límites, campo TX), editor de
  canales, mensajería (borrador, cola, ACK, reintentos), scanner y búsqueda VFO,
  standby, teclado bloqueado.
- **Editor de canales** (`unit/ui_channels_editor`, con el recorrido de
  `ui_walk.hpp`): crear (P1 duplica el canal seleccionado, o el VFO con el banco
  vacío) y guardar con P2; editar un campo con verde y UP/DOWN; P1/P2 mueven el
  cursor dentro del nombre o de la frecuencia, no la selección de campo; rojo
  cancela sin tocar el banco; borrar sólo tras la confirmación (P2 largo y luego
  verde; NO deja el canal donde estaba); y la lista se abre en el canal que
  estaba en uso al apagar (`ui_channels_open()` lee `chstore_lastUsed()`,
  probado con un apagado/encendido de verdad sobre el banco en RAM:
  `chstore_commit()` y releer los dos slots). Las aserciones son sobre lo que la
  pantalla dibuja —título, filas, selección invertida y el rectángulo del cursor
  (`gfx_capture_caret`)— y sobre el registro guardado, nunca sobre un estático
  del módulo. Al escribirla salió un fallo: P2 largo en la lista no llegaba
  nunca a `ui_channels_handleEvent` porque la rejilla de macros se despachaba
  antes, así que la confirmación de borrado era inalcanzable desde el teclado
  (y la tecla siguiente se iba a un menú invisible). La lista se despacha ahora
  con las demás capas, antes de la rejilla.
- **Cola de mensajería** (`unit/messaging_fsm`): cola → `messaging_begin` (una
  entrada sólo se entrega una vez) → `messaging_done(false)` la devuelve a la
  cola hasta `MSG_MAX_ATTEMPTS` y después FAILED; `done(true)` es SENT si no se
  pidió confirmación y sigue SENDING si se pidió, y sólo el `messaging_confirmed`
  posterior la deja DELIVERED (por id o por peer); el plazo de 30 s lo cierra
  `messaging_task`; `messaging_fail` da por perdida una entrada con cualquier
  número de intentos; cancelar la saca de la cola para siempre y un resultado
  tardío no la revive; el outbox lleno (6) y la reutilización de un hueco
  terminado; y el registro completo —outbox, bandeja y borrador— por
  `messaging_serialise`/`deserialise`, con el CRC que rechaza un registro
  corrupto sin dejar media cola y la entrada que estaba en el aire al apagar
  volviendo como encolada.
- **Modo en la pantalla principal**: lo que el modo decodifica se ve sin abrir
  ningún menú (`ui_mode_state.c`, capturas en `reports/mode_status/`).
- **Estados vacíos**: sin sincronismo se lee `SRCH`/`-`, nunca un cero.

## 4. Protocolo, CPS y banco

- `tests/host/pc_proto_host/run.sh`: 4 suites sobre un PTY (43+15+20+7).
- Mensajes: `status`, `diag`, `settings`, banco (arm/tune/ptt/capture), KISS,
  caja negra, captura de audio.
- Reglas del cable: 115200; el testigo de sesión lo gastan **el siguiente
  `HELLO`**, y un `PTT` que lo presente gastado se rechaza con `not_allowed`
  sin tocar la PA — un run que clavea dos veces con el mismo testigo es lo
  normal, y así está fijado (`unit/pc_proto_bench`, `tools/bench/tests/
  test_bench_proto.py`); el cliente necesita anunciar `PC_CAP_EVENTS` en `HELLO`
  para recibir eventos (el flanco de PTT que mide la latencia) y 2 s de plazo
  para tener respuesta (`unit/pc_proto_bench`: sin la capacidad hay respuestas y
  ningún evento; `tools/cps/tests/test_pcproto.py`: con el cerrojo tomado,
  `status` contesta en menos de 100 ms y las lecturas que lo necesitan
  responden `busy`, no silencio). El «1-2 Hz» de las notas era del cliente que
  dispara sin esperar la respuesta: con peticiones serializadas el enlace
  contesta a más de 10 Hz, y la suite lo comprueba.

## 5. Plataforma

- **Presupuesto de pila por hilo, comprobado en compilación**: `rtx`
  (`unit/rtx_stack_budget`), voz DMR (`core/stack_budget.h`, el
  `_Static_assert` sigue en `DmrSpeech.c`) y **todos los demás** en
  `openrtx/include/core/stack_budget.h`, con suelo y margen: teclado, worker del
  códec, `pcproto`, `bb_supervisor` y las tres voces de playback (TETRA, D-STAR,
  DMR). El suelo está calculado sobre lo medido (`-fstack-usage` en Cortex-M33
  para D-STAR: 2 704 B en `parms_repeat_model`; en el host, `-Os`, para
  `pcproto`; las cifras documentadas en el propio fuente para el códec, el
  supervisor y la voz DMR). Bajarlo rompe la compilación, en el firmware y en
  `unit/thread_stacks`. Lo que no se pudo medir aquí (teclado, voz TETRA) tiene
  por suelo el valor que vuela, así que sólo puede subir; el hilo que lo cierra
  es el recorrido de hilos de la caja negra (`free` por hilo,
  `tools/cps/blackbox.py`).
- **Que la llamada exista**: `unit/platform_wiring` lee las fuentes y falla si
  el bucle principal deja de llamar a `rtx_supervise()`, si el perro se alimenta
  fuera de la regla de la UI, si un hilo vuelve a escribir su pila como literal
  o si un mensaje del banco pierde la puerta del testigo. Es la clase de fallo
  que ningún test de valor ve: el camino correcto que nadie llama.
- Cerrojos: nada de llamar al DSP con el mutex de audio tomado; esperas con plazo.
- Perros: el de aplicación se alimenta sólo mientras la UI avanza
  (`core/ui_watchdog.h`, `unit/tx_watch`); el de TX corta a los 90 s de aire
  (`unit/tx_watch`, `unit/tx_guard`, `unit/key_gate`) y el corte está probado de
  punta a punta por el camino real de `rtx.cpp` en `unit/mode_switch`.
- Caja negra: se lee con `tools/cps/blackbox.py`; un `FATAL detail 2` es
  desbordamiento de pila (enum de `fatal_types.h`).

## 6. Lo que sólo puede decir el banco

Potencia real por banda, desviación, ancho de banda ocupado, sensibilidad y BER
con señal de verdad, y el comportamiento con entrada fuerte (saturación). Eso se
pide con `tools/bench/{mode_sweep.py,shape.py,dmr_baseband.py}` y se decide con
la radio delante; no se intenta automatizar.

## 7. Las secciones que se lanzan

Cada una va a un agente con su propio worktree y su rama, sin tocar la radio ni
hacer push:

1. **FM y AM/Airband** — tonos, squelch, paso, TX/RX analógicos, AM.
2. **DMR** — incluido un doble del receptor que alimente `OpMode_DMR_RX` con
   muestras y compruebe CC/TS/LC.
3. **NXDN y dPMR** — 4FSK estrecho, FEC, SACCH/CCH, TX.
4. **P25, YSF y D-STAR** — PHY, FEC, vocoders, TX, y estado publicado.
5. **TETRA** — demodulación, SDS, uplink, capa MM con doble.
6. **Menús y ajustes** — recorrido, filas muertas, duplicados, persistencia.
7. **UI/UX** — teclas por pantalla, layout, máquinas de estado, estados vacíos.
8. **Plataforma y protocolo** — pilas por hilo, cerrojos, perros, mensajes del
   CPS y del banco.
9. **Configuración del chip por modo** — plan puro y tabla de oro (sección 1bis).

Criterio de aceptación de cada sección: tests nuevos que **fallen si el
comportamiento se rompe**, `meson test -C build_linux` en verde, y un informe con
lo que se cubrió, lo que no, y por qué.
