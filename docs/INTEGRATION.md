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

Una compilación con todo encendido tiene, por tanto, **once modos**: FM, M17,
DMR, TETRA, NXDN, dPMR, P25, YSF, D-STAR, AM aéreo y WFM.

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
`C62_MESSAGING`, `C62_SCAN` y `C62_DMR_SMS`; el macro que ven las fuentes sigue
siendo `CONFIG_*` y lo emite `target_compile_definitions`.

## 4. Compilación y medidas

Todo encendido:

```
cd /home/n0p/src/c62/build/c62-ws-mega
sudo docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work openrtx-c62 \
    c62-build -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y \
               -DCONFIG_C62_NXDN_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_P25=y \
               -DCONFIG_C62_DPMR_RX=y -DCONFIG_C62_DSTAR=y
```

`CONFIG_C62_DSTAR` arrastra `C62_DSTAR_VOCODER` (por defecto `y`) y
`DSTAR_TX` (por defecto `y`). El símbolo de dPMR es `CONFIG_C62_DPMR_RX`, y el
de D-STAR `CONFIG_C62_DSTAR`; los dos existen en `Kconfig.board` y el resultado
se comprueba en `zephyr/include/generated/autoconf.h`, no en la línea de
órdenes.

Uso de memoria del enlazado (Cortex-M33, Zephyr 3.4, TETRA, wide RX, DMR TX y
las dos ramas de esta pasada y sus pantallas también encendidos), `zephyr.bin`
de 614 740 B:

| Región | Usado | Tamaño | % |
|---|---|---|---|
| FLASH | 614 740 B | 1 MB | 58,63 % |
| RAM | 226 736 B | 256 KB | 86,49 % |
| ITCM | 8 468 B | 16 KB | 51,68 % |
| DTCM | 0 B | 16 KB | 0,00 % |
| PSRAMAP | 487 046 B | 1 792 KB | 26,54 % |

**Cabe**: quedan 433 836 B de flash, 35 408 B de RAM (34,6 KB) y 1 307 962 B de
PSRAM. La RAM es la región justa otra vez: pasa del 80 % que el banco de TX se
puso como techo (209 715 B) por 17 021 B.

Lo que costó esta pasada, medido contra la misma compilación sin dPMR ni D-STAR
(556 904 B / 213 880 B / 462 166 B):

| | FLASH | RAM | PSRAM |
|---|---|---|---|
| dPMR + D-STAR + UI | +57 836 B | +12 856 B | +24 880 B |

El reparto de esa RAM, por símbolos (`.bss`/`.data`):

- **D-STAR, 10,4 KB**: `dstarMode` son 5 788 B, y 5 760 de ellos son el
  `audioBuffer[2*1440]` que comparten RX y TX (es el búfer que se le pasa a
  `audioStream_start()`, o sea DMA: no puede ir a PSRAM). Encima, el anillo del
  PHY (1 KB), los 660 bits blandos de la cabecera y sus 660 de trabajo (1,3 KB),
  la lista de estaciones oídas (544 B) y el estado de datos lentos (316 B).
  Los **+8 B** que documenta `docs/RX_DSTAR.md` son del *vocoder* (su estado de
  14 096 B está en PSRAM), no del modo entero.
- **dPMR, 128 B** (`dpmrMode`): sus 10 784 B de anillos y buffers están en
  `.psram_section`, como decía su rama.
- Las tablas de la UI son `const`: 4 008 B de flash y 24 B de RAM (el estado
  del menú rápido no cambia).
- El resto es alineación y los símbolos pequeños de los dos PHY.

Si hiciera falta bajar del 80 % (hay que quitar 17 021 B), por este orden y sin
tocar nada medido en el aire:

1. `-DCONFIG_DSTAR_TX=n`: −5,7 KB de golpe (el `audioBuffer` y el estado del
   transmisor); PTT queda inerte en D-STAR y la recepción no cambia.
2. Mover a `.psram_section` lo que no es DMA de D-STAR: cabecera (1,3 KB), lista
   de oídos (544 B) y anillo del PHY (1 KB) → −2,9 KB. Hay que darles su
   `reset()` porque la sección es NOLOAD.
3. El vocoder AMBE+2 (`CONFIG_C62_DMR_VOCODER`, compartido por DMR, NXDN, YSF y
   dPMR) sigue siendo el mayor: apagarlo cuesta la voz de cuatro modos.
4. Y después los modos de recepción que menos se usen (`CONFIG_C62_P25`,
   `CONFIG_C62_YSF`, `CONFIG_C62_DPMR_RX`).

## 5. Tests de host

```
meson setup build_host -Dcpp_args=-fpermissive
meson test -C build_host --suite OpenRTX --print-errorlogs --no-rebuild
```

**43 entradas, 43 en verde** (`SelfTest` de Catch2 aparte). `linux_inputStream_test`
sigue excluido porque ya estaba roto antes de esta integración: no está
registrado en `meson.build` y no se construye.

Lo que añade esta pasada a la suite: las cinco tablas nuevas entran en el
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
  `REG_43 = 0x7e48`) está sin medir en la radio.
- **DMR empaquetado** (`c62/dmr-pkg`): el paquete de cambios de DMR que quedó
  fuera de esta fusión (modulación y detalles de RX/TX que no entraron en
  `c62/next`).
- **dPMR ↔ mensajes**: los datos cortos de dPMR (5.9.1 y 5.9.2) se decodifican,
  pero no hay transporte de dPMR en `core/messaging` (`docs/RX_DPMR.md`).
- **Voz del P25 y del YSF en modo 2** y el resto de lo que cada `docs/RX_*.md`
  deja escrito en su última sección.
