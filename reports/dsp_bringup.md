# Fase 0: la imagen DSP abierta arranca (y fase 1 empezada)

Fecha: 2026-11-28 · Rama `dsp/integration-research` · Continúa `reports/dsp_integration.md` §3.

Este documento cierra la **fase 0** (que la imagen arranque) y arranca la **fase 1**
(que el servidor conteste al AP). Todo lo que se afirma aquí se ha ejecutado en
este árbol; los comandos y su salida van citados. No se ha tocado la radio, ni
`/dev/ttyUSB*`, ni el código de producción del AP.

---

## 0. La respuesta corta

**La imagen arranca, y el servidor contesta al arranque del AP.** En QEMU
(`-M sim -cpu venus_hifi4`) el firmware:

* entra por `0x60000000` (ahí está `j _xt_crt0`, ya no un literal),
* instala `VECBASE=0x60000400` con tabla de 16 vectores real,
* pone a cero `.bss` (se comprueba llenándolo de `0xA5` antes de la primera
  instrucción, como hace el `memcpy` de 1 MB del AP),
* monta la pila y **no cae en ninguna excepción en 30 s** mientras da
  389 465 677 vueltas al bucle y atiende 2 985 interrupciones de nivel 1
  (tick de 10 ms por `CCOMPARE0`),
* y responde la secuencia completa de `lsf_connect()` del AP (EP0 ping,
  EP1 h3 ×2, EP4 h4, EP1 h2, EP3 h2 con el PUSH de «DSP listo», EP1 h1 ×3)
  **por los registros del mailbox emulado**, no por llamadas en proceso.

Por el camino han salido tres cosas que no eran de la fase 0 pero la bloqueaban
o la habrían bloqueado en la radio:

1. **El servidor no recalculaba el checksum de las tramas de salida.** El AP
   habría tirado *todas* las respuestas con `URPC_ERR_INVALID_CRC`. El test de
   contrato del host no lo veía porque no comprueba el checksum de la respuesta.
2. **`EP1 h2` (fence) devolvía 1 byte del `fenceID` en vez de 4.**
3. **El camino de desbordamiento de ventanas de registro no es fiable en este
   emulador** (y el `hello world` del propio toolchain cae igual). El firmware
   se compila `-mabi=call0`, que elimina la clase entera de fallos. §6.

---

## 1. Qué se ha cambiado, fichero a fichero

Todo en `/home/n0p/src/c62/work/dsp-openfw` (rama `bringup`, commit al final).

| Fichero | Antes | Ahora |
|---|---|---|
| `src/crt0.S` | no existía; `_start` era una función C con `entry a1,32` sin pila | entrada: `j` en el byte 0, pool de literales, `a1=_stack_top`, `.bss` a cero, `wsr.vecbase`, `PS.EXCM=0/INTLEVEL=0`, `call0 dsp_main` |
| `src/vectors.S` | no existía | tabla en `0x60000400` (9 vectores propios + los 6 de ventana del vendor) y entrada común que salva `a0..a15`, `SAR`, `EXCCAUSE`, `EPC1`, `EXCVADDR` |
| `src/exc.c` | no existía | despacho C del vector: nivel 1 (tick `CCOMPARE0`, IRQ de software), registro de trampas y *park*. **Compilado `-mabi=call0` a propósito** (con `PS.EXCM=1` una rotación de ventana sería doble excepción) |
| `src/dbg.c`, `src/dbg.h` | no existían | `memcpy`/`memset`/`memmove`/`printf` propios (la imagen va `-nostdlib`), salida por SIMCALL (`a2=4`), `tick_init()` y el bloque `g_dbg` que lee el arnés |
| `src/main_dsp.c` | `void _start(void)`, mailbox fijo, sin diagnósticos | `dsp_main()`; bases de mailbox y banco compartido parametrizables; contadores en `g_dbg`; comandos de depuración; `srv_seal()` al enviar |
| `src/srv_lsf.c` | validaba el checksum de entrada y **no** el de salida | `srv_seal()` + `EP1 h2` ecoa los 4 bytes del `fenceID` |
| `link/venus_hifi4.ld` | `.text` con literales primero; pila detrás de `.bss` | `0x60000000` reservado al `crt0`, `0x60000400` a la tabla, pila fija de 32 KB en `0x600F0000` |
| `Makefile` | `XT_TOOLS` apuntaba a `./toolchain/…` (no existe) y `make dsp` no enlazaba | `XT_TOOLS` desde `$XTENSA_TOOLS_ROOT` o `~/xtensa/RI-2021.7-linux`; reglas `dsp`, `dsp_qemu`, `qemu_run`, `qemu_boot`; los builds de host se aíslan del `as` de Xtensa |
| `tools/qemu_boot.py` | no existía | arnés: cliente RSP (gdbstub) que hace de AP sobre el mailbox emulado |
| `tests/test_contract.c` | no comprobaba el checksum de salida | lo comprueba con una XOR independiente (`bad_cksum`) |

