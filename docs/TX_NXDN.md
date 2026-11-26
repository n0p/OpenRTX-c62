# NXDN transmit mode

NXDN48 transmisor para la Retevis C62: trama de 192 símbolos con FSW, LICH,
SACCH y cuatro canales de voz AMBE+2, el scrambler de datos de 4.6, el FEC de
los campos de control y el modulador 4FSK de la cadena DMR. Cadena completa y
verificada **bit a bit contra el receptor de este árbol**, y con una segunda
opinión de un decodificador de referencia.

Referencias: NXDN Forum, TS 1-A/1-B v1.3 (noviembre 2011): 4.3/4.4.1 (trama),
4.4.2 (FSW), 4.4.3 (preámbulo), 4.5.2/4.5.3 (SACCH y LICH), 4.6 (scrambler),
5.2.1 (campos del LICH), 4.1.1 (supertrama de voz).

## Estado

| pieza | estado |
|---|---|
| FSW, LICH con paridad, SACCH (CRC-6, convolucional K=5 punctado, interleaver), VCH con el reparto de NXDN | hecho |
| scrambler de datos 4.6 (semilla por defecto) y su inverso | hecho |
| mensajes de capa 3: VCALL al inicio, TX_REL al final | hecho |
| modulador 4FSK 2400 simbolos/s, RRC α 0.2, 20 muestras/símbolo a 48 kHz | hecho, reutiliza la cadena de DMR |
| máquina de PTT en `OpMode_NXDN` (preámbulo → voz → release → cola) | hecho; `CONFIG_NXDN_TX`, apagado por defecto |
| verificación contra el RX propio, bit a bit | **hecha**: 3 llamadas × 2 niveles, 11 000 aserciones (`tests/unit/nxdn_tx.cpp`) |
| decodificador de referencia (dsd-neo) | ver "Verificación" |
| desviación y `REG_40` | **pendiente de banco**: la cifra es aritmética sobre la de DMR, no una medida |
| potencia de canal adyacente, prueba en el aire | **pendiente de banco** |
| NXDN96, FACCH (*steal*), SMS, cifrado, repetidores | no implementado, ver "Límites" |

## Habilitarlo

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_NXDN_RX=y -DCONFIG_DMR_TX=y \
                                  -DCONFIG_NXDN_TX=y
