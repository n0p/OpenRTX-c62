# Transmisión AM en banda aérea

**Ámbito:** firmware de la C62 (OpenRTX, Zephyr, Cortex-M33). Modo de transmisión
AM con doble banda lateral y portadora (DSB-FC) en 118.000-136.975 MHz, opción de
compilación `C62_AIR_TX` (apagada por defecto).

La banda es de uso aeronáutico: transmitir en ella no lo ampara ninguna licencia
de aficionado, sino una autorización de estación aeronáutica de la administración
correspondiente (AESA en España). El firmware no lo impide —la responsabilidad es
del operador—, pero no deja que ocurra por accidente: la primera pulsación del
PTT en la banda **no transmite**, saca un aviso en pantalla, y el modo se arma
sólo con una segunda pulsación delante del aviso. Sección 5.

- `openrtx/include/core/am_tx.h`, `openrtx/src/core/am_tx.c` — el modulador: banda
  de audio, nivel, limitador, profundidad, rampas y envolvente → ciclo de trabajo.
- `platform/targets/c62/air_tx.h`, `air_tx.c` — el camino de la C62: la PWM del
  APC, el temporizador que la dosifica a 8 kHz y el enganche con el códec.
- `platform/drivers/baseband/radio_C62.cpp` — la portadora: el BK4819 en TX sin
  audio, el PA y la calibración por frecuencia.
- `openrtx/src/rtx/OpMode_Airband.cpp`, `openrtx/src/core/airband.c` — el modo y
  la compuerta de confirmación.
- `tests/platform/c62_am_tx_sim.c` — el banco de medidas (host y QEMU).
- `tests/unit/am_tx.cpp` — las pruebas.

---

## 1. Por qué no hay AM en el chip, y qué hay en su lugar

El BK4819 es un transceptor de FM. Su lista de registros V3
(`datasheets/BK4819V3_Registers_List_20201218.pdf`) no tiene modo AM, ni
modulador de envolvente, ni convertidor que pueda variar la amplitud de la
portadora:

- **REG_40<12> y <11:0>** son «Enable RF TxDeviation» y la sensibilidad del
  varactor: el camino de TX del chip es un PLL modulado en fase/frecuencia. Con
  audio a cero, la portadora es una frecuencia fija — una portadora desnuda, que
  es exactamente lo que hace falta para modularla después;
- **REG_4B<5>** es el ALC, y el ALC es un control de nivel de **audio** (actúa
  sobre el DSP de TX, no sobre la etapa de RF);
- **REG_48<3:0>** es la ganancia del DAC de AF, también antes del modulador;
- **REG_36<15:8>** es lo único que toca la etapa de RF: «PA Bias output 0~3.2 V»
  en 8 bits, más REG_36<5:3>/<2:0> (PA Gain1/Gain2, 3 bits cada uno). Es un
  registro de configuración por SPI: no tiene entrada de audio, ni temporización
  definida, ni ancho de banda documentado; llevarlo a 8 kHz por SPI sería una
  modulación por registro, no por señal.

Conclusión, con la evidencia delante: **el chip no modula en amplitud**. Lo que
sí tiene la radio es un control de potencia del PA —el «APC»— que el firmware de
fábrica usa para elegir 1 W o 5 W. Ese control es analógico, continuo y
razonablemente rápido, y es donde se hace la AM: se modula la **alimentación/
polarización del PA** con la envolvente de la voz mientras el chip emite una
portadora sin desviación. Es modulación de alta cota (la del transmisor de AM
clásico), no un truco.

El firmware de fábrica **no transmite por debajo de 136.125 MHz**: su tabla de
calibración de banda tiene diez puntos de 136.125 a 173.975 MHz (VHF) y de
400.125 a 479.975 MHz (UHF), y la búsqueda de punto más cercano
(`band_cal_lookup` `0x18021754`) no tiene nada por debajo. Por eso la
calibración de potencia en la banda aérea **está extrapolada** (sección 6).

## 2. La ruta de señal

