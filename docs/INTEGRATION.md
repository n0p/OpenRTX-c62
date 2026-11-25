# Integración: la rama `c62/mega`

**Ámbito:** qué hay dentro de la rama de integración del firmware de la C62, las
reglas de numeración que la hacen fusionable, cómo se compila y mide, y qué
queda fuera. Los documentos de cada modo (`docs/RX_*.md`, `docs/DMR_*.md`,
`docs/SCAN.md`, `docs/TETRA_LIB.md`, `docs/UI.md`) siguen siendo la referencia
de su modo; esto es el mapa del conjunto.

---

## 1. Qué ramas están dentro

La rama parte de `c62/next` (FM, M17, DMR RX y TX con vocoder y encoder, NXDN,
APRS, airband AM, SMS, store de mensajes, `lib/dmr_*`, TETRA con `libtetra`) y
encima fusiona, en este orden:

| Rama | Qué aporta | Modo / ficheros |
|---|---|---|
| `c62/ui-modes` | el registro de modos de la UI: una sola lista de la que salen el editor de canales, la tecla lateral, el menú rápido y la página de ajustes de cada modo | `ui/ui_modes.h`, `ui_quickmenu.c`, `docs/UI.md` |
| `c62/tetra-elk` | TETRA de recepción sobre `libtetra` (ELK): sincronismo, SDS y voz | `OPMODE_TETRA`, `protocols/TETRA/**`, `docs/TETRA_LIB.md` |
| `c62/rx-stats` | estadísticas por modo (CRC, EVM, tramas decodificadas, coste) | `core/mode_stats.*`, `docs/STATS.md` |
| `c62/scan` | exploración de memorias y de VFO con canal prioritario | `core/scanner.*`, `core/scan_service.*`, `docs/SCAN.md` |
| `c62/rx-p25` | P25 Fase 1: C4FM, NID/BCH, TSBK, IMBE encuadrado | `OPMODE_P25`, `docs/RX_P25.md` |
| `c62/rx-fusion` | System Fusion (C4FM) con el vocoder AMBE+2 de DMR | `OPMODE_YSF`, `docs/RX_FUSION.md` |
| `c62/rx-wfm` | FM de radiodifusión con el sintonizador BK1080, fuera del códec | `OPMODE_WFM_BC`, `BK1080.*`, `core/wfm.*`, `docs/RX_WFM.md` |
| `c62/rx-dpmr` | dPMR (TS 102 490): 4FSK 2400 Bd en 6,25 kHz, CCH, datos cortos y voz AMBE+2 | `OPMODE_DPMR`, `protocols/DPMR/**`, `docs/RX_DPMR.md` |
| `c62/dstar-voice` | D-STAR DV: cabecera con FEC convolucional, tramas de 96 bits, datos lentos y voz AMBE 3600x2400 (`lib/dstar_vocoder`) | `OPMODE_DSTAR`, `protocols/DSTAR/**`, `docs/RX_DSTAR.md` |
| `c62/dmr-pkg` | el paquete de protocolo de DMR completo, en lugar de la cadena DMR propia: demodulador con valores soft, FEC (BPTC, Golay, QR, Reed-Solomon, trellis), protocolo y máquina de slots, voz, TX, datos, SMS, LRRP, cifrado y Tier III | `protocols/DMR/**`, `OpMode_DMR`, `docs/DMR_PKG.md` |
| `c62/spectrum` | analizador de espectro: barrido del receptor con la FI ancha desde el hilo RTX, pantalla propia y gancho de traza para el protocolo de PC | `core/spectrum.*`, `core/spectrum_service.*`, `ui/ui_spectrum.*`, `docs/SPECTRUM.md` |

Una compilación con todo encendido tiene, por tanto, **once modos**: FM, M17,
DMR, TETRA, NXDN, dPMR, P25, YSF, D-STAR, AM aéreo y WFM. Los once están en el
enumerado, y el DMR de esa lista ya no es la cadena de `c62/next`: es el paquete
de la tercera pasada (§3.2).

## 2. Reglas de numeración (no se tocan)

### 2.1 `enum opmode` (`openrtx/include/rtx/rtx.h`)

Los valores van escritos a mano y **solo se añaden al final**. Un valor nunca se
mueve ni se reutiliza: viaja en los registros de canal y en los ajustes.

| Valor | Modo | TX |
|---|---|---|
| 0 | `OPMODE_NONE` | — |
| 1 | `OPMODE_FM` | sí |
| 2 | `OPMODE_DMR` | sí, con `CONFIG_DMR_TX` |
| 3 | `OPMODE_M17` | sí |
| 4 | `OPMODE_TETRA` | no |
| 5 | `OPMODE_NXDN` | no |
| 6 | `OPMODE_DPMR` | no |
| 7 | `OPMODE_P25` | no |
| 8 | `OPMODE_YSF` | no |
| 9 | `OPMODE_DSTAR` | sí, con `CONFIG_DSTAR_TX` (voz = trama nula: no hay encoder 3600x2400) |
| 10 | `OPMODE_AIRBAND` | no |
| 11 | `OPMODE_WFM_BC` | no |

