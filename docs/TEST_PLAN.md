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
| **FM** | squelch y su histéresis; tonos CTCSS TX/RX y su anuncio; paso de frecuencia y límites; TX/RX de voz; APRS sobre FM | ⚠️ | `unit/spectrum`, `unit/slevel`, `unit/audio_path`, `host/pc_proto_host` (pty), APRS sólo por `unit/aprs_config` |
| **AM / Airband** | canal, raster, profundidad de modulación, TX de AM, cambio de modo sin clavear | ✅/⚠️ | `unit/am_tx`, `unit/airband`, `unit/mode_switch_am`, `unit/agc` |
| **DMR** | sincronismo, CC/TS, CACH, BPTC, LC con origen>destino, calidad; vocoder AMBE+2 (decodifica y sintetiza); SMS (formato, cola, ACK); TX (ráfaga, terminador, gate) | ⚠️ | `host/dmr_tx_host` (modulador→demod, bit-exacto), `host/dmr_pkg_host`, `unit/dmr_glue`, `unit/dmr_lc_dest`, `unit/dmr_sms_format`, `unit/imbe_*`; **la cadena RX en la radio no tiene doble** |
| **NXDN** | 4FSK 2400 Bd, FEC, RAN, SACCH (origen>destino), TX | ⚠️ | `unit/nxdn_rx`, `unit/nxdn_tx` |
| **dPMR** | 4FSK 2400 Bd, CCH, permutación de voz, TX | ⚠️ | `unit/dpmr_rx`, `unit/dpmr_tx` |
| **P25** | C4FM, sync por sub-fase, TSBK, HDU/TDU, IMBE 7200, NAC | ✅/⚠️ | `unit/p25_phy`, `host/p25_phy_host`, `host/p25_tx`, `unit/imbe_robust` |
| **YSF** | 4FSK, FICH/DGID, modo V/D, IMBE, TX | ✅ | `host/ysf_host`, `host/ysf_tx` |
| **D-STAR** | GMSK 4800, AMBE 3600x2400 propio, header, TX | ⚠️ | `host/dstar`, `unit/` (vocoder por `lib/dstar_vocoder`) |
| **M17** | Golay, Viterbi, RRC, demodulador, callsign, packet mode | ✅ | `unit/M17_*`, `unit/m17_replay` |
| **TETRA** | demodulación, CRC de bloques, SDS (U-SDS/UDD), uplink y registro, celda/MCC-MNC | ✅/⚠️ | `unit/tetra_demod`, `unit/tetra_uplink`; sin doble para la capa MM |
| **WFM (BK1080)** | init del chip, tune, mute del BK4819, conmutación de fuente de audio, coste | ✅/⚠️ | `unit/wfm`, `unit/wfm` + `tests/platform/c62_wfm_cost.c` |

**El hueco común a los digitales:** ningún modo tiene un *doble del receptor*
que alimente `OpMode_*::update()` con muestras sintéticas y compruebe el estado
que se publica (sync, identificadores, calidad). Es exactamente la clase de
hueco por la que un CC inestable se ve en la radio y no en los tests.

## 2. Menús y ajustes

- Recorrido completo con el simulador: `tools/simulator/menu_audit.py` (53
  pantallas, 265 filas) y `tour.py` (314 pantallas). Debe dar **0 filas no
  actuables**.
- Un ajuste por pantalla, no dos: el puerto de accesorio vive sólo en el menú de
  P1 (`tests/unit/ui_menu_audit.cpp` lo fija con tres aserciones).
- Persistencia: cambiar un ajuste, reiniciar el simulador, comprobarlo; y que un
  registro viejo migre sin perder lo del operador (`unit/settings_record`).
- P1/P2 cambian el valor en toda edición de Ajustes; flechas también.
- Guardado diferido: un cambio sobrevive a apagar (el debouncer escribe a los 3 s).

## 3. Lógica de UI/UX

- **Una tecla por pantalla**: cada pantalla declara qué hace cada botón y el tour
  lo comprueba; ninguna tecla sin efecto, ningún efecto sin pista en la fila de
  ayuda.
- **Layout**: ningún texto se sale de su fila ni pisa otro (`ui_layout`,
  `gfx_text`, y el `dump` del simulador da `x`+`w` para medirlo).
- **Máquinas de estado**: entrada de VFO (dígitos, límites, campo TX), editor de
  canales, mensajería (borrador, cola, ACK, reintentos), scanner y búsqueda VFO,
  standby, teclado bloqueado.
- **Modo en la pantalla principal**: lo que el modo decodifica se ve sin abrir
  ningún menú (`ui_mode_state.c`, capturas en `reports/mode_status/`).
- **Estados vacíos**: sin sincronismo se lee `SRCH`/`-`, nunca un cero.

## 4. Protocolo, CPS y banco

- `tests/host/pc_proto_host/run.sh`: 4 suites sobre un PTY (43+11+20+7).
- Mensajes: `status`, `diag`, `settings`, banco (arm/tune/ptt/capture), KISS,
  caja negra, captura de audio.
- Reglas del cable: 115200; el token del banco se gasta en cada PTT (re-armar);
  el cliente necesita `client_caps` y 2 s de plazo; por encima de 2 Hz se pierden
  respuestas (limitación conocida del cliente en Python).

## 5. Plataforma

- **Presupuesto de pila por hilo, comprobado en compilación**: `rtx_stack_budget`
  (rtx) y el `_Static_assert` de `DmrSpeech.c` (voz). Faltan teclado, códec y
  protocolo.
- Cerrojos: nada de llamar al DSP con el mutex de audio tomado; esperas con plazo.
- Perros: el de aplicación se alimenta sólo mientras la UI avanza; el de TX corta
  a los 90 s de aire (`unit/tx_watch`, `unit/tx_guard`, `unit/key_gate`).
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

Criterio de aceptación de cada sección: tests nuevos que **fallen si el
comportamiento se rompe**, `meson test -C build_linux` en verde, y un informe con
lo que se cubrió, lo que no, y por qué.
