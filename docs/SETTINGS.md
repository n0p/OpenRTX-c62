# Ajustes: versión del registro y migración

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre el registro de ajustes en flash: qué guarda, cómo se versiona y qué pasa
al actualizar el firmware. El resto de los ajustes (qué hace cada campo) se
describe en `docs/MESSAGING.md` (DMR/APRS), `docs/ZONES.md` y `docs/UI.md`.

---

## 1. Dónde viven

`settings_t` (`include/core/settings.h`) se guarda en el primer sector de la
partición `storage` del C62 (`platform/drivers/NVM/nvmem_C62.c`), en un registro
de **cabecera + payload**:

| offset | tamaño | campo |
|---|---|---|
| 0 | 4 | magic `"ORTX"` (`0x4F525458`) |
| 4 | 2 | CRC-CCITT del payload |
| 6 | 2 | **versión del payload** (`SETTINGS_VERSION`) |
| 8 | n | `settings_t`, empaquetado |

El formato lo define `core/settings_record.h` / `settings_record.c` y lo prueban
`tests/unit/settings_record.cpp` y `tests/unit/channel_store.cpp`: el mismo
código que corre en el radio.

## 2. Por qué hay versión

`settings_t` crece. Sin versión, un registro escrito por un firmware anterior se
reinterpreta con los campos desplazados: un nivel de squelch leído de un
indicativo, un DMRID leído de un texto. Eso es peor que un reset, porque el
radio arranca con una configuración plausible y equivocada.

La regla es: **si la versión no es la de este firmware, el registro se descarta
entero y vuelven los valores por defecto**. El CRC se comprueba después de la
versión, para poder distinguir «escrito por otra versión» de «corrupto».

| versión | contenido |
|---|---|
| 1 | registro original, sin campo de versión en la cabecera (el hueco era relleno a 0) |
| 2 | destino DMR (`dmr_dest_type`, `dmr_dest_id`), DMRID propio (`dmr_id`) y formato de SMS (`sms_format`) |
| 3 | timeslot de DMR (`dmr_timeslot`, 1 o 2) para el menú rápido |

Un registro de la versión 1 tiene el campo de versión a 0, así que se descarta
solo: no hace falta código de migración y no hay forma de leerlo mal.

## 3. Qué ve el operador

`nvm_readSettings()` devuelve `0` o `-(enum settingsReset)`:

| valor | significado |
|---|---|
| `SETTINGS_RESET_BLANK` | partición vacía: primer encendido |
| `SETTINGS_RESET_VERSION` | registro de otra versión del firmware: descartado |
| `SETTINGS_RESET_CORRUPT` | CRC o magic mal: descartado |

`state_init()` guarda el motivo en `state.settings_reset`;
`state_settingsResetCause()` lo devuelve y `state_settingsResetAck()` lo borra
después de que la pantalla lo haya enseñado una vez. La UI puede así avisar
«ajustes reiniciados (actualización de firmware)» en vez de dejar al operador
buscando su indicativo.

## 4. Tamaño

`sizeof(settings_t)` es 167 B con APRS y los campos nuevos;
`SETTINGS_RECORD_SIZE` = 175 B, por debajo del límite de 256 B del registro del
driver (`NVM_RECORD_MAX`). El test comprueba que el registro no pase de 192 B
para que quede margen antes de tocar la estructura otra vez.

## 5. Qué campos llevan validación

| campo | regla | función |
|---|---|---|
| `dmr_id` | 1..16777215 (24 bits; 0 = sin configurar) | `settings_dmrIdValid()` |
| `dmr_dest_type` | `NONE`, `GROUP` o `USER` | `settings_dmrDestValid()` |
| `dmr_dest_id` | 1..16777215 si el tipo no es `NONE` | `settings_dmrDestValid()` |
| `sms_format` | 0..7 (`enum dmrSmsFormat`) | `settings_smsFormatValid()` |
| `dmr_timeslot` | 1 o 2 | `settings_dmrTimeslotValid()` |

Las funciones son `static inline` en `settings.h`: una pantalla puede validar
sin llamar a nada. Los nombres cortos para los menús
(`settings_dmrDestName()`, `settings_smsFormatName()`,
`settings_dmrTimeslotName()`) sí viven en `settings.c`.

## 6. Canales: migración sin perder nada

El banco de canales (`core/channel_store.h`) tiene su propia versión
(`CHSTORE_VERSION`, hoy **2**, con el destino DMR por canal) y **sí migra**: un
banco de la versión 1 se lee registro a registro de 32 B y se convierte al de
37 B en el sitio (32 B de pila, sin una segunda copia del banco), porque perder
los canales del operador en una actualización no es aceptable. Un banco escrito
por una versión **más nueva** se deja intacto y se presenta vacío, en vez de
interpretarlo. Detalles en `docs/CHANNELS.md`.

## 7. Después de tocar la estructura

1. Sube `SETTINGS_VERSION` en `settings.h`.
2. Si añades campos, añádelos también al final de `default_settings` (el orden
   es posicional).
3. Comprueba el tamaño: `tests/unit/settings_record.cpp` falla si el registro
   pasa de 192 B.
4. Avisa en las notas de la versión: **un reset de ajustes** es lo esperado al
   actualizar.
