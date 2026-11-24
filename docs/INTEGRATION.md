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

Una compilación con todo encendido tiene, por tanto, nueve modos: FM, M17, DMR,
TETRA, NXDN, P25, YSF, AM aéreo y WFM.

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
| 6 | `OPMODE_DPMR` | no (sin implementar, ver §6) |
| 7 | `OPMODE_P25` | no |
| 8 | `OPMODE_YSF` | no |
| 9 | `OPMODE_DSTAR` | no (sin implementar, ver §6) |
| 10 | `OPMODE_AIRBAND` | no |
| 11 | `OPMODE_WFM_BC` | no |

Los valores 6 y 9 están reservados por sus ramas: existen en el enumerado y en
las listas de modos válidos, pero este árbol no tiene su `OpMode_*` ni su
`OPMODE_*` en el `switch` de `rtx.cpp`, así que no se pueden seleccionar.

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
               -DCONFIG_C62_NXDN_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_P25=y
```

Uso de memoria del enlazado (Cortex-M33, Zephyr 3.4, TETRA y wide RX también
encendidos):

| Región | Usado | Tamaño | % |
|---|---|---|---|
| FLASH | 556 752 B | 1 MB | 53,10 % |
| RAM | 213 880 B | 256 KB | 81,59 % |
| ITCM | 8 468 B | 16 KB | 51,68 % |
| DTCM | 0 B | 16 KB | 0,00 % |
| PSRAMAP | 462 166 B | 1 792 KB | 25,19 % |

Cabe, con el margen más justo en RAM (unas 42 KB libres sobre 256 KB) y mucha
holgura en flash y PSRAM. Lo que se recortaría primero si hiciera falta: el
vocoder AMBE+2 (`CONFIG_C62_DMR_VOCODER`, 41 KiB de flash y 52 KiB de RAM,
compartido por DMR, NXDN y YSF), y después los modos de recepción que menos se
usen (`CONFIG_C62_P25`, `CONFIG_C62_YSF`).

## 5. Tests de host

```
meson setup build_host
meson test -C build_host --print-errorlogs
```

41 entradas, **41 en verde** (40 del proyecto más el `SelfTest` de Catch2), con
`linux_inputStream_test` excluido porque ya estaba roto antes de esta
integración: no está registrado en `meson.build` y no se construye.

## 6. Qué falta

- **dPMR** (`c62/rx-dpmr`): el valor 6 del enumerado está reservado y las listas
  lo aceptan, pero no hay `OpMode_DPMR`, ni PHY, ni entrada en el `switch` de
  `rtx.cpp`.
- **DMR empaquetado** (`c62/dmr-pkg`): el paquete de cambios de DMR que quedó
  fuera de esta fusión (modulación y detalles de RX/TX que no entraron en
  `c62/next`).
- **D-STAR** (`c62/dstar-voice`): el valor 9 está reservado, sin decodificador de
  voz ni `OpMode_DSTAR`.
- **Pantallas de P25, YSF y WFM**: los tres modos compilan y se enlazan, y se
  llega a ellos por un canal o una zona, pero no están en el registro de modos
  de la UI (`uiModes[]` en `ui_quickmenu.c`): no salen en el ciclo de la tecla
  lateral ni en el editor de canales, y no tienen línea de estado propia.
- **Voz de P25**: hace falta un decodificador IMBE (`CONFIG_C62_P25_VOCODER`,
  ver `docs/RX_P25.md`); sin él el modo cuenta las tramas y no suena.
- **Voz FR de YSF**: el modo de voz de tasa completa lleva IMBE a 7200 bit/s y
  no hay decodificador; se cuenta y se salta (`docs/RX_FUSION.md`).