```
  micrófono
      |  ADC del códec CSK6, 48 kHz
      v
  core/am_tx.c        paso alto 300 Hz, paso bajo 3 kHz (4º orden),
      |               nivel + limitador, profundidad m, rampas
      |  una muestra cada 6 (8 kHz)
      v
  air_tx.c            anillo de 64 valores
      |  GPT canal 0, interrupción a 8 kHz
      v
  CHx_RELOAD[3]       PWM del APC, 100 kHz, 1000 cuentas por periodo
      |  GPIO_A_03 -> R131 1k -> C309 10n -> U21 (UMC4N) -> APCV   (VHF)
      |                                                  -> APCU   (UHF)
      v
  polarización de la cadena de PA  ->  envolvente de la portadora
      ^
      |  portadora sin modular
  BK4819 en TX  (REG_40 con desviación, pero audio = 0: el códec deja el bloque
                 a cero, así que lo que el chip transmite es una frecuencia fija)
```

Y la otra mitad, la que hace que el chip no module:

```
  micrófono -> worker de audio del códec -> csk6codec_setTxProcessor()
      |                                        |
      |  el modo toma la voz                    |  y deja el bloque a cero
      v                                        v
  core/am_tx.c                            BK4819 (MICN/MICP), sin audio
```

El enganche es el mismo que el del receptor AM (`csk6codec_setRxProcessor()`,
docs/RX_AIRBAND.md) pero en el otro sentido: `csk6codec_setTxProcessor()`
(`platform/drivers/audio/csk6_codec.c`) se llama sólo en el camino
`SOURCE_MIC -> SINK_RTX`, es decir, en el passthrough de micrófono de FM, y no en
los modos digitales, que inyectan su propia banda base por un stream.

### El APC en el esquema

Leído de `C62_Teardown/Schematics/C62_RF.pdf` (no medido; es el punto que más
falta por comprobar):

- **GPIO_A_03 → R131 (1 kΩ) → nodo con C309 (10 nF) → U21 (UMC4N, transistor
  digital dual) → APCV**. Es un filtro de primer orden de 15,9 kHz seguido de un
  buffer; **R132/C308 y U41** hacen lo mismo para **APCU**. APCV polariza la
  cadena de VHF y APCU la de UHF.
- **R1, 0,15 Ω, en la alimentación del PA**, con un amplificador diferencial
  (U1A/U1B, LM2904V, divisores de 150 kΩ) y una referencia ajustada por VR2 y
  Q1/Q9 (DTC114EE) que el propio APC modifica; la salida va por R152/Q12 a la
  polarización. Es un **lazo de corriente** alrededor del PA: si es rápido,
  recorta los picos de la envolvente y limita el ancho de banda de modulación; si
  es lento (los 470 pF de C129 con 220 kΩ de R14 lo dejan en ~1,5 kHz), actúa
  como limitador de corriente media y la modulación pasa. **Cuál de las dos cosas
  es se mide en el banco** (sección 7), y es el número que decide si el modo
  puede pasar de 400 Hz de audio.
- El PA es discreto (2SC4226 y un 0606E en la cadena de VHF), con la polarización
  común de APCV: es lo que hace que modular el APC module la amplitud.

### Por qué se escribe el registro del temporizador y no se usa `pwm_set_dt()`

El driver de Zephyr de la PWM (`csk/drivers/pwm/pwm_csk6.c`) convierte el ciclo
de trabajo a **porcentaje entero** (`uint8_t duty_cycle`), y el HAL hace
`low16bit = high16bit * duty / 100`. Con 101 niveles, una envolvente del 60 %
alrededor de una portadora del 33 % tendría 20 escalones en total: un zumbido
granular y salpicaduras a ambos lados del canal. El registro `CHx_RELOAD[3]` es
de 16 bits por semiperiodo, así que el modulador escribe **directamente**, una
cuenta por 0,1 % de ciclo (1000 niveles), y usa la carga en sombra
(`SHADOW_LOAD = 0x101 << 3`) para que el valor nuevo entre en el siguiente
periodo y no a mitad de uno. El driver sigue siendo el dueño del canal: fija
frecuencia, prescaler, pin y polaridad, y el nivel de portadora grueso sigue
saliendo de `pwm_set_dt()` y de la calibración de banda.

### La dosificación a 8 kHz