Los once valores están vivos en este árbol: cada uno tiene su `OpMode_*`, su
`case` en `rtx_rxSquelchOpen`/`rtx.cpp` y su entrada en el registro de la UI.

### 2.2 Ajustes (`openrtx/include/core/settings.h`)

- `SETTINGS_VERSION` vale **5**. Un registro de otra versión se descarta limpio
  (`settings_record.c` compara la versión y vuelve a los valores por defecto), no
  se migra: no hay conversión de campos entre versiones.
- Los bloques de campos de cada rama están **apendidos** al final de
  `settings_t`, nunca intercalados, y el inicializador `default_settings` sigue
  el mismo orden. Un bloque nuevo se añade detrás del último y el contador sube.
- El registro tiene un techo de 256 B; el bloque de scan está dimensionado para
  no gastar más de lo que necesita (ver el comentario de `settings.h`).
- **Tercera pasada**: el paquete de DMR apenda `int16_t dmr_tx_air_delay_us`
  (retardo de aire del camino de audio, 0 = el valor medido por defecto, ver
  `docs/DMR_PKG.md`) **detrás del bloque de scan**, o sea al final del todo. La
  versión **no sube**: el árbol es una sola línea de desarrollo y ningún campo
  se reinterpreta, pero el registro creció dos bytes, así que un registro de la
  pasada anterior (misma versión 5, dos bytes más corto) falla el CRC y se
  descarta entero en vez de leerse con el retardo a medias. El operador pierde
  los ajustes una vez, que es el precio documentado del apendido; lo prueba
  `tests/unit/settings_record.cpp` («A record from before the air delay is
  refused, not half-read»).

### 2.3 Zonas (`openrtx/include/core/zones.h`)

| id | zona | flags |
|---|---|---|
| 0 | `ZONE_VHF_REPEATERS` | — |
| 1 | `ZONE_UHF_REPEATERS` | — |
| 2 | `ZONE_PMR446` | — |
| 3 | `ZONE_AIRBAND` | `ZONE_FLAG_AIRBAND` (0x40), `ZONE_FLAG_RX_ONLY`, `ZONE_FLAG_SIMPLEX` |
| 4 | `ZONE_WFM_BC` | `ZONE_FLAG_WFM` (0x80), `ZONE_FLAG_RX_ONLY`, `ZONE_FLAG_SIMPLEX` |

`ZONE_COUNT = 5`. Los dos planes se identifican por su frecuencia
(`118.025`, `101.1`) mediante su flag: `ZONE_FLAG_AIRBAND` enruta el nombre a
`core/airband.h`, `ZONE_FLAG_WFM` a `core/wfm.h`.

### 2.4 Listas de modos válidos

`channel_store.c` (`chstore_valid`) y `zones.c` (`zones_channelValid`) aceptan
**todos** los modos del enumerado. No filtran por lo que esté compilado a
propósito: un registro de canal se escribe y se lee igual en cualquier
compilación, y es la UI (`ui_mode_find`) la que decide qué se puede elegir.

## 3. Cómo se resolvió la fusión

Las dos ramas que chocaron en `radio_C62.cpp` son airband (AM del BK4819) y
WFM (BK1080). Ninguna se descartó:

- **Constantes y estado**: los bloques de AM (`AM_RX_AUDIO_GAIN1`, umbrales del
  AGC, `AM_AF_STEP_MS`) y los de WFM (`WFM_STATUS_MS`, `WFM_VOLUME`) conviven;
  el estado del AGC (`amAgc`, `amAfGain1`) y el del sintonizador (`wfmReady`,
  `wfmLast`, …) también.
- **`radio_setOpmode()`**: una sola cadena `if/else if`, ordenada por modo
  (M17, DMR, NXDN, TETRA, AIRBAND, WFM_BC, y FM en el `else`). Antes de mirar el
  modo nuevo se hace la vuelta atrás de los dos perfiles especiales:
  - si el modo nuevo **no** es airband, se restaura la modulación FM
    (`bk4819_setModulation(true)`, que devuelve REG_13/REG_4A/REG_2B/REG_28 a su
    sitio) y se desinstala el procesador de audio del AGC;
  - si el modo nuevo **no** es WFM y el sintonizador estaba encendido, se apaga
    (`wfmLeave()`) y se cierra la fuente analógica (`audio_setExtSource(false)`),
    o el BK1080 seguiría sonando por debajo del modo siguiente.
- **`radio_enableTx()`**: WFM sale primero (no hay transmisor y no se toca la
  PA), y después el corte común de los modos de solo recepción (TETRA, airband,
  `txDisable`).
