# dPMR transmit mode

Transmisor dPMR (ETSI TS 102 490 V1.9.1) para la Retevis C62: trama de
cabecera, supertramas de cuatro tramas de carga con su CCH, trama END, el FEC
de los campos (CRC-7/CRC-8, Hamming (12,8), intercaladores de 12×6 y 12×10), el
scrambler X⁹+X⁵+1 y el modulador 4FSK de la cadena DMR. Verificado **bit a bit
contra el receptor de este árbol** y **contra DSDcc**, que es el decodificador
de referencia de este modo.

## Estado

| pieza | estado |
|---|---|
| trama de cabecera (P, FS1, HI0, CC, HI1), 192 símbolos | hecho |
| tramas de carga (FS2 o CC, CCH, 4×TCH), 192 símbolos, fn 0..3 | hecho |
| trama END (FS3 + 72 bits = el mismo bloque de 36 dos veces) | hecho |
| CCH con CRC-7, Hamming (12,8), intercalador 12×6 y scrambler | hecho, cerrado contra su decodificador |
| HI0/HI1 con CRC-8, intercalador 12×10 | hecho |
| voz AMBE+2 3600x2450 con el reparto de dPMR en el TCH | hecho (`lib/dmr_encoder`) |
| máquina de PTT en `OpMode_DPMR` (preámbulo → cabecera → supertramas → END → cola) | hecho, `CONFIG_DPMR_TX`, apagado por defecto |
| verificación contra el RX propio, bit a bit | **hecha**: 2 llamadas × 2 niveles, 2 288 aserciones (`tests/unit/dpmr_tx.cpp`) |
| decodificador de referencia (DSDcc) | **hecho**: cabecera, IDs, canal y 96/96 cuadros AMBE |
| desviación y `REG_40` | **pendiente de banco** (aritmética sobre DMR, no medida) |
| LBT, datos (tipo 1/2/3), ARQ, modo con estado, cubierta de 6.25 kHz real | no implementado, ver "Límites" |

## Lo que dPMR no es

dPMR es **FDMA**: no hay slots de 40 ms ni una segunda ranura que escuchar. El
encargo hablaba de "trama de 80 ms con dos slots"; los dos slots de 40 ms son
de DMR y de NXDN. Una portadora lleva una llamada, y su estructura (cláusula
4.2.2) es:

```
trama de carga   384 bits, 80 ms:   FS2 o CC (24) | CCH (72) | TCH (72) × 4
supertrama       4 tramas de carga, 320 ms
trama de cabecera 384 bits:         P (≥72) | FS1 (48) | HI0 (120) | CC (24) | HI1 (120)
trama END         96 bits:          FS3 (24) | END (72)
```

## Habilitarlo

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DPMR_RX=y -DCONFIG_DMR_TX=y \
                                  -DCONFIG_DPMR_TX=y
```

`DPMR_TX` depende de `C62_DPMR_RX` y de `DMR_TX` por el codificador AMBE+2 y la
cadena de desviación y PA, igual que `NXDN_TX`. Sin él, el PTT en dPMR no hace
nada.

## Dónde está el código

```
openrtx/include/protocols/DPMR/DpmrTx.h   las tramas
openrtx/src/protocols/DPMR/DpmrTx.c       cabecera, carga, END, preámbulo
openrtx/src/protocols/DPMR/DpmrDefs.c     + dpmrFec_{cch,hi,end,voiceFrame}Encode
openrtx/include/protocols/Fsk4Tx.h        el modulador 4FSK de 2400 simbolos/s
openrtx/src/rtx/OpMode_DPMR.cpp           la máquina de PTT (CONFIG_DPMR_TX)
tests/unit/dpmr_tx.cpp                    las mitades y el bucle completo
tests/platform/c62_tx_4fsk_cost.c         el coste, en QEMU mps2-an505
```

## La cadena, campo a campo

Cada campo es el decodificador del receptor al revés: los bits de información
con su CRC, troceados en bloques Hamming (12,8) (que protegen un octeto con
cuatro paridades), escritos en el mapa del de-intercalador al revés,
scramblados desde el estado de reposo (`0x3FF`) y empaquetados de dos en dos
bits por dibit, el más significativo primero.

### CCH (72 bits por trama de carga)

```
41 bits:  FN (2) | ID12 (12) | M (3) | V (2) | F (2) | SLD (18)
 7 bits:  CRC-7 (X⁷+X³+1)