El worker de audio entrega milisegundos de audio (48 muestras) de golpe. Escribir
los 48 valores de ciclo "en cuanto se pueda", una vez por milisegundo, no
reproduce la envolvente: la comprime en el tiempo y luego la mantiene. Por eso la
base de tiempos es **hardware**: el canal 0 del mismo GPT corre como temporizador
periódico a 8 kHz (PCLK de 100 MHz, 12500 cuentas) y en su interrupción saca un
valor del anillo y lo escribe. El anillo tiene 64 valores (8 ms), cuatro veces el
bloque de audio, para absorber la latencia del worker; si se vacía, se mantiene
el último valor —un click a mitad de sílaba sería peor que 2 ms de retardo— y lo
que termina la transmisión son las rampas y el vigilante de TX, no un bloque
tardío.

El canal 0 se programa con el HAL directamente y no con el driver `counter` de
Zephyr: ese driver y el de la PWM no compilan juntos en este SoC
(`pwm_csk6.c` sólo define su función de init del GPT si el driver counter está
apagado), y aquí hace falta un canal, un periodo y un callback.

## 3. Registros y valores

| Registro / recurso | Valor | Para qué |
|---|---|---|
| REG_40<12>, <11:0> | desviación de FM, heredada | irrelevante: el audio que llega al chip es cero |
| REG_30 | `bk4819_enableTx()` (`0xC1FE`) | VCO, PLL, PA gain, DSP de TX |
| REG_43 | perfil AM del modo | el filtro de FI de RX sigue igual; no afecta a TX |
| REG_38/39 | frecuencia de TX, ajustada al raster 8,33 kHz | la portadora |
| GPIO_A_03 / GPT ch3 | PWM 100 kHz, 1000 cuentas | ciclo de trabajo = amplitud |
| `CHx_RELOAD[3]` | `(hi << 16) \| lo`, `hi+lo = 1000` | el valor de la envolvente, 0,1 % |
| `SHADOW_LOAD` | `0x101 << 3` | la carga entra en el siguiente periodo |
| GPT ch0 (temporizador) | 12500 cuentas = 8 kHz | dosifica la envolvente |
| PA_VHF_EN / PA_UHF_EN (BK4819 GPIO 2/3) | el de la banda | conmutación del PA |
| ALC_TX_LED_EN | 1 | LED de TX |

Nada de esto toca el perfil de recepción AM: al terminar la transmisión el modo
vuelve a `bk4819_setModulation(false)` y a su AGC.

## 4. La modulación

Todo en punto fijo, en `core/am_tx.c`, sin acceso al hardware: se puede medir en
el host y en QEMU.

- **Banda de audio**: paso alto de 300 Hz (2º orden, Butterworth) y paso bajo de
  3 kHz (4º orden), a 48 kHz, más el promedio de 6 muestras de la decimación.
  Respuesta medida (`tests/platform/c62_am_tx_sim.c`): 300 Hz −2,4 dB,
  1 kHz 0 dB, 2,7 kHz −3,0 dB, 3 kHz −4,9 dB, 4 kHz −16,3 dB, 6 kHz −37,4 dB.
  El canal de la banda es 300-2700 Hz y lo que podría ensuciar al vecino de
  8,33 kHz está 16 dB abajo.
- **Nivel**: seguidor de pico sobre la salida (ataque instantáneo, caída de
  16 ms), ventana entre 0,80 y 0,95 de fondo de escala, ganancia de −12 a +36 dB,
  ataque de 10 dB en 9,6 ms y liberación de 10 dB en 640 ms. Es lo que hace que
  la profundidad no dependa de si el operador habla fuerte o bajo.
- **Limitador**: lineal hasta 0,98 de fondo de escala y 4:1 por encima, con tope
  en fondo de escala. Nunca recorta: lo que el AGC no llega a bajar se comprime.
- **Profundidad**: `duty = dutyPortadora + pendiente · m · x`, con `m` en 30-90 %
  (ajuste `DEPTH` del menú rápido y `settings.am_depth`), `x` la voz limitada a
  ±1 y `pendiente` la calibración. La **portadora no puede caer a cero**:
  `duty` se recorta a la ventana `[dutyMin, dutyMax]`, y si la ventana no da para
  la profundidad pedida, el modulador **baja la profundidad** (y `am_tx_getDepth()`
  dice a cuánto) en vez de recortar los picos — una envolvente plana arriba
  salpica al canal de al lado, una modulación más superficial no.