- **Audio**: WFM no pide ninguna ruta del códec; su audio va del BK1080 al
  amplificador por el potenciómetro analógico (`audio_setExtSource`). Airband sí
  usa el códec (AF del BK4819 → ADC → altavoz) y por eso instala su procesador
  de AGC en esa ruta.
- **`rtx.cpp`**: el `case OPMODE_NXDN` había quedado dentro del `#ifdef
  CONFIG_TETRA`; se sacó, y se quitó un `return false;` duplicado.
- **`ui_main.c`**: los ayudantes de línea de estado (`_ui_printFit`,
  `_ui_printWithSymbol`, `_ui_formatSsi`) viven fuera del bloque
  `#ifdef CONFIG_TETRA`, como los dejó `c62/tetra-elk`; la fusión los había
  duplicado dentro y fuera, con un `#ifdef` sin cerrar.
- **`meson.build`**: airband, agc y wfm se compilan y se prueban a la vez.

### 3.0 Segunda pasada: dPMR y D-STAR

Las dos ramas son aditivas y se resolvieron conservando todo, ordenando por modo
y apendiendo lo que va a los ajustes (esta rama ya estaba en `SETTINGS_VERSION`
5 y ninguna de las dos trae campos nuevos: dPMR y D-STAR no guardan nada propio,
así que no hubo renumeración ni subida de versión).

Lo que chocó y cómo se resolvió:

- **`rtx.h`**: las dos ramas reañadían su valor al enumerado. Se quedó la tabla
  única (0..11) que ya tenía `c62/mega`; `OPMODE_DPMR = 6` y `OPMODE_DSTAR = 9`
  ya estaban ahí con el mismo valor que traían las ramas.
- **`channel_store.c`, `zones.c` (listas de modos válidos) y
  `voicePromptUtils.c`**: las ramas añadían su modo a listas que aquí ya lo
  tenían. Se conservó la lista completa y se añadieron los anuncios de voz que
  faltaban (`dPMR`, `P25`, `YSF`, `D STAR`, `F M`).
- **`rtx.cpp`**: la rama de dPMR metía un `case OPMODE_DPMR` delante de
  `OPMODE_DMR` en `opmode_canTransmit()` y el árbol ya tenía otro más abajo — un
  `case` duplicado que no compila; se quitó el de la rama. En las listas de
  `#include` y de instancias, la inclusión de D-STAR había quedado **dentro** del
  `#ifdef CONFIG_DMR` (la rama la puso detrás del include de DMR y el `#endif` de
  DMR la absorbió): se sacó a su propio `#ifdef CONFIG_DSTAR`. D-STAR entra en
  `opmode_canTransmit()` con `CONFIG_DSTAR_TX`, como en su rama.
- **`radio_C62.cpp`**: se conservaron los dos juegos de constantes (AM/WFM de la
  primera pasada, dPMR de la segunda) y los dos bloques del `if/else if` de
  `radio_setOpmode()`, en orden de modo (NXDN, DPMR, P25/YSF/DSTAR, TETRA,
  AIRBAND, WFM_BC). La vuelta atrás de los perfiles especiales (modulación FM y
  salida del sintonizador) sigue **antes** del cambio de modo, como estaba.
- **`flatMode()`**: P25, YSF y D-STAR no estaban, así que recibían el perfil de
  voz FM (de-énfasis, compresor y REG_7E con el paso alto) en vez del
  discriminador plano. Los tres son baseband de envolvente constante (C4FM a
  4800 sym/s y GMSK de D-STAR), así que ahora entran en `flatMode()` y en
  `radio_setOpmode()` con el perfil de NXDN: `AUDIO_BYPASS`, canal de 12,5 kHz,
  filtro 7,7 y `REG_43 = 0x7e48`. **No medido en el aire todavía**: es el perfil
  con el que se desarrollaron sus demods en el host.
- **`CMakeLists.txt`**: los bloques de NXDN, dPMR, YSF, P25, DMR SMS y D-STAR
  conviven; el registro de `-ffp-contract=off` de `DpmrPhy.c` se conserva.
- **`Kconfig.board`**: dPMR entra como `C62_DPMR_RX` (selecciona
  `C62_DMR_VOCODER` si no hay DMR RX) y D-STAR como `C62_DSTAR` + sus dos
  subopciones `C62_DSTAR_VOCODER` y `DSTAR_TX`. La rama de D-STAR traía
  `select MESSAGING`, que en este árbol no existe como símbolo de Kconfig (el
  store es la opción CMake `C62_MESSAGING`, y la macro `CONFIG_MESSAGING` la
  emite `target_compile_definitions`): se quitó el `select` y el `if` del CMake
  pasa a `if(C62_MESSAGING)` para definir `CONFIG_DSTAR_MESSAGING`. Con el
  `if(CONFIG_MESSAGING)` de la rama, ese macro nunca se definía.