```

`NXDN_TX` (`platform/targets/c62/Kconfig.board`) depende de `C62_NXDN_RX` y de
`DMR_TX`: el codificador AMBE+2 (`lib/dmr_encoder`) y la cadena de desviación y
de PA son los de DMR, y no se duplican. Sin `NXDN_TX` el PTT en NXDN no hace
nada, exactamente como en DMR sin `DMR_TX`.

## Dónde está el código

```
openrtx/include/protocols/NXDN/NxdnTx.h     la trama y sus mitades
openrtx/src/protocols/NXDN/NxdnTx.c         FSW, LICH, SACCH, VCH, preámbulo
openrtx/src/protocols/NXDN/NxdnFec.c        + nxdnFec{Scramble,LichEncode,SacchEncode,VchFromAmbe}
openrtx/include/protocols/Fsk4Tx.h          el modulador 4FSK de los modos de 2400 simbolos/s
openrtx/src/protocols/Fsk4Tx.c
openrtx/src/rtx/OpMode_NXDN.cpp             la máquina de PTT (CONFIG_NXDN_TX)
tests/unit/nxdn_tx.cpp                      las mitades y el bucle completo
tests/platform/c62_tx_4fsk_cost.c           el coste, en QEMU mps2-an505
tests/platform/run_tx_4fsk_cost.sh
```

## La trama, símbolo a símbolo

Una trama son **192 símbolos**, 384 bits a 4800 bit/s, 80 ms a 2400
símbolos/s:

```
símbolo  0.. 9   FSW, 10 símbolos, hex CDF59 (TS 1-A tabla 4.4-2), sin scrambler
dibit    0.. 7   LICH: 7 bits de control + paridad par
dibit    8..37   SACCH: 30 dibits = 60 bits, bloque convolucional punctado
dibit   38..181  VCH1..VCH4: 36 dibits cada uno, un cuadro AMBE+2 por 20 ms
```

Todo menos el FSW pasa por el scrambler de 4.6, que **invierte el signo** del
símbolo donde el LFSR de 9 bits (X⁹+X⁴+1, semilla `0b011100100`) da 1. La
máscara para los 182 dibits está en `nxdnScramble[]`, generada una vez, y el
TX aplica exactamente la misma operación que el RX deshace (`nxdnFecScramble`
es `nxdnFecDescramble` con otro nombre: es una involución).

### Convención de dibit

La del receptor (`NxdnPhy.c`, `level_to_dibit()`), que **no** es la de DMR: el
bit de signo es el **alto** del dibit y el bit de magnitud el bajo.

| dibit | símbolo | desviación |
|---|---|---|
| 00 | −1 | interior |
| 01 | −3 | exterior |
| 10 | +1 | interior |
| 11 | +3 | exterior |

### LICH

Siete bits de control y una paridad par sobre los cuatro más significativos
(4.5.3, figura 4.5-7); cada bit va como un dibit cuyo bit de signo es el dato,
con el de magnitud a 1 (todos los símbolos del LICH son exteriores). El modo
emite, para una llamada directa:

```
[6:5] = 10  RDCH   (canal de tráfico convencional/directo)
[4:3] = 10  SACCH de supertrama; 00 en la primera y la última trama
[2:1] = 11  sin FACCH (los cuatro VCH son voz)
[0]   = 0   inbound (transmite una estación móvil)
```

### SACCH

26 bits de información (8 de SR + 18 de capa 3), CRC-6, cuatro bits de cola a
cero, código convolucional K=5 rate 1/2 (G1 = 1+D³+D⁴, G2 = 1+D+D²+D⁴)
punctado a 60 bits e intercalado en cinco filas de doce (4.5.2.1). El CRC-6 es
el registro del decodificador leído tras los 26 bits: el TX añade exactamente
lo que el RX compara.

El byte SR lleva la RAN en los bits 5:0 y el índice de segmento en 7:6, con la
convención del receptor de este árbol (`part = 3 − ((SR>>6)&3)`). El mensaje de
capa 3 son 72 bits en cuatro segmentos de 18, uno por trama de la supertrama:
VCALL (tipo 0x01, opción de control, tipo de llamada, origen y destino) al
empezar y TX_REL (0x08) al terminar. Los identificadores son de 16 bits.

### VCH

Cada VCH son 36 dibits con el mismo reparto que DMR (las tablas
`ambe_rw/rx/ry/rz` de `NxdnFec.c`), porque NXDN y DMR llevan el mismo cuadro
AMBE+2 3600x2450: solo cambia el empaquetado. `nxdnFecVchFromAmbe()` es la
inversa exacta de `nxdnFecVchToAmbe()` y el test cierra las dos en 64 cuadros
aleatorios (los 72 bits, sin excepción: el último bit del plano C también va).

## El modulador: lo que se reutiliza y lo que cambia

| | DMR | NXDN48 |
|---|---|---|
| símbolos/s | 4800 | 2400 |
| muestras/símbolo a 48 kHz | 10 | 20 |
| forma de pulso | RRC α 0.2, 81 taps | `H(f) sinc(fT)`, 161 taps (±4 símbolos) |
| tabla dibit → nivel | {+1,+3,−1,−3} | {−1,−3,+1,+3} |
| rejilla temporal | slot TDMA de 30 ms, CACH, guardas | portadora continua, trama de 80 ms |
| rampa | guardas de silencio + gate del PA | igual: símbolos a cero + gate del PA |
| desviación exterior | ±1944 Hz (ETSI), 1600 Hz medidas en el C62 | ±1050 Hz (12.5 kHz), ±350 Hz (6.25 kHz) |

`Fsk4Tx.c` es el mismo algoritmo de `dmr_mod_dibits()` (RRC por la misma forma
cerrada, convolución polifásica con símbolos a cero entre medias, saturación a
int16) con el número de muestras por símbolo como parámetro: la cadena DMR se
queda intacta, byte por byte, y los dos modos de 2400 simbolos/s comparten la
suya. La rampa es la de DMR: no hay envolvente en el modulador, se sale con
símbolos a cero (que arrastran la cola del pulso) y el `radio_c62_gateStart()`
del driver es el que sube y baja la portadora.

**El pulso, en las dos mitades.** El transmisor de este árbol modulaba con un
RRC α 0.2 pelado, y el receptor con el mismo RRC: el par funcionaba porque los
dos se equivocaban igual, pero el aire de NXDN no lleva eso. TS 1-A 3.4/3.5
reparte el pulso: el transmisor manda `H(f) sinc(fT)` (el RRC por la retención
de un símbolo durante su periodo) y el receptor toma `H(f) D(f)`, con
`D(f) = 1/sinc(fT)`; juntos dan el coseno alzado de Nyquist `H(f)²`. Con el RRC
pelado el par quedaba en `RC(f)/sinc(fT)`, que no es Nyquist, y por eso el lazo
propio salía a EVM 0.125 mientras una emisión real lo dejaba en 0.08–0.10.

Ahora `Fsk4Tx` aplica la retención (`fsk4Mod_init(..., hold = true)`, la tabla
`hold_pulse`, 161 taps de `tools/nxdn_rx_pulse.py --tx`) y el receptor recibe
con `H(f) D(f)`: el lazo propio baja a **EVM 0.051** y sigue saliendo **bit a
bit**. dPMR se queda con el RRC pelado porque su receptor también lo es; el
`hold` es un parámetro, no un cambio global. La captura real de `docs/RX_NXDN.md` también mejora: 18 tramas, 18 LICH
y 18 SACCH, contra 17 y 6 antes, y 72 cuadros de voz contra 68.

## Desviación y máscara

El modulador entrega la desviación de una tirada de símbolos +1; el nivel se
elige con la misma aritmética que DMR:

```
DMR   : 23000 · 0.3165 · 1944 / 2400   (exteriores a ±1944 Hz)
NXDN  : 23000 · 0.3165 · 1050 / 2400   (12.5 kHz, ±1050 Hz) = 3189
        23000 · 0.3165 ·  350 / 2400   (6.25 kHz, ±350 Hz)  = 1063