Nada de esto toca OpenRTX. El AP no se ha modificado.

---

## 2. Cómo queda la imagen

```
$ source ~/xtensa/env.sh && cd ~/src/c62/work/dsp-openfw && make dsp
...
build/dsp_firmware.bin: 9299 bytes (flat, load 0x60000000, partition limit 1 MiB)
--- entry point / first instruction:
60000000 <_start>:
60000000:	000b06                     	j	60000030 <_xt_crt0>
```

```
$ xt-nm build/dsp.elf | grep -E ' (_start|_xt_crt0|_xt_vecbase|_WindowOverflow4|_bss_start|_bss_end|_stack_top|dsp_main)$'
60003650 B _bss_end
60002460 B _bss_start
60000bd0 T dsp_main
600f8000 B _stack_top
60000000 T _start
60000400 T _WindowOverflow4
60000030 T _xt_crt0
60000400 T _xt_vecbase
$ xt-size build/dsp.elf
   text	   data	    bss	    dec	    hex	filename
   9292	      0	  37360	  46652	   b63c	build/dsp.elf
```

9 299 B de imagen (límite de partición: 1 MB), 9 292 B de `.text` — frente a
los 27 018 B del esqueleto anterior, porque ya no se enlaza libc.

Tabla de vectores (la del core, `core-isa.h`: `XCHAL_VECBASE_RESET_VADDR=0x60000400`,
`INTLEVEL2_VECOFS=0x17C`, kernel `0x23C`, user `0x25C`, double `0x27C`):

```
6000057c:  j 60000730   ; nivel 2      -> park (no hay drivers todavía)
6000059c:  j 60000730   ; nivel 3
600005bc:  j 60000730   ; nivel 4
600005dc:  j 60000730   ; nivel 5
600005fc:  j 60000730   ; nivel 6 / debug
6000061c:  j 60000730   ; NMI
6000063c:  j 6000073c   ; kernel exception  -> _xt_exc_entry
6000065c:  j 6000073c   ; user exception    -> _xt_exc_entry
6000067c:  j 600007a4   ; double exception  -> _xt_double_entry
```

En `0x60000400..0x6000056A` están los manejadores de ventana del vendor
(`libhandlers-board.a`, MIT, sin símbolos externos). Con `-mabi=call0` no se
usan; se quedan porque ocupan el sitio que el core exige y porque el día que se
vuelva a la ABI ventaneada ya están.

---

## 3. Evidencia: arranque y 30 s sin trampas