- **`rtx.cpp`, `rtx_rxSquelchOpen()`**: las filas `SYNC` de dPMR y YSF preguntan
  por el squelch del modo, y en el host `currMode` es `NULL` hasta `rtx_init()`
  (el hilo del RTX no se arranca), así que la primera pasada de tests se caía con
  SIGSEGV en `ui_modes_test`. La función devuelve `false` sin modo.

### 3.1 Trampa de Kconfig (arreglada)

`platform/targets/c62/CMakeLists.txt` declaraba `option(CONFIG_MESSAGING)`,
`option(CONFIG_SCAN)` y `option(CONFIG_DMR_SMS)`. Zephyr trata **cualquier**
variable de caché CMake que empiece por `CONFIG_` como una asignación de Kconfig
(`cmake/modules/kconfig.cmake`): en el primer `configure` no existen todavía y
no pasa nada, pero quedan en `CMakeCache.txt` y el siguiente aborta con
*«attempt to assign the value 'ON' to the undefined symbol»*. Ahora se llaman
`C62_MESSAGING` y `C62_SCAN`. Con la tercera pasada el SMS de DMR deja de ser una
opción de CMake: pasa a ser el símbolo de Kconfig `DMR_SMS` del paquete, que el
CMake lee como variable (`CONFIG_DMR_SMS`), y eso no es una entrada de caché. El
macro que ven las fuentes sigue siendo `CONFIG_*`.

### 3.2 Tercera pasada: el paquete de DMR

`c62/dmr-pkg` (base `44ece688`, cerrada en `090be87a`) no sustituye una rama
pequeña: reemplaza **toda** la capa DMR de este árbol por el paquete de
protocolo de otra C62 (≈12 100 líneas de C) y se queda con lo nuestro que había
alrededor. La fusión fue de 111 ficheros; esto es lo que chocó y cómo se quedó.

**Lo que se va con el paquete.** Se borran `DmrPhy.c`, `DmrFec.c`, `DmrSms.c`,
`DmrSmsApp.c`, `DmrTx.c`, `DmrTxTables.c`, `DmrVocoder.c`,
`DmrVocoderEncoder.c` y las cabeceras `DmrFec.h`, `DmrPhy.h`, `DmrSms.h`,
`DmrTx.h`, `DmrTxFsm.h`, `DmrVocoder.h`. Antes de borrarlas se sacó de ellas lo
que otros modos usaban:

- **NXDN** tomaba de `DmrDefs.h` el horario de dibits del frame de voz
  (`dmr_rw/dmr_rx/dmr_ry/dmr_rz`) y `dmr_ambe_layout`. Ahora son tablas privadas
  de `NxdnFec.c`: NXDN era su único usuario, y el `DmrDefs.h` del paquete solo
  lleva ya los nombres de tipo de ráfaga que pinta la pantalla.
- **El test de dPMR** comparaba su desentrelazado con
  `dmr_fec_voice_deinterleave()`. El paquete deja el horario de cuatro bloques
  de TS 102 361-1 privado en `dmr_voice.c`, así que la referencia vive ahora en
  el propio test, con la regla escrita al lado (bit canónico `c` → bit de aire
  `4*(c mod 18) + c/18`).
- **Los arneses del camino viejo** no compilan sin esos ficheros y se borran:
  `tests/host/dmr_phy_host/`, `tests/platform/c62_dmr_host.c`,
  `c62_dmr_phy_cost.c`, `c62_sms_host.c`, `c62_sms_cost.c` y
  `tests/unit/dmr_tx_fsm.cpp` (este último ya lo quitaba la rama). Sus
  sustitutos son `tests/host/dmr_pkg_host/`, `tests/host/dmr_pkg_qemu/` y
  `tests/host/dmr_enc_qemu/`. `docs/DMR_RX.md`, `docs/DMR_TX.md` y
  `docs/MESSAGING.md` siguen describiéndolos por sus medidas, con un aviso
  arriba.

**Lo que se conserva de nuestro lado.** Los vocoders. El paquete trae mbelib
(decoder) y OpenDMR (encoder) y los deja **apagados**: no caben en el M33
(mbelib 9,36 M instrucciones por trama de 20 ms, OpenDMR 4,10 M; los nuestros
113 402 y 1,427 M). `C62_DMR_VOICE=ON` y `DMR_ENC=ON` siguen ahí para poder
volver a medirlos. `lib/dmr_vocoder` y `lib/dmr_encoder` no cambian de API: el
paquete los llama a través de `dmr_voice.c`/`dmr_enc.c`.

**Los conflictos, uno a uno.**