- **Rampas**: la portadora sube de 0 a su valor en 16 ms y la profundidad entra
  en 20 ms; al descolgar, la portadora baja en 16 ms antes de que se caiga el PA.
  La rampa de bajada es síncrona (128 pasos de 125 µs) porque cuando el modo
  descolga el camino del micrófono ya se está cerrando y una rampa que dependa de
  bloques de audio no terminaría nunca.

Medidas del banco, portadora de 330 cuentas (33 %) y ventana 10-190 %:

```
profundidad pedida 60 %:  -20 dBFS -> 58 %   -5 dBFS -> 58 %   (0,50 y 0,44 de portadora en el mínimo)
profundidad pedida 90 %:  -20 dBFS -> 95 %   -5 dBFS -> 89 %   (0,15 de portadora en el mínimo)
profundidad pedida 30 %:  -20 dBFS -> 27 %   -5 dBFS -> 28 %
1 kHz al 60 %: THD de la envolvente 0,3 % (-49 dB)
1 kHz al 90 %: THD de la envolvente 0,3 % (-52 dB)
rampas: portadora 9-11 ms, profundidad 20 ms, bajada 17 ms
piso de portadora con tono a fondo de escala: 50 % a 60 % de profundidad, 25 % a 90 %
```

La profundidad alcanzada es el 80-95 % de la pedida porque el nivel se queda en
0,95 de fondo de escala y la envolvente se muestrea a 8 kHz; la pendiente de la
calibración es lo que ajusta el número final sobre el aire. Por debajo de unos
−35 dBFS de micrófono el AGC se queda sin ganancia (+36 dB) y la profundidad cae:
es un micrófono demasiado bajo, no un fallo del modulador.

## 5. Seguridad y legalidad

Tres piezas, y ninguna de las tres se guarda en flash:

1. **La compuerta** (`core/airband.h`, `airbandTxState()`). La primera pulsación
   del PTT en una frecuencia de la banda **no transmite**: pone el modo en
   `AIRBAND_TX_PROMPT`. Una segunda pulsación, con el aviso en pantalla, lo arma
   para el resto del encendido (`AIRBAND_TX_ARMED`); a partir de ahí el PTT
   transmite. Si nadie pulsa nada en 8 s, el aviso se cierra y el modo vuelve a
   `LOCKED`. Al apagar, `LOCKED` otra vez: una radio que cambia de manos no
   transmite en la banda por lo que hizo el anterior dueño.
2. **El aviso** (`ui_main.c`, `_ui_drawAirbandTxPrompt()`): un recuadro rojo que
   cubre la pantalla principal con «AIRBAND TX / 118-137 MHz: licensed band, you
   are the one in command / PTT again = transmit / wait = locked». Mientras el
   modo está seleccionado, la línea 1 de la pantalla lleva además `TX-LOCK` o
   `TX-ARM` al lado del squelch: el estado del transmisor no está a un menú de
   distancia.
3. **El menú**: la fila `TX` del menú rápido del modo arma o bloquea a mano, y la
   fila `DEPTH` elige la profundidad (30-90 %).

Además:

- el modo sólo transmite **dentro de la banda** (`airband_inBand()`); fuera de
  118-137 MHz la petición se rechaza aunque esté armado;
- la potencia de portadora está **limitada a 1 W** (`AM_CARRIER_MW`), no a la que
  pida el canal: con 90 % de profundidad los picos son 3,6 veces la portadora, y
  el PA de esta radio no está calibrado por encima de eso en esta banda;
- `platform_getPttStatus()` se lee con detección de flanco, de modo que la misma
  pulsación que abre el aviso no puede ser la que confirme;
- el vigilante de transmisión de `rtx.cpp` (`TxGuard`) sigue siendo el que corta
  una transmisión que se alarga o un PTT atascado.

## 6. Calibración

La calibración son **cuatro números por frecuencia**: el ciclo de trabajo de la
portadora (1 W), el fondo y el techo de la ventana lineal del PA, y la pendiente
envolvente → ciclo. En el código son `AM_CARRIER_MW`, `AM_DUTY_MIN_PCT`,
`AM_DUTY_MAX_PCT` y el `slopePct` de `air_tx_setCalibration()`, hoy en sus
valores de partida: 1 W, 10 %, 190 % y la pendiente igual al ciclo de la
portadora (envolvente proporcional al ciclo).

