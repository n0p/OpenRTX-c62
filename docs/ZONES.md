# Zonas: planes de canales como datos

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Este documento
cubre `core/zones.*`: los planes de canales que el editor ofrece como punto de
partida (repetidoras de 2 m y 70 cm y canales PMR446). Son **tablas de datos y
una API de consulta**, no pantallas: la UI decide cómo se listan y qué se hace
con un canal, y `chstore_applyRtx()` / `chstore_add()` son los que lo llevan al
radio.

Nada de esto ocupa RAM: una zona es una base, un paso, un número de canales y un
desplazamiento, y cada canal se calcula cuando se pide. 125 canales cuestan
unas decenas de bytes de flash.

---

## 1. Las zonas

| zona | nombre | canales | RX del canal 0 | paso | shift | potencia |
|---|---|---|---|---|---|---|
| `ZONE_VHF_REPEATERS` | `R0`..`R7` | 8 | 145.600 MHz | 25 kHz | −600 kHz | 5 W (bajable) |
| `ZONE_UHF_REPEATERS` | `U0`..`U100` | 101 | 438.650 MHz | 12,5 kHz | −7,6 MHz | 5 W (bajable) |
| `ZONE_PMR446` | `PMR1`..`PMR16` | 16 | 446.00625 MHz | 12,5 kHz | 0 (símplex) | **500 mW fijos** |
| `ZONE_WFM_BC` | `85.0`..`108.0` | 231 | 85.0 MHz | 100 kHz | 0 (símplex) | — (sólo RX) |

`rx` es siempre **la frecuencia en la que se escucha** (la salida de la
repetidora) y `tx = rx + shift`; con el plan español `shift` es negativo porque
la entrada está por debajo de la salida.

La zona de radiodifusión es distinta de las otras tres: no es un plan de
radioaficionado sino la rejilla CCIR de emisoras comerciales, **sólo recepción**
(modo `OPMODE_WFM_BC`, receptor BK1080), y sus canales se llaman por su
frecuencia («101.1») porque es así como se usa la banda. El plan va de 85,0 a
108,0 MHz porque el BK1080 no sintoniza por encima de 108,0; el porqué y el
detalle del receptor están en `docs/RX_WFM.md`. La potencia del canal es un
valor de relleno: la radio no transmite en esta banda y el PTT no la clavea.

## 2. Fuentes y verificación de frecuencias

Las frecuencias no son una convención de radioaficionado: son el plan legal
español. Se han verificado en fuentes públicas antes de escribirlas:

1. **Reglamento de uso del dominio público radioeléctrico por aficionados**,
   BOE nº 137 de 9 de junio de 2006, §5 «Frecuencias de los repetidores
   analógicos», reproducido por la URE/EA8NQ en
   <https://ea8nq.ure.es/bandas/plan_ea.htm>:
   - §5.1, 144-146 MHz: **16 canales de 12,5 kHz**, separación Tx/Rx de
     **600 kHz**; entradas 145,0000-145,1875 MHz, salidas 145,6000-145,7875 MHz.
   - §5.2, 430-440 MHz: **32 canales de 25 kHz**, separación de **7,6 MHz**;
     entradas 431,050-431,825 MHz, salidas 438,650-439,425 MHz.
2. **URE, «Canales y frecuencias repetidores VHF»** — tabla R0..R7 con entrada y
   salida (R0: 145.000 / 145.600 … R7: 145.175 / 145.775):
   <https://www.ure.es/wp-content/uploads/wpforo/default_attachments/media/kunena/attachments/legacy/files/Canales_y_frecuencias_repetidores_VHF.doc>
3. **Listado de repetidores de la URE**, que nombra cada repetidor por canal
   («145.600 MHz -600 kHz FM (RV48 – R0)», «145.675 MHz -600 kHz FM (RV54 – R3)»,
   «145.775 MHz … (RV62 – R7)»):
   <https://www.ure.es/wp-content/uploads/wpforo/default_attachments/1530887843-FRECUENCIAS-REPETIDORES.xlsx>
4. **EA8NQ/URE, «Denominación de canales NBFM en VHF/UHF»** (sistema IARU de
   Tel Aviv 1996): `RV48` = 145.600 MHz = antiguo **R0**, `RU692` = 438.650 MHz
   = antiguo **R70**, paso de 12,5 kHz desde 145,000 y desde 430,000 MHz:
   <https://ea8nq.ure.es/bandas/can_vhf.htm>
5. **Listado EA1DVY** (URE), que da los canales de 70 cm como `R70`..`R101` con
   −7,6 MHz (`438.650 MHz FM -7.6 MHz (RU692 - R70)`, …, `439.425 … (RU754 -
   R101)`):
   <https://www.ure.es/wp-content/uploads/wpforo/default_attachments/1574468519-REPETIDORES-RADIOAFICIONADOS-Espaoles-EA1DVY.pdf>
6. **Plan de banda IARU Región 1 de 430-440 MHz** (Davos 2005), publicado por la
   URE: canales de salida de repetidor **438,650-439,425 MHz**, espaciado de
   25 kHz, desplazamiento de 7,6 MHz; entradas 431,050-431,825 MHz:
   <https://ea8nq.ure.es/bandas/pla_430m.htm>