- **`lib/dmr_vocoder/dmr_decoder.c/.h`**: las dos ramas añadieron funciones a la
  misma API. El paquete parte `dmr_vocoder_decode_frame()` en
  `dmr_vocoder_unpack()` (FEC, deja los 49 bits todavía cifrados) +
  `dmr_vocoder_decode_data()` (síntesis sin FEC), que es lo que necesita el
  descifrado de DMR; este árbol había añadido `dmr_vocoder_decode_ambe_params()`
  (el atajo de YSF, que ya trae los 49 bits) y `dmr_golay24_correct()` (el Golay
  del FICH de YSF). Se quedan **las cuatro**: `decode_data()` se implementa
  encima del `decode_params()` estático que ya existía, así que no hay una
  segunda copia de la síntesis; `decode_frame()` sigue llamando a
  `decode_params(v, d, pcm, nerr, errs)` y mantiene el comportamiento
  bit-exacto de antes.
- **`OpMode_DMR.cpp`**: se toma el del paquete entero (es otro modo: no decodifica
  nada, alimenta `dmr_demod_feed_disc()` y lee lo que el paquete publica), y se
  le devuelven los enganches de estadísticas de `c62/rx-stats` que el paquete no
  tiene: `modeStats_*` sobre los contadores del propio paquete
  (`dmr_proto_get_stats()` para tramas/válidas/CRC, `dmr_demod_get_status()`
  para las pérdidas, `radio_getRssi()` para el nivel), en `feed()`, y
  `txKey/txBlock/txStarve/txEnd` en el camino de TX. `core/mode_stats.h` entra
  como *include* y el estado de los deltas (3 × `uint32_t`) va en el `.hpp`.
- **`DmrDefs`/`DmrStatus`**: se quedan los del paquete (`dmr_data_type_name()`
  es lo único que la UI usa). `DmrStatus.c` del paquete sustituye al nuestro con
  los mismos nombres que esperan `ui_main.c` y el modo.
- **`Kconfig.board`**: se conserva la estructura de este árbol (menús NXDN, DMR,
  System Fusion, D-STAR, dPMR, APRS, P25) y dentro del menú DMR entran los
  símbolos del paquete `DMR_TRUNK` y `DMR_SMS` (los dos `default y`), más
  `C62_DMR_VOCODER` como símbolo compartido (lo seleccionan NXDN, YSF y dPMR; el
  modo DMR construye la biblioteca por su cuenta). **`DMR_TX_ENC` desaparece**:
  en el paquete `dmr_enc.c` no tiene hook débil, siempre es `lib/dmr_encoder`
  (o OpenDMR con `DMR_ENC=ON`), así que la opción ya no decidía nada.
- **`CMakeLists.txt`**: `lib/dmr_vocoder` se construye **una vez**, en un bloque
  propio que se dispara con `CONFIG_C62_DMR_RX` *o* `CONFIG_C62_DMR_VOCODER`,
  porque ahora lo piden cuatro modos y un segundo `add_subdirectory()` sobre el
  mismo directorio de binarios es un error de CMake. El bloque del DMR del
  paquete trae su parte de voz (mbelib opcional), SMS, Tier III, TX y encoder,
  sin volver a añadir la biblioteca. El dPMR ya no la enlaza él: la enlaza el
  bloque común.
- **`settings`**: versión 5 y el campo del paquete apendido al final (§2.2).
- **`meson.build`**: los tests de DMR del paquete (`dmr_lc_dest`, `dmr_sms_format`
  con `dmr_fec.c`/`dmr_data.c`/`dmr_lrrp.c`/`dmr_glue.c`, y el nuevo
  `dmr_glue_test`) conviven con los de dPMR; el `dmr_tx_fsm_test` desaparece y
  la suite sigue en 43 entradas. `dpmr_rx_test` deja de compilar `DmrFec.c` y
  `DmrDefs.c` (ver arriba).

## 4. Compilación y medidas

Todo encendido, con el paquete de DMR y su transmisor:

```
cd /home/n0p/src/c62/build/c62-ws-mega
sudo docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work openrtx-c62 \
    c62-build -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y \
               -DCONFIG_C62_NXDN_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_P25=y \
               -DCONFIG_C62_DPMR_RX=y -DCONFIG_C62_DSTAR=y \
               -DCONFIG_DMR_TX=y
```

`CONFIG_C62_DSTAR` arrastra `C62_DSTAR_VOCODER` (por defecto `y`) y
`DSTAR_TX` (por defecto `y`); `CONFIG_C62_DMR_RX` arrastra a su vez
`DMR_TRUNK` y `DMR_SMS` (los dos `default y`), y construye el vocoder y, con
`CONFIG_DMR_TX`, el codificador. El símbolo de dPMR es `CONFIG_C62_DPMR_RX` y el
de D-STAR `CONFIG_C62_DSTAR`: los dos existen en `Kconfig.board` y el resultado
se comprueba en `zephyr/include/generated/autoconf.h`, no en la línea de
órdenes. La instrumentación por modo sigue siendo la opción de CMake
`C62_MODE_STATS` (apagada; con ella encendida compila y suma 552 B de RAM, ver
abajo).