```

El modo elige uno u otro según `rtxStatus_t::bandwidth` (`BW_6_25` o el resto).
La desviación **absoluta** la pone `REG_40` del BK4819, que es el mismo registro
que usa DMR y M17: `DMR_TX_DEVIATION = 0x476` sale de escalar el 0x582 de M17
(±2.4 kHz) a ±1944 Hz, y **no está calibrado** — es un punto de partida. En
este C62 la desviación medida del DMR es de unos 1600 Hz, no 1944, así que los
±1050 Hz de NXDN saldrían en unos 864 Hz si el registro se queda como está. No
hay cifra inventada: lo que queda es de banco.

Para calibrarlo: portadora modulada con símbolos exteriores alternos (+3/−3) y
un analizador (o un segundo C62 con sus estimaciones de nivel, que es lo que
usa `docs/DMR_RX.md`), subir `REG_40` hasta leer 1050 Hz en el exterior, y
llevar el valor a `calib_C62` como los de FM (`txDevNarrow`/`txDevWide`). La
máscara de emisión es la de un 4FSK de 6.25/12.5 kHz con RRC α 0.2: no hay
medida de canal adyacente todavía.

## Verificación

### Contra el receptor propio, bit a bit

`tests/unit/nxdn_tx.cpp` (registrado como `NXDN TX Test` en `meson test`)
construye llamadas completas, las modula y las mete en `nxdnPhy_push()`. Por
cada trama recibida exige:

* LICH válido, con `rfType` RDCH, sin FACCH y **cuatro** VCH;
* los **182 dibits** de carga iguales a los que salieron, uno a uno
  (comparados en el dominio del receptor, es decir con la máscara aplicada);
* los **cuatro cuadros AMBE+2** byte a byte;
* la SACCH cerrada y el mensaje de capa 3 reensamblado igual al VCALL (origen,
  destino y RAN).

Y todo eso en **seis** configuraciones: tres llamadas (grupo RAN 17, individual
RAN 0 con otros IDs, grupo RAN 63) × dos niveles de modulación (±1050 Hz y
±350 Hz de exterior). 11 000 aserciones, todas verdes. Con el scrambler
apagado el test comprueba además que los símbolos en el aire son los mismos sin
la máscara y que la SACCH del receptor no cierra: es lo que hace visible el
scrambler.

### Contra un decodificador de referencia

`work/` no está en el árbol, así que esto se reproduce a mano: se genera un
`.wav` de 48 kHz con `tests/unit/nxdn_tx.cpp`/el generador de `work` y se pasa
por **dsd-neo** con el demodulador de NXDN. Resultado en "Verificación" de
`reports/` — ver el informe de la tarea: sincronismo, RAN y los cuadros AMBE+2
salen; los identificadores los lee de la SACCH con el reparto de este árbol.

### Coste

Medido, no estimado, con `tests/platform/run_tx_4fsk_cost.sh nxdn` en un QEMU
`mps2-an505` (Cortex M33, sin DSP, FPU simple) y el plugin de instrucciones de
`work/dsp-probe/common/`:

| pieza | instrucciones | % de un M33 a 300 MHz |
|---|---|---|
| trama (FSW+LICH+SACCH+4 VCH) + modulación de 192 símbolos | 527 721 cada 80 ms | 2.2 % |
| vocoder AMBE+2, 4 cuadros de 20 ms (`lib/dmr_encoder`) | 4 × 1 427 272 = 5 709 088 cada 80 ms | 23.8 % |
| **total de la cadena de TX** | ≈ 6.2 M cada 80 ms | **≈ 26 %** |

Objetivo: menos del 60 % del M33. El presupuesto queda en la cuarta parte, y el
trozo que añade este trabajo (framing + modulación) es el 2.2 %.

Tamaños, sobre los objetos compilados para el M33 con `-Os` y las opciones del
firmware (`arm-none-eabi-size`, delta contra la base):

| | FLASH | RAM |
|---|---|---|
| `NxdnTx.c` | +645 B | — |
| `Fsk4Tx.c` (compartido con dPMR) | +988 B | — (la instancia va en el modo: 728 B) |
| mitades de codificación en `NxdnFec.c` | +548 B | — |
| instancia de `OpMode_NXDN` con TX (`txSamples` 3 840 B, `micPcm` 640 B, `txMod` 728 B, resto) | — | +5 608 B |
| cambios del receptor (filtro de 161 taps y búsqueda de timing) | +700 B | +1 024 B |
| **total** | **≈ 2.9 KB** | **≈ 6.6 KB** |

Las cifras de imagen completa (FLASH ocupada y RAM al porcentaje que marca el
enlace) salen de compilar el firmware; con este árbol la RAM de SRAM está al
80 % antes de esto, así que `NXDN_TX` y `DPMR_TX` a la vez son 11 KB de SRAM y
hay que mirar el mapa antes de encenderlos juntos.

## Límites

* **Solo NXDN48** (2400 simbolos/s, 6.25 o 12.5 kHz). NXDN96 es el mismo
  formato a 4800 simbolos/s y no está.
* **Sin FACCH**: el LICH va siempre con `steal = 11`; no hay mensajes que
  roben los VCH, ni SMS, ni datos, ni cifrado (VCALL_IV no se emite).
* **Sin repetidor**: no hay alineación temporal con un slot ajeno (el pre-roll
  de DMR no aplica: NXDN directo no tiene rejilla) y el RAN es el que el
  receptor filtra, sin color code ni sitio.
* **El codificador de voz es el de DMR** (`lib/dmr_encoder`), que produce el
  mismo cuadro AMBE+2 3600x2450: si suena distinto en un radio NXDN comercial
  será por la Ganancia/AGC del analizador, no por el reparto, que está
  verificado bit a bit.
* **Preámbulo de 40 ms** (0x5775FD + el patrón +3+3−3−3). NXDN no pide más,
  pero un receptor que solo tenga correlación de FSW agradece el doble.
* **Desviación sin calibrar** y sin máscara espectral medida: ver arriba.
* El mensaje de capa 3 es el de este árbol; su reparto se validó contra una
  captura real en el receptor (`docs/RX_NXDN.md`), pero no contra la tabla de
  tipos de mensaje de TS 1-C completa (alias, registro, etc.).