```
$ python3 tools/qemu_boot.py --elf build/dsp_qemu.elf --mode phase0 --seconds 30
   g_dbg=60002690 bss=60002690..60003880 stack_top=600f8000 vecbase=60000400
   pre-filled .bss with 0xA5 (4592 B), sample=a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5
   firmware reached its main loop: True (magic=30505344 loops=91418)
  [ok] firmware reaches its main loop
-- phase 0: boot
  [ok] VECBASE = 0x60000400 -- got 0x60000400
  [ok] PS.EXCM=0, INTLEVEL=0 -- PS=0x00040000
  [ok] no trap during 30.0 s -- traps=0
  [ok] main loop still advancing -- 91418 -> 389465677
  [ok] 10 ms tick interrupts dispatched -- irq=2985 tick=2985
   loop rate ~ 12616618 iter/s, 10.3 ms emulated per tick
  [ok] 12-deep call chain returns 234 with no trap -- deep_res=234 traps=0
  [ok] .bss zeroed by crt0 (0xA5 canary cleared) -- 0000...0000
   stack window 600f0000..600f8000 (32 KB, NOLOAD)
```

Lo del `0xA5` no es adorno: el arnés **escribe `0xA5` en `.bss` y en el top de
la pila antes de la primera instrucción**, que es lo que hace el AP en hardware
(`memcpy(0x60000000, partición, 1 MB)` de basura de flash). Si el `crt0` no
limpiara `.bss`, la canaria `g_bss_canary[256]` saldría `a5a5…`.

Salida por el canal de depuración (SIMCALL → consola serie de QEMU):

```
[cp] open LSF firmware up: vecbase=60000400 ps=00040000 bss=60002690..60003880 stack_top=600f8000
[cp] srv_init done: shm=60080000 size=65536
[cp] tick armed: period=3000000 cycles
[cp] loops=1 polls=0 irq=0 tick=0 in=0 out=0 traps=0
[cp] loops=200001 polls=0 irq=6 tick=6 in=0 out=0 traps=0
...
[cp] loops=388400001 polls=0 irq=2977 tick=2977 in=0 out=0 traps=0
```

---

## 4. Evidencia: la tabla de vectores está viva

Dos pruebas que no dependen de que llegue una interrupción de verdad:

```
$ python3 tools/qemu_boot.py --elf build/dsp_qemu.elf --mode traps
-- phase 0: the vector table is live
  [ok] software interrupt 7 dispatched and returned from -- irq=228 seen=00000040
  [ok] illegal instruction caught by the kernel vector -- cause=0 epc1=60000f7c
   parked: EXCCAUSE=0 EPC1=60000f7c
```

* `DSP_CMD_IRQ` escribe `INTSET` bit 7 (interrupción de software, nivel 1). El
  vector de kernel la recibe con `EXCCAUSE=4`, el manejador la atiende, la
  reconoce por `INTCLEAR` y vuelve con `rfe`: el contador de vueltas del bucle
  sigue subiendo después.
* `DSP_CMD_ILL` ejecuta `ill`: `EXCCAUSE=0` (IllegalInstruction) con
  `EPC1=0x60000f7c` (dentro de `.text`), `traps=1` y el core aparcado en
  `_xt_panic_loop` sin tocar memoria (para que no pueda encadenar trampas).

El tick de 10 ms es una interrupción de nivel 1 de verdad (`CCOMPARE0`,
`XCHAL_TIMER0_INTERRUPT=6`), con `INTENABLE` y `PS.INTLEVEL=0`: 2 985 en 30 s,
a 10,3 ms de media (el reloj virtual de QEMU no va exactamente a 300 MHz de
pared; el periodo programado son 3 000 000 ciclos).

---

## 5. Fase 1 empezada: el AP arranca contra el firmware emulado

`make test` reproduce la secuencia del AP **en proceso**, con colas en memoria.
Lo que faltaba era demostrar que el transporte real (registros del mailbox,
ack, *doorbell*) funciona con la imagen de verdad. `tools/qemu_boot.py` hace de
AP: para el núcleo, escribe la trama en `MBX_R0..R3`, levanta el bit del canal
en `MBX_AP_IRQ`, lo deja correr y lee `MBX_W0..W3` cuando el CP pone el bit
`eps+16` en `MBX_AP_CTRL`.

