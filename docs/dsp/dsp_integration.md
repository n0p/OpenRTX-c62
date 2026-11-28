# Integración de la reducción de ruido en el firmware del DSP — índice

El informe completo, con la evidencia (`fichero:línea`, símbolo y medida), está en
[`../../reports/dsp_integration.md`](../../reports/dsp_integration.md). Este fichero es el
resumen que se consulta de pie.

## Respuesta corta

**Sí se puede llegar a «la radio suena con la reducción de ruido en el DSP».** La etapa ya está
medida (bit-exacta contra el M33, 933 066 ciclos/trama = 31,1 % del HiFi4) y los drivers de audio
del CP existen como fuente en este árbol y compilan para `venus_hifi4` (0 errores, ~46 KB de
texto). Lo que falta es *bring-up*, no algoritmo.

Lo que más probabilidades tiene de pararlo:

1. **Nuestra imagen DSP no arranca.** `_start` en `0x600002f4` (el AP salta a `0x60000000`), sin
   pila, sin `.bss` a cero y sin `vecbase`; en QEMU muere con `EXCCAUSE=15` antes de 3 s.
   Es un `crt0` que falta: 8–16 h.
2. **El APC/DMA no se puede emular.** Sólo se valida en la radio, y el PDF del chip no está en el
   árbol. El modo de fallo es «códec en silencio sin error».
3. **El HAL de `extract/` no es la revisión de la imagen que corre hoy**: misma API, otro
   binario; hay que validar en hardware.
4. **El coste en silicio** puede pasar del 31 % al 38–45 % por *misses* de la D-cache (16 KB
   frente a un *working set* de ~150 KB). Se mitiga poniendo estado y pesos en DRAM1.

## Las cinco secciones del informe

| § | Qué responde |
|---|---|
| 1 | Periféricos y registros (ADC01/AON_CODEC, ADC23+DAC01/CP_CODEC, APC, DMAC, AUDPLL), IRQs 3 y 4, modelo FIFO+APC+DMA con callback y LLP, qué usa la fábrica (misma familia de drivers, `str-factory.txt`), qué usa la imagen A (**también** los lleva), y qué asume `dsp_sim` (nada: no hay reloj ni interrupciones). |
| 2 | La etapa va **entre el buffer del DMA del ADC y `srv_record_push()`**, canal de radio, in-place sobre la trama de 480 muestras: mismo punto de señal que hoy en el AP (`csk6_codec.c:836-859`), latencia cero añadida, sin cambios en el cable (a lo sumo un `NS=` en el `k=v` que ya existe). |
| 3 | Fases 0/a/b/c/d con criterio medible y qué se valida sin radio (a y b: todo; c: sólo el arranque; d: parcial). |
| 4 | Riesgos: arranque/XOS, APC opaco, revisión del HAL, caché, bucle sondeado (10 ms de latencia de RPC), doble procesado, licencias. |
| 5 | 100–168 h en total, camino crítico 0 → c1 → c2. Nada que comprar salvo la licencia FlexNet si sale del laboratorio. |

## Estado de partida comprobado hoy

```
make test      → PASSED (0 failures)      # contrato LSF
make dsp_sim   → dsp_sim ok               # bucle del DSP en el host
make dsp       → falla (XT_TOOLS mal)     # y aunque arregle, la imagen no arranca
QEMU -M sim    → PC=0x6000067c, EXCCAUSE=0x0f, EXCVADDR=0x10
```
