# Copia de seguridad interna

Las tres pantallas `MENU_BACKUP_RESTORE`, `MENU_BACKUP` y `MENU_RESTORE` venían
del OpenRTX genérico, donde detrás había un volcado xmodem de la flash externa
del MD-3x0 a un ordenador. En el C62 no abría ninguna tecla, así que eran código
muerto: la auditoría de cobertura del simulador las marcó y estuvieron a punto de
borrarse. En su lugar hacen lo que prometen, con el almacenamiento que la radio
ya tiene.

## Qué guarda

Un registro con los seis almacenes del equipo, tal y como los escribe cada uno
en flash (no se reinterpreta ningún campo):

| entrada | origen |
|---|---|
| ajustes | `settings_recordPack()` |
| VFO | el registro de VFO de `nvmem_C62.c` |
| banco de canales | `chstore_export()` (el slot en uso) |
| libreta de contactos | `cstore_export()` |
| mensajes | `messaging_serialise()` |
| zonas | `zones_export()` |

## Dónde

En la mitad libre de `storage_partition` (`0x2B0000` + `0x7000`), en **dos áreas
de 16 KiB**. La partición queda así:

| rango | uso |
|---|---|
| `+0x0000` | ajustes (sector de 4 KiB) |
| `+0x1000` | VFO |
| `+0x6000` | caja negra (registro de post-mortem) |
| `+0x7000` .. `+0x9FFFF` | copias de seguridad: área A y área B |

El banco de canales sigue viviendo donde siempre (`0x3C0000`); la copia lo
guarda como dato, no lo mueve.

## Formato

Cabecera de 80 bytes: `BKUP`, versión del formato, número de secuencia, fecha y
hora, tamaño total, CRC-16 de cabecera y carga (con relleno), y el **directorio**
de las seis entradas con desplazamiento, tamaño y CRC de cada una. Después, las
cargas alineadas a 4 bytes, que es lo que acepta la NOR del C62.

Dos propiedades son el diseño entero:

- **Una copia nunca cuesta la anterior.** Se escribe en el área que *no* tiene
  el registro válido más nuevo, y **la cabecera va al final**: un corte de luz a
  mitad deja un área sin cabecera utilizable y el registro anterior sigue siendo
  el bueno.
- **Una restauración verifica antes de escribir.** Primero magic, versión,
  directorio y CRC de todo el registro; si algo no cuadra se rechaza y **no se
  toca ni un byte** de los almacenes vivos. Una versión distinta se rechaza
  entera, porque las cargas son los formatos de cada almacén y reinterpretarlas
  pondría los campos en el sitio equivocado.

## En la radio

`Menú ▸ Backup` dice qué se va a guardar; verde guarda y contesta
`BACKUP OK nnnn B`. `Menú ▸ Restore` enseña el registro disponible con su fecha
y tamaño y pregunta `Restore it?` con Sí/No; restaurar es destructivo y por eso
pide confirmación. El resultado dice cuántos de los seis almacenes se aplicaron.

El motor es C portable (`core/backup_image.c`), habla con la flash por un puerto
(`platform/targets/c62/backup_c62.c`; en el PC,
`platform/targets/linux/emulator/backup_host.c`, que usa un fichero del
directorio de estado de la ejecución) y con los almacenes por sus APIs públicas,
así que los tests lo ejercitan sin radio.

## Límites

- Un área son 16 KiB: cabe el banco en uso y los otros cinco almacenes con
  holgura, pero no un banco lleno hasta el último canal más mensajes largos; en
  ese caso la copia se rechaza con `BKP_ERR_TOO_BIG` en vez de escribir a medias.
- Restaurar deja los almacenes recargados en RAM cuando se puede en caliente; si
  un almacén lo pide, la pantalla lo dice y hay que reiniciar.
- La copia es interna: no sale de la radio. Para sacarla a un ordenador está el
  volcado completo por el programador (ver `FLASH.txt` del paquete).