```
$ python3 tools/qemu_boot.py --elf build/dsp_qemu.elf --mode all
-- phase 1: lsf_connect() over the emulated mailbox
  [ok] EP0 ping answered (dst/src swapped, RESPONSE|SYNC) -- eps=0 flags=4 dst=255 src=1 sess=1
  [ok] allocator 0 published (EP1 h3) -- addr=60080000 off=0 mode=1 size=128
  [ok] allocator 1 published (EP1 h3) -- addr=60080d00 off=0 mode=0 size=128
  [ok] allocator 0 lies inside the shared bank -- 60080000
  [ok] EP4 h4 push registration acknowledged -- eps=4 flags=8 dst=1 src=4 sess=4 p0=d4c3b2a1
  [ok] EP1 h2 fence id echoed -- eps=1 flags=4 dst=1 src=2 sess=5 p0=44332211
  [ok] EP3 h2 answered with the 'DSP ready' PUSH -- eps=3 flags=8 dst=1 src=2 sess=6 p0=44332211
  [ok] stream 0 published (EP1 h1) -- share=60081a00 size=256 mode=0
  [ok] stream 1 published (EP1 h1) -- share=60083ac0 size=256 mode=0
  [ok] stream 3 published (EP1 h1) -- share=60085b80 size=256 mode=1
  [ok] server dispatched the frames (poll_count rose) -- polls=1673112
  [ok] no trap after the whole sequence -- traps=0
RESULT: PASSED
```

Eso es `lsf_connect()` entero, que es lo que el AP necesita para no quedarse
reintentando: ping de EP0 con `session` ecoada, los dos allocadores, el registro
de push, el fence con su PUSH de «DSP listo» (el AP se bloquea ahí:
`docs/INTEGRACION_AP.md` §0.4) y los tres `ICStream`.

### 5.1 Dos defectos del cable que ha destapado esto

**(a) El checksum de salida no se recalculaba.** Cada handler edita la trama que
contesta (`dst`/`src` intercambiados, `REQUEST`→`RESPONSE`, bit `PUSH`) y el
checksum cubre exactamente esos bytes. El código validaba la entrada y enviaba
la salida con el checksum de la petición. Medido en la primera pasada del
arnés: la respuesta de EP0 llegaba con `chksum=0xed` cuando la trama valía
`0xef` (justo el bit `REQUEST`), es decir, `urpc_recv_check()` del AP la habría
descartado con `URPC_ERR_INVALID_CRC (-12)`. `make test` no lo veía porque
comparaba `session`, no el checksum.

Arreglo: `srv_seal()` en `src/srv_lsf.c`, aplicado en los dos transportes
(`mbx_send()` en la imagen, `port_send()` en el test) — el sellado es cosa del
cable, no de cada handler. El test ahora recalcula la XOR por su cuenta y
cuenta `bad_cksum` (hoy 0).

**(b) `EP1 h2` ecoaba un byte del `fenceID`.** `int id = f->rpc.payload[0]`
devolvía `0x44` para un fence `0x11223344`. El AP ignora el valor, así que no
rompe nada hoy, pero el contrato dice `[0..3]` y cualquier día alguien lo
compara. Arreglo: `wr32(&out.rpc.payload[0], rd32(&f->rpc.payload[0]))`.

Ninguno de los dos se habría encontrado sin sacar las tramas por el mailbox
real; los dos son de la clase «funciona en el test, no en la radio».

---

## 6. El problema de las ventanas de registro (y por qué `-mabi=call0`)

Primero apareció como un cuelgue al procesar la primera trama. La traza, leída
del propio core (paquete `g` del gdbstub: `pc`, `depc`, `exccause`, `excvaddr`):

```
PC=600007af  (park)   EPC1=60001143  DEPC=6000048f  EXCCAUSE=0xf  EXCVADDR=ffffffe4
```

* `EPC1=0x60001143` es la segunda instrucción de `srv_seal()`, o sea: ahí se
  disparó el **desbordamiento de ventana** (`call8` gasta dos de las ocho
  ventanas que dan los 32 `AREG`; el cuarto `call8` anidado desborda).