7. **PMR446**: Decisión **ERC/DEC/(98)25** (8 canales analógicos de 446,00625 a
   446,09375 MHz, 500 mW p.i.r.e.) y **ECC/DEC/(15)05**, que amplía la banda a
   16 canales (hasta 446,19375 MHz, analógicos y digitales), con separación de
   12,5 kHz; requisitos técnicos en ETSI EN 300 296 y EN 303 405.

### Qué convención se ha elegido y por qué

Hay dos numeraciones en circulación para los mismos canales:

- la **antigua española**, que numera la *entrada* en VHF (`R0` = 145,000) y la
  *salida* en UHF (`R70` = 438,650);
- la **IARU/actual**, que numera el canal por su frecuencia (`RV48` = 145,600,
  `RU692` = 438,650) o que numera la salida en VHF (`R0` = 145,600 en el listado
  de la URE).

Se ha elegido la **española del listado de la URE y del BOE**, que es la que el
operador ve en los listados y en las revistas: **R0..R7 son salidas de 145,600 a
145,775 MHz** y **U0..U100 son salidas desde 438,650 MHz hacia arriba**. En los
dos casos la entrada es la salida menos 600 kHz o 7,6 MHz. `docs/ZONES.md` y los
comentarios de `zones.c` citan la fuente de cada número; el test
`tests/unit/zones.cpp` comprueba las 8 + 32 + 16 frecuencias contra el plan.

## 3. La tabla U y los 12,5 kHz

El plan legal de 70 cm son **32 canales de 25 kHz** (438,650-439,425). La tabla
`U0..U100` va de 12,5 en 12,5 kHz desde 438,650 MHz, de modo que:

- los índices pares (`U0, U2, … U62`) son **los 32 canales del BOE**, con su
  pareja de 7,6 MHz;
- los impares son los **canales intercalados** a 12,5 kHz. En VHF se usan de
  hecho (el listado catalán publica `R0.5` = 145,6125 MHz junto a `R0`), en UHF
  quedan fuera del plan legal de 25 kHz y por eso van marcados con
  `ZONE_FLAG_INTERLEAVED`: la pantalla puede decir «fuera de plan» en vez de
  presentarlos como equivalentes;
- `U100` = 439,900 MHz, todavía dentro del segmento 438-440 MHz. Se eligió el
  paso de 12,5 kHz precisamente para que los 101 canales que pide la numeración
  quepan en la banda: con 25 kHz, `U100` caería en 441,150 MHz.

La decisión, el cálculo y la alternativa descartada están aquí para que nadie
tenga que deducirlos del código.

## 4. API

```c
int            zones_count(void);
const zone_t  *zones_get(uint8_t id);            /* ZONE_VHF_REPEATERS, ... */
const zone_t  *zones_at(int i);
int            zones_channels(const zone_t *z);
int            zones_channel(uint8_t id, uint16_t index, zone_channel_t *out);
const char    *zones_channelName(uint8_t id, uint16_t index, char *buf, size_t cap);
bool           zones_channelValid(const zone_channel_t *zc);
int            zones_toChannel(uint8_t id, uint16_t index, channel_t *out);
int            zones_toStore(uint8_t id, uint16_t index, chstore_channel_t *out);
void           zones_setShiftInverted(bool inverted);
bool           zones_shiftInverted(void);
```

`zone_channel_t` lleva nombre, RX, TX, shift, modo, ancho de banda, potencia y
flags; `zone_t` es el plan del que se calcula. **La dirección del shift es
configurable**: `zones_setShiftInverted(true)` la invierte en todas las zonas de
repetidora (PMR, que es símplex, se queda en cero) para los planes que se leen
al revés.

`zones_toStore()` deja el registro listo para `chstore_add()`: nombre, RX/TX,
potencia, modo, flag de símplex. El destino DMR queda a `NONE` porque un canal
de repetidora es FM; el color code y el timeslot van a 1/TS1.

## 5. Límites: se avisa, no se bloquea

`zones_channelValid()` sólo rechaza lo que el radio no puede hacer (frecuencia
fuera de la cobertura del BK4819, potencia 0 o > 5 W) y la potencia de PMR
distinta de 500 mW, que no es un límite de software sino la definición del
canal. Todo lo demás —un canal intercalado, una repetidora fuera del plan
nacional— se puede programar y se marca con un flag para que la pantalla lo
diga. La regla es la de siempre en este firmware: la frecuencia es
responsabilidad del operador, el firmware informa.

## 6. Pruebas

`tests/unit/zones.cpp` (host, Catch2):

- las tres zonas existen, con 8, 101 y 16 canales y los índices fuera de rango
  rechazados;
- R0..R7: RX 145,600 + 25 kHz·n, TX 600 kHz por debajo, nombres `R0`..`R7`,
  flags de repetidora, sin intercalados;
- U0..U100: RX 438,650 + 12,5 kHz·n, TX 7,6 MHz por debajo, los pares son los 32
  del BOE y los impares llevan `ZONE_FLAG_INTERLEAVED`, U100 = 439,900;
- PMR1..PMR16: 446,00625 + 12,5 kHz·n, símplex, **500 mW**, y 5 W rechazados;
- la inversión del shift cambia VHF y UHF y no toca PMR;
- `zones_toStore()` produce registros que `chstore_valid()` acepta.