Lo que hay que medir, con carga fantasma y atenuador (nunca por antena):

1. **Potencia de portadora contra ciclo de trabajo**, en 118.000, 127.000 y
   136.975 MHz. Barrido estático de 5 % a 70 % en pasos de 5 %, con el modo en
   `DEPTH 30 %` y sin hablar (portadora desnuda): medidor de potencia o el
   analizador en zero-span. De aquí salen el ciclo de 1 W (el de partida es el
   `loPwr` de la calibración de fábrica, extrapolado desde 136.125 MHz) y el
   punto donde la curva se dobla (techo de la ventana). El fondo es el ciclo
   más bajo en el que la potencia sigue a la polarización sin desplomarse.
2. **Pendiente**: con un tono de 1 kHz al 50 % y el modo en `DEPTH 60 %`, medir
   la profundidad de la envolvente (sección 7) y ajustar `slopePct` hasta que
   sea 60 %. Es el número que corrige que la potencia del PA no sea lineal con el
   ciclo (lo normal es que vaya con el cuadrado o peor).
3. **Profundidad** a 30, 60 y 90 %: si a 90 % la envolvente se achata arriba o
   abajo antes de llegar al 90 %, la ventana es más estrecha de lo que dice el
   valor de partida y hay que cerrarla; el modulador bajará la profundidad solo.
4. **Ancho de banda de la modulación**: el número que no se puede suponer. Tono
   de 400 Hz y luego de 2,7 kHz al 50 % de profundidad, y comparar la
   profundidad medida en los dos casos. Si a 2,7 kHz cae más de 3 dB respecto a
   400 Hz, el filtro del APC (o el lazo de corriente del PA) está limitando el
   audio: con 2,7 kHz no hay banda lateral completa, y el modo debería bajar el
   techo del paso bajo (`core/am_tx.c`) a lo que el hardware dé, en vez de
   ecualizar.

## 7. Verificación: cómo se mide esto

La propia radio no sirve: es half-duplex, y su receptor AM está apagado mientras
transmite. Hacen falta uno de estos dos, por orden de preferencia:

**a) Analizador Anritsu MS2090A** (el del banco, con la skill `anritsu-ms2090a`
para el control por SCPI), en 118.000 MHz con carga fantasma y 40 dB de
atenuador:

- **zero-span** en la portadora con demodulador AM: da la forma de onda de la
  envolvente en el tiempo. De ahí salen la profundidad (`(Vmax-Vmin)/(Vmax+Vmin)`),
  las rampas y el piso de portadora, sin más procesado;
- **captura IQ** de 200 kHz alrededor de la portadora y demodulación AM en el
  PC: profundidad, THD de la envolvente por FFT y, sobre todo, el **espectro**:
  portadora, bandas laterales a ±1 kHz, y qué hay a ±8,33 kHz (el canal vecino)
  y a ±25 kHz. Un transmisor de AM bien ajustado deja el vecino 40 dB abajo; uno
  que recorta los picos, no;
- **potencia**: lectura de canal para la calibración estática (sección 6).

**b) Un SDR** (RTL-SDR, USRP B210, o cualquier receptor con AM y 2 MHz de ancho):
grabar IQ a 118 MHz y demodular en el PC con la misma cuenta. Es lo que usa
`tests/platform/c62_airband_agc_sim.c` al revés, y sirve igual para el espectro.

**c) Osciloscopio en el APC** (pin GPIO_A_03 o el nodo APCV, sonda 10:1):
comprueba la cadena eléctrica sin radiar — que el filtro deja pasar 2,7 kHz, que
la rampa son 16 ms y que el piso no llega a cero. Es la medida que separa un
problema del modulador de un problema del PA.

Si no hay ninguno de los tres, lo que queda es el análisis teórico de este
documento y las medidas de host: el modulador está verificado hasta el ciclo de
trabajo, y **la calibración del PA queda pendiente**, que es exactamente el
estado en que está el valor de partida.

## 8. Coste

Instrucciones por muestra de audio, medidas en QEMU (`mps2-an505`, Cortex-M33,
plugin `work/dsp-probe/common/libinsnframe.so`, un segundo de audio entre dos
marcadores con nada más que `am_tx_process()`):