* `DEPC=0x6000048f` es `s32e a4, a0, -32` dentro de `_WindowOverflow8`
  (el manejador del vendor, en `0x60000480`).
* `EXCVADDR=0xffffffe4` ⇒ `a0 = 4`, y `a0` venía de `l32e a0, a1, -12`: la
  ranura de *spill* contenía un 4 en vez de un puntero de pila.

Reproducido sin mailbox ninguno con una recursión de 12 niveles
(`DSP_CMD_DEEP`): mismo `DEPC`, mismo `EXCVADDR`. No es el protocolo, es el
camino de ventanas.

Y no es culpa de esta imagen: **el `hello world` del propio toolchain**, con su
`crt1-boards.o` y sus vectores de `libhandlers-board.a` (mismo `0x60000400`,
mismo `_WindowOverflow8` en `0x60000480`), recorre 12 niveles y cae también:

```
$ xt-clang --xtensa-core=venus_hifi4 -O2 -o t.elf t.c   # crt1 de fábrica
$ $QEMU_HIFI4 -M sim -cpu venus_hifi4 -m 128M -display none -semihosting -kernel t.elf
  (sin salida; 3 s después)  PC=6000067c  (vector de doble excepción)
                             DEPC=60000604  EXCCAUSE=0
```

**Decisión:** el firmware se compila `-mabi=call0`. Sin ventanas no hay
excepciones de ventana, ni `s32e`/`l32e`, ni pila de *spill* que cuadrar: la
clase entera de fallos desaparece. Coste: unos registros más de tráfico por
llamada; en el bucle del servidor es ruido, y la etapa de ruido (fase b) es un
bucle interno sin llamadas. Se puede volver a la ABI ventaneada cuando se
valide en silicio (o con XOS), y entonces los manejadores del vendor ya están
en su sitio.

Queda anotado como **riesgo abierto**: no sabemos si el fallo es de QEMU o de
los manejadores para este core. Lo que sí sabemos es que hoy no hay forma de
validar código ventaneado en este emulador, y que la imagen de fábrica usa XOS
(cuyos vectores de ventana son el mismo esquema) — o sea, que en hardware
*debería* funcionar; simplemente no podemos probarlo aquí.

---

## 7. Decisión: XOS 2.10 o *bare-metal* → **bare-metal + call0**

Lo pedido era elegir y justificarlo. **Bare-metal**, con estas razones:

1. **El modelo de concurrencia no necesita planificador.** El firmware es un
   bucle que sondea el buzón y una ISR (hoy el tick; en fase (c), la del DMA del
   APC). `register_ISR()`/`enable_IRQ()` de `venus_cp.h` son vacías sin
   `XOS_USED`, pero son tres ganchos (`venus_cp.h:396-426`), no un RTOS: con la
   tabla de vectores propia ya hay dónde colgarlos.
2. **XOS trae su propio `crt` y su mapa de memoria de EVB.** Habría que
   sustituir su *linker script* por el nuestro (imagen plana en `0x60000000`,
   banco compartido en `0x200C0000`, pila en `0x600F0000`). Ya está hecho el
   nuestro y verificado en emulación; con XOS habría que rehacer el mismo
   trabajo *más* el encaje del BSP.
3. **La decisión se puede revertir con coste acotado.** Si la fase (c) necesita
   dos prioridades (audio y RPC, ver `dsp_integration.md` §4.5), XOS es el
   camino natural, y para entonces el contrato, el arranque y el transporte ya
   estarán probados: sólo cambia el `crt` y quién llama a `srv_poll()`.
4. **El riesgo real no era el RTOS, era el arranque.** Está cerrado, y cerrado
   sin XOS. Añadir XOS ahora añade una dependencia (licencia Cadence para
   fabricar, `dsp_integration.md` §4.7) sin resolver nada de lo que queda.

Consecuencia que hay que respetar en fase (c): **el código que corre en una ISR
no puede rotar la ventana** (con `PS.EXCM=1` una rotación es doble excepción).
Con `call0` eso sale gratis. Si algún día se enlaza HAL compilado ventanas
(`-mabi=windowed`), esa regla hay que revisarla — y probarla en silicio, no en
QEMU.