### 4.1 Memoria

Dos variantes del mismo árbol, Cortex-M33, Zephyr 3.4, TETRA, wide RX y las
pantallas encendidas; la única diferencia es el transmisor de DMR:

| build | FLASH | RAM | ITCM | DTCM | PSRAMAP |
|---|---|---|---|---|---|
| todo RX, **sin** `CONFIG_DMR_TX` | 648 184 B (61,82 %) | **195 056 B (74,41 %)** | 8 468 B (51,68 %) | 12 640 B (77,15 %) | 566 354 B (30,86 %) |
| todo RX **+ `CONFIG_DMR_TX`** | 709 400 B (67,65 %) | **208 136 B (79,40 %)** | 8 468 B (51,68 %) | 12 640 B (77,15 %) | 566 354 B (30,86 %) |
| `C62_MODE_STATS=ON`, todo RX | 649 660 B (61,96 %) | 195 608 B (74,62 %) | 8 468 B | 12 640 B | 566 354 B |

Las dos caben. La de TX queda **1 579 B por debajo del techo del 80 %**
(209 715 B) y le sobran 339 176 B de flash, 3 744 B de DTCM y 1 268 654 B de
PSRAM. La variante sin transmisor queda a 14 659 B del techo (67 088 B libres de
RAM en total).

El precio del DMR, medido con la misma línea de órdenes sin
`-DCONFIG_C62_DMR_RX` ni `-DCONFIG_DMR_TX` (diez modos, 599 888 B de flash,
182 400 B de RAM, 0 B de DTCM, 517 530 B de PSRAM):

| | FLASH | RAM | DTCM | PSRAM |
|---|---|---|---|---|
| DMR RX + TX sobre los otros diez modos | +109 512 B | +25 736 B | +12 640 B | +48 824 B |

Los dos últimos coinciden casi exactamente con lo que su rama midió en su propio
árbol (200 000 B de RAM y 12 640 B de DTCM con RX+TX, frente a 464 100 B de
flash y 174 288 B de RAM sin DMR: +25 712 B de RAM y +48 824 B de PSRAM). El
flash no es comparable uno a uno porque los árboles no llevan los mismos modos.

### 4.2 Cómo se bajó del 86,5 % al 79,4 %

La fusión del paquete dejó la RAM en **238 616 B (91,02 %)** — el DMR del paquete
solo cuesta 11 880 B más que la cadena propia, pero la cadena propia ya iba por
226 736 B. Se bajó **30 480 B** moviendo a PSRAM estado que se toca una vez por
trama o por ráfaga, nunca por muestra. Nada de lo que sigue es DMA, y nada
cambia lo que el modo hace; el mapa está en `core/psram_state.h`:

| qué | dónde | B |
|---|---|---|
| estado del demodulador de NXDN (historia 48 kHz, anillo 24 kHz, trama) | `NxdnPhy.c`, `nxdnPhy_reset()` lo limpia al abrir | 10 016 |
| instancia única del vocoder AMBE+2 (la que usan NXDN, YSF y dPMR) | `lib/dmr_vocoder/dmr_vocoder.c`, con `dmr_ready` en `.bss` | 8 540 |
| anillo del PHY de D-STAR (1 024) + 660 bits blandos de cabecera + 1 320 de memoria de camino del Viterbi + 544 de la lista de estaciones oídas | `DStarPhy.c`, `DStarFec.c`, `DStarStatus.c` | 3 548 |
| lista de estaciones y buzón de APRS | `aprs_service.c`, inicializados en `aprs_service_init()` | 5 588 |
| cola de reproducción de NXDN | `NxdnSpeech.c`, como la de YSF | 1 460 |
| registro empaquetado del store de mensajes | `messaging.c`, escrito antes de leerse en carga y guardado | 2 500 |

`lib/dmr_vocoder` recibe el atributo por CMake (`DMR_VOCODER_STATE_ATTR`), igual
que ya hacía el vocoder de D-STAR (`DSTAR_VOCODER_STATE_ATTR`); `YsfMem.h`
mantiene su macro `YSF_EXT_RAM` pero ahora es la misma definición
(`core/psram_state.h`), no una segunda copia a mano.

**Lo que no se movió, y por qué:**

- El `audioBuffer` de D-STAR (5 760 B) y los `rxSamples`/`txSamples` de DMR
  (5 760 B cada uno), los `baseband[]` de NXDN y dPMR, el `pcm` de NXDN y el
  `buf` de APRS: son los búferes que recibe `audioStream_start()`, o sea DMA.