| build | instrucciones/muestra | por segundo | a 48 kHz sobre 200 MHz |
|---|---|---|---|
| `-O2` | 135,6 | 6,51 M | 3,25 % de la CPU |
| `-Os` | 133,6 | 6,41 M | 3,21 % |

Los tres biquads a 48 kHz son casi todo. La interrupción de 8 kHz que dosifica
la envolvente añade del orden de 8000 × 40 instrucciones (≈0,16 %): lo que se
paga por tener la base de tiempos en hardware y no en el worker de audio.

Memoria, con `-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`:

| | flash | RAM |
|---|---|---|
| `master` `a1e4d304` | 566 992 B (54,07 %) | 179 504 B (68,48 %) |
| rama, `C62_AIR_TX=OFF` | 567 116 B (54,08 %) | 179 520 B (68,48 %) |
| rama, `C62_AIR_TX=ON` | 573 132 B (54,66 %) | 179 936 B (68,64 %) |
| **diferencia (ON − master)** | **+6 140 B** | **+432 B** |

Los 432 B son el modulador (tres biquads con su estado, el detector, la
ganancia y la calibración), el anillo de 64 valores de ciclo, el estado del
modo y el enganche del códec. Con la opción apagada quedan 124 B de flash y 16 B
de RAM: el campo `am_depth` del registro de ajustes, el estado de la compuerta y
las llamadas que el compilador deja fuera.

## 9. Límites, y lo que falta por medir en la radio

1. **El ancho de banda de modulación del APC** es el desconocido principal: el
   RC de entrada (1 kΩ, 10 nF, 15,9 kHz) no limita, pero el lazo de corriente
   del PA y las capacidades de desacoplo de la polarización sí pueden. Se mide en
   el banco (sección 6.4) y decide si el modo llega a 2,7 kHz.
2. **La linealidad del PA con la polarización** no está medida en ninguna
   frecuencia. De ella dependen la pendiente, la ventana y la profundidad máxima
   real; el valor de partida (10-190 % de la portadora) es el que necesita un
   90 % teórico, no el que el PA aguanta.
3. **La calibración de fábrica no llega a la banda**: por debajo de 136.125 MHz
   el ciclo de 1 W está extrapolado. La primera medida del banco es esa.
4. **Térmica**: a 1 W de portadora con 90 % de profundidad la disipación media
   del PA es mayor que en FM a 1 W, y el firmware no la vigila. Transmisiones
   largas en esta banda no son el caso de uso (una llamada de tráfico aéreo son
   segundos), pero el vigilante de TX no es un modelo térmico.
5. **No es un transmisor certificado**: la modulación, el ancho de banda y las
   emisiones no esenciales se han diseñado para parecerse a un AM de la banda,
   pero no hay medida de EMC ni de máscara espectral contra la norma. Para volar
   de verdad hace falta un equipo certificado, no esto.
6. **El ALC del chip** (REG_4B) queda activo con el perfil de voz: como el audio
   que entra al chip es cero, no actúa, pero conviene saber que si alguien
   cambiara el camino (por ejemplo, dejando pasar la voz al BK4819) el ALC
   pelearía con la envolvente.
7. **El modo digital AM (ACARS/VDL)** no está contemplado: esto es voz DSB-FC.

## Referencias

- Lista de registros del BK4819 V3: `datasheets/BK4819V3_Registers_List_20201218.pdf`
  (REG_30, REG_36, REG_40, REG_4B, REG_47, REG_48).
- Firmware de fábrica de la C62: `forks/c62-analysis/stock-firmware-report.md`
  §«TX» (APC en PA3, GPT ch3 a 100 kHz, tabla de calibración por banda) y
  `forks/c62-analysis/reconstructed/radio/bk4819.c` (`radio_tx_tune`, REG_36).
- Esquema RF: `C62_Teardown/Schematics/C62_RF.pdf` (R131/C309/U21/APCV,
  R132/C308/U41/APCU, R1 y el LM2904).
- Driver PWM del SoC: `csk/drivers/pwm/pwm_csk6.c` y
  `modules/hal/listenai/csk6_cm33/src/gpt_pwm.c` (cuantización al 1 %).
- ICAO Annex 10 Vol. III: la banda y la modulación DSB-FC de la voz aeronáutica.
- EUROCONTROL, *8.33 kHz Voice Channel Spacing* (el raster de 8,33 kHz).