---

## 8. Qué queda para la fase 1 (y después)

Hecho aquí: `lsf_connect()` completo sobre el mailbox emulado. Falta de la fase 1
propiamente dicha:

1. **`AudioRecord_ctor`/`AudioTrack_ctor` por el cable**: los `EP4 AF_OPEN_RECORD`
   / `AF_CREATE_TRACK` / `getCblk` con bloque de argumentos en el banco
   compartido. Eso obliga a portar `mpoolq_alloc` al arnés (el AP asigna del
   pool 0 y el CP libera) — es la pieza que falta para reproducir `make test`
   enteros contra la imagen.
2. **Ida y vuelta de audio con reloj**: el tick de 10 ms ya existe;
   `audio_task()` tiene que colgar de él (y no de la vuelta del bucle) para que
   los anillos no se llenen de rampas a 12 M tramas/s.
3. **QEMU necesita ~100 ms de reloj de pared por transacción** (el `vCPU`
   reanudado por el gdbstub tarda en arrancar): el arnés duerme 150 ms por
   trama. Con 12 transacciones son ~2 s; no es un problema, pero explica
   tiempos raros si alguien toca el arnés.
4. **El tick se pierde cuando el núcleo está parado** (reloj virtual de QEMU):
   entre transacciones `CCOUNT` salta y el primer tick llega tarde. No afecta a
   las pruebas, sí a cualquier medida de tiempo fino.

Para la fase (c) (audio real) queda lo ya sabido: APC/DMA/ADC/DAC, ganancias del
`k=v`, reparto L/R y **probar en la radio**. Y el coste real en silicio
(`dsp_integration.md` §4.4).

---

## 9. Reproducción

```bash
# build (no hace falta exportar nada; el Makefile resuelve XT_TOOLS)
cd ~/src/c62/work/dsp-openfw
make test              # contrato: PASSED (0 failures), bad_cksum=0
make dsp_sim           # bucle del DSP en el host
make dsp               # ELF + imagen plana; imprime la primera instrucción

# emulador (las dos necesitan el QEMU con el core importado)
source ~/xtensa/env.sh        # o QEMU_HIFI4=... a mano
make qemu_run                 # 35 s de consola
make qemu_boot                # fase 0 + trampas + lsf_connect(), con veredicto
python3 tools/qemu_boot.py --elf build/dsp_qemu.elf --mode phase0 --seconds 30
python3 tools/qemu_boot.py --elf build/dsp_qemu.elf --mode traps
python3 tools/qemu_boot.py --elf build/dsp_qemu.elf --mode connect
```

`build/dsp_qemu.elf` **no se flashea**: mueve el mailbox a `0x60090000` y el
banco compartido a `0x60080000` porque `-M sim` no modela ninguno de los dos
(ver `main_dsp.c`, `SRV_MBX_BASE`/`SRV_SHM_BASE`). La imagen de hardware es
`make dsp`.

---

## 10. Lo que este informe **no** afirma

* Que el camino de ventanas esté roto en silicio: sólo que **en este QEMU** no
  funciona, ni con nuestra imagen ni con la del toolchain.
* Que `call0` sea gratis: no se ha medido el coste en el bucle del servidor ni
  en la etapa de ruido. Se mide cuando haya audio (fase b).
* Que `lsf_connect()` contra el AP real funcione ya: se ha reproducido contra un
  AP simulado que habla el protocolo, incluidos los dos defectos de cable que
  sólo se ven en el transporte.
* Que el reparto ADC01/ADC23, el APC o el DMA estén resueltos: es fase (c) y
  necesita la radio.

---

# Fase 1: el audio (actualización, 2026-11-28 tarde)

El código está en `work/dsp-openfw`, rama `bringup`: `76bef38` (etapa, tick y HAL),
`a0cda41` (push y streamID), `571e297` (captura) y `5e1ef17` (el chequeo de anillo).