- El estado de los modos que se midieron en el aire (TETRA `tmo`/`demod`, los
  anillos de P25 y del DMR del paquete) se queda donde estaba: los dos primeros
  no se han vuelto a medir en esta pasada y el del DMR ya vive en `.psram_dmr`
  por decisión de su paquete.
- DTCM sigue en 12 640 B (77,15 %): ahí van el estado del vocoder de DMR
  (`DMR_FAST_BSS`) y la pila del hilo de voz, que es donde el paquete los puso.

Si una pasada futura vuelve a acercarse al techo, lo siguiente por este orden,
sin tocar nada medido: `-DCONFIG_DSTAR_TX=n` (−5,7 KB: el `audioBuffer` y el
estado del transmisor; PTT queda inerte en D-STAR), los `headerBits`/
`slowBlocks`/`modAcc` del transmisor de D-STAR (932 B), el estado del `tmo` y
del `demod` de TETRA (11,9 KB, pero están medidos en el aire y habría que
repetir la medida), y por último apagar modos de recepción
(`CONFIG_C62_P25`, `CONFIG_C62_YSF`, `CONFIG_C62_DPMR_RX`).

Las dos secciones de PSRAM no se confunden: `.psram_dmr` (NOLOAD, la limpia
`platform_init_csk6()` con `psram_dmr_clear()`) es del paquete de DMR;
`.psram_section` (también NOLOAD, compartida con el heap de 300 KB del CSK6, no
se limpia) es donde va lo de arriba, y cada módulo se reinicia al abrir su modo.

## 5. Tests de host

```
meson setup build_host -Dcpp_args=-fpermissive
meson test -C build_host --suite OpenRTX --print-errorlogs --no-rebuild
```

**43 entradas, 43 en verde** (`SelfTest` de Catch2 aparte). `linux_inputStream_test`
sigue excluido porque ya estaba roto antes de esta integración: no está
registrado en `meson.build` y no se construye.

**El cuarto pase sube la cuenta a 47** con `ui_regressions_test` (y su variante
sin APRS), que son las pruebas *por síntoma* de lo que el operador reportó en la
radio: menú rápido con P2, una página de Ajustes por modo, entrada de frecuencia
y ENTER, búsqueda tras tres segundos, menú de mensajes y PTT de DMR. Las seis
funciones estaban probadas por debajo del despacho y no a través de él, que es
donde estaban los fallos; §6.1 recoge los dos que aparecieron.

Lo que añade la tercera pasada: el `dmr_tx_fsm_test` se va con su máquina de
estados y entra `dmr_glue_test` (los *records* que lee el paquete, el retardo de
aire y su pre-roll), así que la cuenta no cambia; `dmr_lc_dest_test` y
`dmr_sms_format_test` pasan a compilar los ficheros del paquete
(`dmr_fec.c`, `dmr_data.c`, `dmr_lrrp.c`, `dmr_glue.c`) con
`openrtx/include/protocols/DMR` y su `trunk/` en la ruta de includes;
`dpmr_rx_test` deja de arrastrar `DmrFec.c`/`DmrDefs.c` y compara contra la
referencia de cuatro bloques que lleva dentro; `settings_record_test` cambia el
caso de migración por el del registro dos bytes más corto (§2.2). Los ficheros
que se movieron a `.psram_section` compilan igual en el host: el atributo solo
existe con `PLATFORM_C62` (`core/psram_state.h`).

Lo que añadió la segunda: las cinco tablas nuevas entran en el
recorrido compartido de `ui_quickmenu_test` (todas las filas de todos los modos,
formateadas y ajustadas a su límite), y `ui_modes_test` comprueba modo a modo
que la etiqueta cabe en la cabecera, que la tecla lateral pasa por los once y
vuelve, que cada página de ajustes es la misma tabla que el menú rápido y que un
modo sin datos no imprime un cero (`SRCH`/`---`). Para poder hacerlo en el host,
`meson.build` compila en el binario de test los tres módulos de estado
portables que leen las pantallas: `P25Status.c`, `DStarStatus.c` y
`DStarSlowData.c`, y define `UI_TEST_DPMR/P25/YSF/DSTAR` (no los `CONFIG_*`,
que arrastrarían los demods y el vocoder a todos los binarios).

## 6. Qué queda (y qué no)

Los once modos están en la UI: ciclo de la tecla lateral, editor de canales y
página de ajustes propia, con sus filas de menú rápido y su línea de estado en
la pantalla principal (`docs/UI.md`, «Mode registry»). Lo que sigue pendiente es
de fondo, no de registro:

- **Voz de P25**: hace falta un decodificador IMBE (`CONFIG_C62_P25_VOCODER`,
  ver `docs/RX_P25.md`); sin él el modo cuenta las tramas y no suena.
