# Pruebas de la reducción de ruido en el DSP

**Para qué sirve.** RNNoise se va a mover del M33 al HiFi4 (`reports/dsp_ns.md`).
Lo que se puede decidir sin la radio se decide aquí: bit-exactitud, coste,
latencia, continuidad y qué pasa cuando el DSP no contesta.  Todo corre en el
host o en los simuladores de los dos núcleos; lo que necesita la imagen
integrada se marca como pendiente y se dice qué falta.

Los comandos viven en el `Makefile` de `dsp/ns`:

| comando | qué comprueba | dónde |
|---|---|---|
| `make m33-dump dsp-dump compare` | el DSP reproduce al M33 trama a trama | QEMU `mps2-an505` + ISS |
| `make crosscheck N=32` | x86/glibc, M33/newlib y HiFi4 dan lo mismo | host + QEMU + ISS |
| `make test` | el cuerpo de servicio int16 (incluye redondeo) | ISS |
| `make cost` / `make cost-check` | el presupuesto de ciclos e instrucciones | ISS |
| `make test-path` | continuidad, latencia y degradación sin DSP | host |
| `make test-path-mutations` | que las dos pruebas anteriores **pueden fallar** | host |

---

## 1. Qué NO cubre este documento

* La radio. Nada de esto toca el aire, el ADC ni el DAC.
* La integración en la imagen: no hay servicio LSF todavía, ni drivers de audio
  del DSP (`reports/dsp_ns.md` §6).  §5 mide el **lado del AP** de ese contrato
  con un modelo, no el camino real; lo que falta está listado allí.
* La calidad subjetiva (PESQ/STOI): es del banco, no de aquí.

---

## 2. Presupuesto de coste (umbral, no cifra)

`make cost` mide una trama de 10 ms con el ISS de dos builds del mismo arnés (0 y
8 tramas) y **falla** si se pasa:

```
COST BUDGET: instructions <= 700000, cycles <= 1000000
COST OK
```

| magnitud | umbral | medido | margen |
|---|---:|---:|---:|
| instrucciones/trama | ≤ 700 000 | 671 441 | 4,1 % |
| ciclos/trama | ≤ 1 000 000 | 933 066 | 6,7 % |

El margen es pequeño a propósito: 700 000 instrucciones son el 23,3 % de un
núcleo a 300 MHz y 1 000 000 de ciclos el 33,3 %, así que el umbral salta cuando
un cambio empuja la etapa por encima de «cabe con holgura» aunque no llegue a
romper el criterio del 25 % del informe.  Un umbral un 20 % por encima de la
medida no lo dispararía nunca.

Los umbrales están **en el `Makefile`** (`COST_MAX_INSN`, `COST_MAX_CYCLES`), no
en este documento ni en el informe; se pueden bajar o subir en la línea de
comandos para un experimento, y eso es lo que se usa para comprobar que el test
sirve:

```
$ make cost-check COST_MAX_INSN=600000
COST BUDGET: instructions <= 600000, cycles <= 1000000
COST OVER BUDGET: instructions/frame 671441 > 600000
make: *** [Makefile: cost-check] Error 1            (make sale con 2)
```

Con el umbral real: `COST OK` y estado 0.  El número y el umbral salen siempre
juntos, para que un fallo diga qué se pasó y por cuánto.

El umbral también muerde en una regresión de verdad, no sólo en la mutación
artificial: `make SIMD=1 cost` (el kernel de 2 carriles de §4.1 del informe, que
cuesta 1,53×) devuelve

```
COST OVER BUDGET: instructions/frame 1030327 > 700000; cycles/frame 1299890 > 1000000
```

---

## 3. Bit-exactitud

| prueba | criterio | resultado |
|---|---|---|
| M33 vs DSP, 8 tramas | 3 840/3 840 muestras | **3 840/3 840** |
| cuerpo de servicio int16 | 3 840/3 840 | **3 840/3 840** |
| `crosscheck N=8` (x86 ↔ M33 ↔ DSP) | los tres pares | **3 840/3 840** en los tres |
| `crosscheck N=32` | 15 360/15 360 por par | **15 360/15 360** en los tres |
| neutralidad para el firmware | M33 con `LIBFIX=0` == `LIBFIX=1` | **8/8 tramas** |

Es igualdad de bits, no «equivalente».  Lo que la sostiene (tablas y estado de
`kiss_fft` de `ns_tables.h`, `log10f` fdlibm, literales en `float`, sumas en
orden de programa) está en `reports/dsp_ns.md` §3.1 y §5.  Si algún día el M33
compila con contracción FMA, la igualdad se rompe y la tolerancia es la de §5.2
de ese informe: 0,777 LSB, SNR ≈ 72 dB.

---

## 4. Latencia y continuidad — `make test-path`

**Qué inyecta.** Un tono de 997 Hz (elegido así porque su periodo no es
submúltiplo de los 48 muestras de la rejilla: con 1 kHz todos los retardos
múltiplos de 48 correlacionan igual y el retardo no se puede medir), 60 s a
48 kHz = 6 000 tramas.

**Qué comprueba.**

1. **Continuidad:** cuántas muestras llegan al consumidor.  Criterio: igual al
   número de muestras inyectadas, cero tramas perdidas en 60 s.
2. **Latencia:** máximo de la correlación cruzada normalizada entre entrada y
   salida, retardos 0–40 ms.  Criterio: **≤ 30 ms añadidos**.

**Resultado medido:**

```
CONTINUITY OK 2880000 samples in, 2880000 samples out, 0 frames lost
LATENCY OK latency: added delay 4.98 ms (peak correlation 0.997), budget 30.00 ms
RESULT: PASS
```