## 11. Lo que funciona, medido

| Qué | Evidencia |
|---|---|
| Captura completa | `read 100` → **100/100 tramas de 1920 B, 0 muestras distintas**, `drops=0`, `traps=0` |
| Tick | `[cp] tick armed: period=3000000 cycles`; `tick == blocks+drops+held+idle+pending` en todas las muestras |
| Un bloque por tick | `blocks=10 drops=0` para 10 lecturas, con `held` e `idle` contados aparte |
| Cliente del AP | `modules/lib/{urpc,lsf,af}` **sin tocar**, `-m32` (el contrato es ILP32: en 64 bits los punteros de los objetos compartidos se leen como dos campos pegados) |
| HAL del CP | siete ficheros **sin una modificación**, tres shims; imagen **142 560 B** |
| `sqrtf` propio | bit-idéntico a glibc en las **2 139 095 040** floats positivas |

## 12. El bloqueo del push, resuelto con datos

El cliente se paraba registrando el push de propiedades (EP4 h4). Instrumentando una copia
del cliente (`tools/ap_client/probe/ic_property.c`):

```
[probe] push: eps=4 flags=8 dst=4 src=4 sess=4 pl=00000001 00180000 00
[probe] find(0,1): count=0 items=(nil)   -> MISS
```

El handler del AP parsea ese payload como *notify* de `group=0, property=1`, y **aún no ha
importado ningún grupo** (`lsf_import_properties` llega después, desde
`AudioSystem_initialize`). El `IC_ASSERT` se queda en `while(1)` **dentro del task async de
urpc**, que es el que entrega todas las respuestas: por eso el primer `lsf_set()` se colgaba.

La corrección es la del vendor (`lsf_server/src/ic_property.c:875`): **no responder** a
`PUSH|REQUEST`, sólo guardar la plantilla e intercambiar ids. Implementado así, más
`srv_prop_notify_uint()` como camino real de notificación.

## 13. La etapa de ruido no se puede ejecutar en QEMU (opcodes AE)

```
after bench: traps=1 cause=0 epc1=60002daf vaddr=00000000
trap at 60002daf (ns_dsp_process+47)
```

`xt-clang` compila `kiss_fft` a instrucciones del **Audio Engine** (`ae_*`): ~470 en `-O2`,
y también en `-O1`. El modelo `venus_hifi4` **no implementa ninguno**
(`target/xtensa/translate.c` no tiene casos AE). No es configurable: el modelo no existe.

De ahí, dos reglas para el resto del proyecto:

- La bit-exactitud de la etapa **se mide en el ISS**, que es donde se midió (8/8 tramas,
  3 840/3 840 muestras).
- `make noae` **falla el build** si las fuentes propias emiten opcodes AE; `make noae-ns`
  avisa de los de la etapa sin romper el build.

## 14. La deriva no se mide en QEMU

El CP corre en ventanas de 120 ms de vCPU gobernadas por el cliente, así que su tiempo no
corre con el reloj de pared: en el escenario libre, el productor adelanta al consumidor
(`drop` sube) porque el cliente no está siendo planificado, no porque el ritmo esté mal.
Lo medido y firme: **periodo del tick exactamente 3 000 000 de ciclos** y **un bloque de 480
muestras por tick**. La deriva contra reloj es una medida de silicio.

## 15. Pendiente para la fase (c)

- **Pin muxing del códec**, vacío a propósito en `src/audio_hw.c`: los números de pad están
  en el board file del SDK y no en el árbol; inventarlos da códec mudo **sin error**.
- **Reparto ADC01/ADC23** (micrófono vs. discriminador) y mapeo DAC0/DAC1: pregunta A/B, en
  la radio.
- **`register_ISR`/`enable_IRQ`** del HAL, vacías sin XOS: en silicio hay que colgarlas de
  la tabla de vectores propia.
- **Coste real con caché**: el 31,1 % es del ISS; 16 KB de D-cache frente a un working set
  de 150 KB puede moverlo.