48 bits -> 6 bloques Hamming (12,8) -> 72 bits -> intercalador 12×6 -> scrambler -> 36 dibits
```

`FN` es el número de trama 0..3. `ID12` es el trozo de 12 bits de dirección que
corresponde a cada trama: 0 y 1 llevan la dirección llamada (mitad alta y
baja) y 2 y 3 la propia. Así el receptor reconstruye las dos direcciones de 24
bits a partir de las cuatro tramas de una supertrama sin depender de la
cabecera.

### HI0 / HI1 (60 símbolos cada uno)

```
72 bits:  HT (4) | CID (24) | OID (24) | M (3) | V (2) | F (2) | 2 reservados | CI (11)
 8 bits:  CRC-8 (X⁸+X²+X+1)
80 bits -> 10 bloques Hamming (12,8) -> 120 bits -> intercalador 12×10 -> scrambler -> 60 dibits
```

La trama de cabecera lleva el mismo campo dos veces, con el código de canal en
medio, para que el receptor lo tenga aunque una de las dos copias no cierre.

### Canal

Cada frecuencia de 6.25 kHz tiene su código de 12 bits (cláusula 6.1.5, tablas
6.1 y 6.2) y va como 12 símbolos exteriores `(c, 1)`: el bit de signo es el del
código y el de magnitud siempre 1, lo que además sirve de referencia de nivel.
El modo lo saca de `dpmrDefs_channelCodeBits(frecuencia, grupo A)`, el mismo
que el receptor lee. Las tramas 1 y 3 (índice 0 y 2) abren con FS1... con FS2,
y las 2 y 4 con el código de canal.

### TCH

288 bits = cuatro cuadros AMBE+2 de 72 bits, cada uno enviado como cuatro
corrientes intercaladas de 18: el bit canónico `t = 4k + j` sale como bit
transmitido `18j + k`. Es exactamente el reparto que deshace
`dpmrFec_voiceFrame()`, y el test lo cierra en 64 cuadros aleatorios.

### END

17 bits (`ET`, `ARQ`, `WAIT`, estado) + CRC-7 → 3 bloques Hamming → 36 bits,
scrambados desde el reposo, y **el bloque entero repetido**: 72 bits, 36
dibits. Cada copia se scrambla por separado (cláusula 9.6); con un LFSR
corrido por los 72 bits el receptor solo decodifica una mitad de cada dos.

## El modulador: lo que se reutiliza y lo que cambia

| | DMR | dPMR |
|---|---|---|
| símbolos/s | 4800 | 2400 |
| muestras/símbolo a 48 kHz | 10 | 20 |
| pulso | RRC α 0.2 | RRC α 0.2 (161 taps) |
| tabla dibit → nivel | {+1,+3,−1,−3} | **la misma** (tabla 12.1 = la de DMR) |
| estructura | slot TDMA, CACH, guardas | FDMA, portadora continua |
| desviación exterior | ±1944 Hz | ±1050 Hz (h = 0.29) |

Aquí la tabla de dibits coincide con la de DMR, así que el modulador es
literalmente el mismo con 20 muestras por símbolo (`Fsk4Tx.c`); la rampa de
salida es la de DMR: símbolos a cero que arrastran la cola del pulso y el gate
del PA del driver.

## Desviación y máscara

```
DPMR_TX_LEVEL = 23000 · 0.3165 · 1050 / 2400 = 3189   (±1050 Hz exteriores)
```

La cifra sale de escalar el nivel de DMR (±1944 Hz) a los ±1050 Hz de dPMR; la
desviación absoluta es `REG_40` del BK4819, el mismo registro de DMR y M17, con
`0x476` como **punto de partida, no una calibración**. En el C62 la desviación
medida del DMR es de ~1600 Hz en vez de 1944, así que estos ±1050 saldrían en
~864 Hz. Calibración pendiente de banco: símbolos exteriores alternos y un
analizador, o un segundo C62 con sus estimaciones de nivel, subiendo `REG_40`
hasta leer 1050 Hz y llevando el valor a `calib_C62`. Máscara espectral (canal
adyacente) también pendiente.

Una diferencia con DMR que sí es de fondo: dPMR vive en canales de 6.25 kHz y
el C62 recibe con el filtro de FI de 12.5 kHz. La emisión no cambia por eso
(la señal ocupa 3 kHz), pero la comprobación en el aire debería hacerse con el
modo de 6.25 kHz del receptor (`docs/RX_DPMR.md`).

## Verificación

### Contra el receptor propio, bit a bit

`tests/unit/dpmr_tx.cpp` (`dPMR TX Test` en `meson test`) construye una
transmisión completa — preámbulo, cabecera, ocho tramas de carga y END — la
modula y la mete en `dpmrPhy_push()`. Exige, por cada trama de carga:

* CCH con CRC válido, `FN` correcto y el `ID12` que le toca (mitad alta/baja
  de la dirección llamada o propia);
* los **cuatro cuadros AMBE+2**, byte a byte, y que el índice de trama
  transmitida se reconozca por su contenido (36/36 bits);
* el código de canal correcto en las tramas que abren con él;
* la cabecera con `HT`, `CID`, `OID`, modo y `CI` exactos, y la trama END
  cerrada.

En dos llamadas (IDs distintos / códigos de canal distintos) × dos niveles
(±1050 Hz y ±350 Hz): 2 288 aserciones, todas verdes. Las doce tramas de cada
transmisión salen **bit a bit**, con EVM de 0.02 a 0.09 en el receptor.

### Contra un decodificador de referencia

**DSDcc** (`dsdccx`, del proyecto de f4exb, el decodificador con el que se
validó la captura de `docs/RX_DPMR.md`), con el fichero de 48 kHz generado por
la cadena:

```
./build/dsdccx -i - -o - -fm -d 0 -v 2 < dpmr_tx.raw
```

```
Sync: +dPMR   good sync found: 20 symbol 451
DSDdPMR::processHeader: start
DSDdPMR::processHIn: HT: 0 CID: 00393D OID: 000135 M: 0 F: 01
DSDdPMR::processColourCode: 221
DSDdPMR::processSuperFrame: start
96 x MBE:
```

Es decir: sincronismo, cabecera con **HT 0 (inicio), CID 0x00393D, OID
0x000135, modo 0, formato 01**, el código de canal **221 = 0x0DD** y **96
cuadros AMBE decodificados de 96** (24 tramas × 4). Todo coincide con lo que
se transmitió.

### Coste

`tests/platform/run_tx_4fsk_cost.sh dpmr` en QEMU `mps2-an505` (Cortex M33):

| pieza | instrucciones | % de un M33 a 300 MHz |
|---|---|---|
| trama (CCH + 4 TCH) + modulación de 192 símbolos | 523 000 cada 80 ms | 2.2 % |
| cabecera (HI0 + HI1 + canal) + preámbulo, una vez | 1 048 500 | 0.4 % de una llamada de 2 s |
| vocoder AMBE+2, 4 × 20 ms | 5 709 088 cada 80 ms | 23.8 % |
| **total de la cadena de TX** | ≈ 6.2 M cada 80 ms | **≈ 26 %** |

Objetivo, menos del 60 % del M33: se queda en la cuarta parte.

Tamaños sobre los objetos del M33 con `-Os` (delta contra la base):

| | FLASH | RAM |
|---|---|---|
| `DpmrTx.c` | +705 B | — |
| `Fsk4Tx.c` (compartido con NXDN) | +988 B | — (instancia en el modo: 728 B) |
| mitades de codificación en `DpmrDefs.c` | +814 B | — |
| instancia de `OpMode_DPMR` con TX | — | +5 536 B |
| cambios del receptor (fracción fina, cabecera→trama siguiente, `channelCode` a 16 bits) | +128 B | — |
| **total** | **≈ 2.6 KB** | **≈ 5.5 KB** |

Con `NXDN_TX` y `DPMR_TX` a la vez son ~11 KB de SRAM: la SRAM del C62 está al
80 % antes de esto, así que hay que mirar el mapa (los buffers que van al
stream de audio son DMA y **no** pueden ir a PSRAM; por eso la escritura es de
20 ms y no de 40).

## Límites

* **Sin LBT**: no se escucha antes de emitir. dPMR comparte canal por
  cortesía; el modo emite cuando el operador aprieta el PTT.
* **Sin datos**: ni tipo 1/2/3, ni ARQ, ni supertramas de datos, ni mensajes de
  estado en el END (se emite `ET = 00`, los 5 bits de estado son ficticios).
  El campo SLD va a cero.
* **Direccionamiento**: si el destino es un usuario se emite formato 01
  (*peer to peer*); si es un grupo, formato 00 (*all call*), que es lo que
  entiende un receptor sin gestión de grupos. No hay direccionamiento de grupo
  con estado.
* **Un solo canal de 6.25 kHz por llamada**, sin salto ni gestión de canal; el
  código de canal se resuelve por frecuencia (grupo A). Fuera de la tabla de
  6.25 kHz del estándar el código va a cero y las tramas pares salen con el
  patrón 000…, que el receptor acepta pero un radio comercial puede ignorar.
* **El preámbulo son dos tramas (160 ms)**, más de los 72 bits que pide el
  estándar: con una sola, este receptor encuentra FS1; con ninguna, no. Un
  radio comercial no necesita tanto, y el precio es 160 ms de retardo al
  empezar a hablar.
* **La voz es el cuadro AMBE+2 de DMR** (`lib/dmr_encoder`), que es el mismo
  que lleva dPMR: el reparto en el TCH está verificado bit a bit.
* **Desviación sin calibrar** y sin máscara medida.