Los 4,98 ms son el retardo que introduce la etapa (análisis/síntesis con ventana
de 960 y salto de 480) más el anillo del modelo; la correlación no es un retardo
puro porque el denoisador no es lineal, de ahí que se informe también el valor
del pico (0,997 = el tono pasa prácticamente intacto).

**Mutación** (`-DNS_DROP_FRAME=1`: la etapa se come la trama 100 y el consumidor
no la recibe).  El test tiene que fallar y decir cuánto falta:

```
FAIL continuity: 2880000 samples in, 2879520 samples out, 1 frame(s) lost,
480 samples missing
RESULT: FAIL (1)                       (estado ≠ 0)
```

`make test-path-mutations` ejecuta las dos mutaciones y comprueba que ambas
fallan; si alguna pasara, el target termina con error.

---

## 5. Degradación sin DSP — `make test-path`

**Qué mide.** El camino del AP con la etapa activada y el DSP ausente.  Como no
hay servicio LSF todavía, se modela el **lado del cliente**: anillo de 4 tramas
de 480 muestras por sentido (la profundidad que diseña `docs/NOISE.md`), una
llamada por trama con plazo de 2 ms y la rama «no contesta».

**Criterio y resultado:**

```
DEGRADE OK degradation: state=DSP_ABSENT warnings=6000 log=1
fallback=935 us in 6000 frames (0 us/frame), supervisor stall at 2000 ms
RESULT: PASS
```

* el audio que sale es **byte a byte la entrada** (sin reducción de ruido, sin
  silencio): `memcmp` de los 2 880 000 bytes;
* **cero muestras perdidas** con el DSP ausente;
* el estado pasa a `DSP_ABSENT` y se emite **una** línea de log por transición
  (contador de tramas aparte: 6 000), que es lo que leen la UI y el `blackbox`;
* el coste de degradar es de microsegundos por trama, tres órdenes de magnitud
  por debajo de los **2 000 ms** que el `blackbox` tarda en declarar un atasco
  (`BB_STALL_MS`, `openrtx/include/core/blackbox.h`).

**Mutación** (`-DNS_NO_FALLBACK=1`: un cliente que interpreta «no contesta» como
«silencio»).  Tiene que fallar:

```
FAIL degradation degradation: state=DSP_ABSENT warnings=6000 log=0 ...
  audio is not the input: the AP would hear the stage's silence
  the degradation is not visible in the state or the log
RESULT: FAIL (2)
```

**Lo que este modelo NO puede demostrar, y qué hace falta para medirlo:**

1. **El cliente LSF real** (`CMD_NS_OPEN`, `ICStream_Consumer_commitRemote`,
   plazo del RPC): hoy no existe el servicio; hay que escribirlo en el AP sobre
   `services/service_audio.c` y en el DSP (`dsp-openfw`, junto a `lsf_audio`).
2. **Que el DSP «no contesta» de verdad** (imagen ausente, mailbox mudo, RPC
   perdido) — hace falta la imagen DSP y el `urpc` reales.
3. **El efecto en el hilo de voz del AP**: el modelo no comparte CPU con el
   codec ni con el modulador, así que no puede demostrar que la degradación no
   provoca un atasco *por contención*; eso se mide en la radio con el
   `blackbox` (contador de ciclos por hilo).
4. **El aviso en la UI**: el modelo escribe el estado; que la pantalla lo pinte
   es una prueba del simulador (`tests/host`), no de aquí.

---

## 6. Estado de las pruebas

| # | prueba | criterio | estado | dónde |
|---|---|---|---|---|
| 1 | bit-exactitud M33 ↔ DSP, 8 tramas | 3 840/3 840 | ✅ hecha | `make compare` |
| 2 | cuerpo de servicio int16 | 3 840/3 840 | ✅ hecha | `make test` |
| 3 | tres targets (x86, M33, DSP) | 15 360/15 360 por par, 32 tramas | ✅ hecha | `make crosscheck` |
| 4 | neutralidad `LIBFIX` para el firmware | 8/8 tramas | ✅ hecha | `make LIBFIX=0 m33-dump` |
| 5 | presupuesto de coste | ≤ 700 000 instr / ≤ 1 000 000 ciclos | ✅ hecha, con mutación a 600 000 | `make cost` |
| 6 | continuidad de 60 s | 0 muestras perdidas | ✅ hecha, con mutación (trama perdida) | `make test-path` |
| 7 | latencia añadida | ≤ 30 ms | ✅ hecha: 4,98 ms | `make test-path` |
| 8 | degradación con el DSP ausente (lado AP, modelo) | audio idéntico, 0 pérdidas, estado + log, ≪ 2 000 ms | ✅ hecha, con mutación (sin fallback) | `make test-path` |
| 9 | cliente LSF real y servicio en el DSP | `CMD_NS_OPEN` → `-ENOSYS` si no está, audio sin etapa | ⏳ pendiente: no hay servicio ni imagen | — |
| 10 | degradación en la radio (contención de CPU, `blackbox`) | sin atasco, aviso en la UI | ⏳ pendiente: requiere imagen y radio | — |
| 11 | calidad (PESQ/STOI) contra la tabla de `docs/NOISE.md` | igual que la etapa del M33 | ⏳ pendiente: banco | — |
| 12 | criterio de carga en la radio (≤ 10 % del DSP) | contador de ciclos del CSK6011B | ⏳ pendiente: hace falta la radio; hoy 31,1 % en el modelo del ISS | — |