- **Voz FR de YSF**: el modo de voz de tasa completa lleva IMBE a 7200 bit/s y
  no hay decodificador; se cuenta y se salta (`docs/RX_FUSION.md`). Las filas
  del menú de YSF son, por eso, `SYNC` y la cola común: el frame layer todavía
  no publica un bloque de estado que la UI pueda leer.
- **Voz de D-STAR**: el transmisor manda la trama nula de AMBE (no hay encoder
  3600x2400); la recepción sí decodifica voz.
- **P25/YSF/D-STAR en el aire**: su perfil de RF (discriminador plano, 12,5 kHz,
  `REG_43 = 0x7e48`) está sin medir en la radio. El DMR usa su propio perfil
  (`radio_C62.cpp`, `C62_DMR_12K5`).
- **DMR del paquete en el aire**: el demodulador, el TX y el retardo de aire
  del paquete están medidos en su árbol (37,1 M instr/s = 14,8 % en recepción)
  pero **no en la C62**: falta la captura con repetidor real, el ajuste fino de
  `dmr_tx_air_delay_us` en el banco (por defecto 0 = 40 ms calculados) y la
  comprobación de la desviación del modulador.
- **Cifrado y Tier III de DMR**: BP/EP (ARC4), AES-128/256, DES y Hytera, y el
  motor Tier III de RadioFoundry, compilan y están en el modo, pero solo se han
  probado con los vectores del paquete (`tests/host/dmr_pkg_host`,
  `t3_probe`). No hay clave en la UI ni sistema Tier III en el banco.
- **LRRP**: las posiciones se decodifican y se ven en el registro de llamadas;
  no se dibujan en el mapa ni entran en el store de posiciones.
- **dPMR ↔ mensajes**: los datos cortos de dPMR (5.9.1 y 5.9.2) se decodifican,
  pero no hay transporte de dPMR en `core/messaging` (`docs/RX_DPMR.md`).
- **Voz del P25 y del YSF en modo 2** y el resto de lo que cada `docs/RX_*.md`
  deja escrito en su última sección.

## 6.1 Cuarto pase: lo que el operador encontró en la radio

Seis síntomas reportados sobre la build de `c62/mega` flasheada. Dos eran
fallos reales de cableado; los otros cuatro no se reproducen en el FSM (el
despacho actual los hace bien) y quedan cubiertos por pruebas para que un
cambio futuro no los rompa en silencio.

| Síntoma | Causa | Arreglo |
|---|---|---|
| DMR ya no emite | `OpMode_DMR::startTx()` del paquete se negaba a emitir sin ID propio válido; el registro de ajustes creció dos bytes y el CRC descartó el anterior, así que la radio volvió al `dmr_id = 0` de fábrica: PTT muerto, sin error | `dmrKey_allowed()` (`rtx/DmrKey.h`): el ID propio no es condición, solo un destino no codificable; y el rechazo marca `txBlocked` en vez de reintentar cada 30 ms |
| El menú rápido de P2 no abre, no hay knobs, el keypad no entra frecuencia, la búsqueda no arranca | los cuatro son la pantalla principal: `ui_state.edit_mode` solo tiene salida dentro del `#ifdef CONFIG_M17`, y lo pone la tecla # de M17. Un cambio de modo con el destino M17 en edición dejaba la pantalla sorda a todas las teclas | al entrar en `MAIN_VFO`/`MAIN_MEM`, un `edit_mode` que no es de M17 se descarta |
| No hay menú de mensajes | el `#ifdef CONFIG_APRS` del despacho de APRS seguía abierto cien líneas más abajo y se tragaba el despacho de mensajes, contactos y zonas: en una build sin APRS (y en los tests de host) la bandeja abría pero el teclado iba al FSM anterior | el bloque de APRS se cierra donde acaba y el despacho de las otras tres pantallas es incondicional |
| El keypad no elige frecuencia: se teclea y no pasa nada | con el campo RX en edición, el verde no aplicaba: pasaba al campo TX y dejaba la frecuencia sin aplicar (el receptor no se movía y el campo se quedaba en blanco) | el verde vuelve a ser *Set*: aplica lo tecleado y sale al VFO; el split se sigue haciendo con UP/DOWN y el campo TX vacío copia RX |
| Ajustes sin páginas por modo | no reproducido: `settings_items[]` sale de `UI_MODE_PAGES` y la build lleva las siete páginas (`NXDN`, `dPMR`, `P25`, `YSF`, `DSTAR`, `AM`, `WFM`), comprobado leyendo la tabla del `zephyr.elf` | prueba por síntoma: la lista tiene una página por modo compilado y cada una abre, responde a P2 y se cierra |

Nota de uso para el operador: en el VFO, el verde **aplica** la frecuencia
tecleada aunque no llegues a los ocho dígitos; si quieres un split, el octavo
dígito (o UP/DOWN) pasa al campo TX y el campo de TX vacío copia el de RX. Los
modos de solo recepción piden una sola frecuencia.
