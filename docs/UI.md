# User interface

How the C62's UI is laid out, which key does what on every screen, and what the
contextual quick menu offers in each operating mode. The reasoning behind each
item and the comparison with the stock firmware's menus are in
`reports/ui_menus_spec.md`, outside this repository.

The C62 has a 160×128 RGB565 panel and a 20-key matrix plus a side key. There
is no knob and no left/right key: `KEY_LEFT`, `KEY_RIGHT`, `KNOB_LEFT` and
`KNOB_RIGHT` never arrive from `keyboard_c62.c`. Every screen that used them
now uses P1/P2 or UP/DOWN instead.

## One convention, seven screens

The menus, the quick menu, the channel list, the contact book, the messages,
the APRS screens and the zone browser are seven small state machines written at
different times. They used to disagree on everything that matters when the
radio is in the hand: where the title goes, how tall a row is, where the
selection rectangle sits, what UP means, what green and red do, and in which
order the keys are claimed. The user-visible result was a channel list whose
title was half off the top of the screen, a selection highlight drawn above the
text it was supposed to hold, and a quick menu that worked in one mode and not
in another.

The convention below is now the only one, it lives in one place for the
geometry (`openrtx/include/ui/ui_layout.h`, `openrtx/src/ui/ui_layout.c`) and
is pinned by `tests/unit/ui_layout.cpp`:

1. **Header band.** 14 px at the top of the screen: the screen title on the
   left in the 8 pt font, an optional status on the right in the 5 pt font
   (the entry count, the battery, the transport), and a grey rule at y = 13.
   Title baseline is 11, so the 8 pt ascent (10 px) starts at y = 1: the title
   can never be drawn off the top, which is what happened when a title was
   printed on `y = 2` as if that were a top coordinate instead of a baseline.
   `ui_list_header(title, right)` draws the band and clears the screen;
   `ui_list_header_only()` for screens that paint their own background.
2. **Rows.** 12 px, 6 pt font, first row at y = 15 (just under the rule). The
   6 pt glyph box is 7 px of ascent and 2 of descent, so a row has 2 px of air
   above the text and 1 below the descenders. A baseline inside a row is
   `ui_row_baseline_at(top, row_h)`: +9 for a 12 px row, and it clamps itself
   so the ink never leaves a shorter row either. Taller rows (the channel
   list's 20 px two-line rows, the contact editor's fields) take their
   baselines from the same function.
3. **Selection.** The inverted rectangle is exactly the row: `ui_row_selection(top,
   row_h, filled)` fills `[top, top + row_h)` across the full width, and the
   text of that row is black. Hollow instead of filled means "editing this
   value" (settings pages). Because the rectangle and the baseline come from
   the same `top`, the black-on-white text always sits inside the white box.
4. **Margins.** 2 px left and right everywhere (`UI_TEXT_X`). Values are
   right-aligned on the right margin, never on a column that depends on the
   label, so a list of values reads as a column.
5. **Scroll.** One function, `ui_list_clamp(&sel, &first, count, rows)`: the
   cursor wraps at both ends of a list, the window follows it one row at a
   time, it never scrolls past the last entry (no blank lines at the bottom),
   and it never lets the selection leave the screen. Every list uses it, so
   walking 100 channels behaves like walking 8 menu entries.
6. **Keys.** UP and DOWN mean "previous" and "next" *always*, in every list;
   green is OK (open / edit / save / tune); red is back; P1 and P2 are the two
   modifiers of the screen. P1 is "minus / previous / left" and P2 is "plus /
   next / right", the same way round everywhere. A long press is a bigger step,
   never a different function, except where the table below says otherwise.
7. **Dispatch order.** `ui_updateFSM()` claims keys in one fixed order (PTT,
   periodic work, quick menu, overlays that own the keypad, global shortcuts,
   the screen switch). A new screen is inserted as one block in that order, not
   inside another screen's `case`.

## Keys

`keyboard_c62.c` scans the matrix as index `2 + row*5 + col` (the stock
numbering) and maps it to `KEY_*` through `keyMap[]`:

| Physical key | Matrix | `KEY_*`     | Meaning in this UI                     |
|--------------|--------|-------------|----------------------------------------|
| Green MENU   | r0 c0  | `KEY_ENTER` | OK / open / edit / save (per screen)   |
| P1           | r0 c1  | `KEY_F1`    | minus / previous / caret left          |
| P2           | r1 c1  | `KEY_F2`    | plus / next / caret right (long: latch the quick menu, macro grid elsewhere) |
| Red A/B      | r3 c0  | `KEY_ESC`   | back / cancel                          |
| UP / DOWN    | r1 c0 / r2 c0 | `KEY_UP` / `KEY_DOWN` | previous / next entry, or the value of the field being edited |
| Side key     | —      | `KEY_MONI`  | MONI; short or long opens the macro grid |
| Digits       | —      | `KEY_0`..`KEY_9` | frequency entry; 1..9 jump to a quick menu row; multi-tap text in the composer |

On the main screens P1 short replays the last voice prompt and P2 short opens
the quick menu. The side key is a second PTT and the mode selector: it is not a
menu key. PTT always closes the quick menu and keys the radio, and it re-arms
TX when leaving a screen that had disabled it.

Per screen:

| Screen | UP / DOWN | Green | P1 | P2 | Red |
|---|---|---|---|---|---|
| VFO frequency input | digit | Set | — | — | Cancel |
| Main VFO / MEM | cursor (FM: step) | menu | replay prompt (long: tools menu) | quick menu (long: macro grid; FM: APRS) | — (S-meter) |
| Tools menu | previous / next row (P1/P2 too) | run the row | previous row | next row | back to the main screen |
| Tools menu, mode submenu | previous / next mode (P1/P2 too) | apply the mode and leave | previous mode | next mode | back to the tools list |
| Menu (top, banks, settings, backup, info, about) | previous / next | open | — | — | back |
| Backup | — | save, then `BACKUP OK` | — | — | back |
| Restore | — | ask `Restore it?` (Yes/No) | — | — | back / No |
| Settings pages | previous / next | edit | — | — | back (leaves edit first) |
| Settings page of a mode (NXDN, dPMR, P25, YSF, D-STAR, AM, WFM) | previous / next row | cycle the value | value − (long: coarse) | value + (long: coarse) | back |
| Settings page, editing | change value | confirm | — | — | leave edit |
| Default settings | — | Reset, then Reset again | — | — | back |
| Quick menu | previous / next row | cycle value / open screen | value − (long: coarse) | value + (long: latch; FM main screen: APRS) | close |
| Channel list | previous / next | tune | new (duplicate / from the VFO) | edit (long: delete) | exit |
| Channel editor | next / previous field | edit field / apply | caret left, DMR: timeslot | save; in a field: caret right | cancel (leaves the field first) |
| Contacts list | previous / next | edit | new | edit (long: delete) | back to the menu |
| Contacts editor | next / previous character | save | caret left, on MODE: transport − | caret right, on MODE: transport + | discard |
| Messages inbox | previous / next | open | switch to Sent | reply | back |
| Messages sent | previous / next | status page | switch to Inbox | cancel the queued message | back |
| Message view | scroll text | reply | — | — | back |
| Compose, row mode | next / previous row | start typing / send | target − | target + | back (draft kept) |
| Compose, typing | — | done, next row | backspace | space | leave typing, keep the draft |
| Delivery status | — | back | — | cancel | back |
| APRS station list | previous / next | detail | inbox | setup | back to the main screen |
| APRS setup | previous / next field | save to NVM | value − | value + | back to where it was opened |
| Zone browser, zones | previous / next zone | open the zone | — | — | back to the menu |
| Zone browser, entries | previous / next entry | tune and leave | back to the zones | — | back to the zones |

## Layout

`_ui_calculateLayout()` fixes these sizes for the main screens; the list screens
take theirs from `ui_layout.h`. The top bar carries, from left to right, the
short name of the mode (6 pt, `UI_MODE_LABEL_MAX_W` wide at most, then the keypad
lock symbol), the clock (centred) and the battery (right):

| Element | Size | Font |
|---|---|---|
| Top bar of the main screens | 16 px | 8 pt (clock), 6 pt (mode label, left) |
| Header band of a list screen | **14 px** | 8 pt title, 5 pt status |
| List rows | **12 px** | **6 pt** |
| Channel list rows (two lines) | 20 px | 6 pt + 5 pt |
| Editor fields | 12 px | 5 pt label, 6 pt value |
| Bottom S-meter | 23 px | 8 pt |
| Key hints row | **11 px** | **5 pt** |
| Frequency input | 12 pt | 12 pt |

Eight 12 px rows fit between the header and the hints
((128 − 15 − 11) / 12 = 8.5); the channel list, with its two-line rows, shows
five. No list draws over the hints row: `ui_rows_fit()` is the only place that
answers "how many fit".

The hints row is drawn by `ui_hints.c` for every screen that returns a hint
table, at the end of `ui_updateGUI()` (`ui_hints_draw_for_screen()`), and by the
modules that own their screen (channels, zones) through `ui_hints_draw()`. Each
entry is `KEY:action` in the 5 pt font, the key in yellow and the action in
white; four short hints fit in the 152 px of usable width. `P1`/`P2` are drawn
with their physical names, the green and red keys without a prefix, because
that is how the stock manual names them.

### Quick menu metrics

The popup uses the **6 pt** font and the same header band as every list. The
numbers are pinned by `tests/unit/ui_quickmenu.cpp`:

| Quantity | Value | Why |
|---|---|---|
| `QM_TOP_Y` | 16 px | below the top bar |
| `QM_HEADER_H` | 14 px | 8 pt title and frequency on the baseline at 11, rule at 13 |
| `QM_FONT` | 6 pt | the same as the lists: one font for one kind of text |
| `QM_ROW_H` | **12 px** | 7 px ascent + 2 px descent + 3 px of slack |
| `QM_ROW_BASELINE` | **9 px** | ink at +2..+8, descenders at +10, inside the row |
| `QM_TEXT_X` | 2 px | left margin of the labels |
| `QM_VALUE_X` | 2 px | right margin of the values |
| Visible rows | **7** | (128 − 11 − (16 + 14)) / 12 = 7.25 → 7 |

The history of this table is the bug report: the popup was first drawn with the
6 pt font on a baseline one pixel below the row top and a 12 px rectangle
starting one pixel above it, so the glyphs sat five pixels *above* the
selection rectangle and the black-on-white text fell on black. Shrinking to
5 pt hid the overlap but made the text too small to read at arm's length. Now
the row is 12 px, the baseline comes from the same `qm_rowTop()` as the
rectangle, and `qm_adjust_step()` keeps a long press for a coarse jump.

Values are right-aligned and measured in the row font: if a value does not fit
between the label and the right margin it is shortened from the left, where a
frequency keeps its least significant digits, and prefixed with `<` so a
shortened value never looks like a real one. At 6 pt (7 px per character) the
widest realistic row is `SEARCHING` at 63 px, which fits in the 96 px left by
the widest label.

## State machines

Every screen is a small FSM that owns the keypad while it is open. The order of
the dispatch in `ui_updateFSM()` is what keeps them from swallowing each
other's keys:

1. PTT: closes the quick menu, unlatches the macro grid.
2. Periodic part (runs with an empty event queue): quick menu timeout and mode
   tracking, message poll and draft flush.
3. Event pop, low battery check.
4. Quick menu, if open: consumes everything.
5. P2 short on the main screens: opens it.
6. APRS screens, then messaging screens, then the contact book, then the zone
   browser: each consumes everything while its screen is on and keeps
   `state.txDisable` true.
7. P2 long on the FM main screen: APRS, and the popup it may have opened on the
   way is closed first. Then MONI / P2 long: macro grid.
8. Channels: P1 long opens the tools menu, whose last row is the channel list;
the module consumes everything while open. The backup and restore screens are
reachable from the top menu and are documented in `docs/BACKUP.md`.
9. The screen switch (main screens, menus, settings).

Everything an overlay opens goes through `ui_open_screen(screen, item)` and
nothing else. That one function remembers the main screen to come back to,
clears a stale `edit_mode` (a leftover edit mode made the first key on the new
screen mean "leave edit" instead of "move"), calls the module's own `open()`,
selects the requested row — the quick menu's `CALL` and `META` used to land on
the first field of the settings page — and arms or disarms the transmitter the
way the destination screen needs. `MSGS` and `MSG` from the popup therefore
behave exactly like the same entries in the menu.

Transitions, briefly:

| FSM | States | Enter | Leave |
|---|---|---|---|
| Quick menu | closed → open (latched / unlatched) | P2 short on `MAIN_VFO`/`MAIN_MEM`, unlocked, not editing | ESC, PTT, unlatched timeout (`QM_TIMEOUT_MS` = 15 s), `QM_GOTO` row |
| Channels | `CHS_CLOSED`/`LIST`/`EDIT`/`CONFIRM` | P1 long, or Menu → Channels | ESC on the list; green on the list tunes, closes, and leaves the radio on `MAIN_MEM` |
| Contacts | `CTS_CLOSED`/`LIST`/`EDIT`/`CONFIRM` | Menu → Contacts | ESC on the list (back to the menu) |
| Zones | `ZS_CLOSED`/`ZONES`/`ENTRIES` | Menu → Zones | ESC from the zone list (back to the menu); green on an entry tunes and goes to the VFO |
| Messages | inbox / view / sent / compose / status | Menu → Messages, quick menu `MSGS`/`MSG`, or reply from a received message | ESC from the inbox (to the menu), from sent (to the inbox), from compose (inbox, draft kept) |

The quick menu has no edit mode. A modal editor made sense on a radio with
arrow keys; here it meant that the only way to change an integer was ENTER
followed by UP/DOWN, and enumerations could not be reached at all. P1/P2 change
the value of the highlighted row directly, integers clamp at `min`/`max`,
enumerations and toggles wrap, read-only rows beep, and green is exactly P2
except on `QM_GOTO` rows, where it opens the screen. Green never closes the
popup.

`qm_tick()` compares the operating mode with the one the table was chosen for
(`qm_state.mode`), not with the table's own `mode` field: a mode without a table
falls back to the FM one, and comparing contents there rebuilt the popup on
every UI pass — the cursor jumped back to the first row as soon as it was
moved. That was the "the quick menu stops working with some options" report.

## Herramientas (P1 mantenido)

P1 mantenido en las dos pantallas principales abre el menú de herramientas
(`openrtx/src/ui/default/ui_tools.c`, pantalla `TOOLS_MENU`). Antes ese atajo
abría la lista de canales; la lista es ahora la última fila del menú, así que el
atajo no se pierde y el escáner, la búsqueda en VFO, el analizador y el selector
de modo ganan la puerta que no tenían.

Cabecera `TOOLS` (8 pt) con la batería a la derecha, filas de 12 px con la
fuente de 6 pt sobre `ui_row_baseline_at()`, etiqueta a la izquierda y valor
alineado a la derecha, y la pista de teclas en la última fila del panel:
`OK`, `P1:Up`, `P2:Down`, `Back`.

| fila | valor a la derecha | qué hace el verde |
|---|---|---|
| `Mode` | el modo en uso | abre el submenú de modos (no cierra el menú) |
| `Scan` | canales del banco, `12 CH` | arranca el escáner de memorias (`scan_service_start(SCAN_MODE_MEMORY)`) y cierra el menú |
| `VFO scan` | `UP` | arranca la búsqueda de portadoras en VFO (`vfoSearchService_start(VFOS_UP)`), la misma que tres segundos de arriba o abajo |
| `Spectrum` | — | abre el analizador en barrido, la pantalla `SPECTRUM`, centrada en la frecuencia del radio |
| `Channels` | canales del banco | abre la lista de canales (`docs/CHANNELS.md`) |

Reglas:

- **Las filas son de compilación.** Sin `CONFIG_SCAN` no hay fila `Scan`; sin
  `CONFIG_VFO_SEARCH` no hay `VFO scan`. Ninguna fila es una tecla que no hace
  nada, y el valor de la derecha dice de antemano lo que hay (un banco vacío se
  ve como `0 CH` antes de pulsar).
- **Mientras está abierto, el menú reclama todas las teclas**, MONI y el P2
  largo incluidos (`UI_CLAIM_TOOLS`), y desactiva el TX (`state.txDisable`): la
  misma regla que la lista de canales, desde un menú no se clavea.
- **Al cerrar, la pantalla vuelve a la que estaba** (`MAIN_VFO` o `MAIN_MEM`, que
  se recuerda al abrir) y `_ui_syncTxDisable()` vuelve a armar el PTT. Si la
  fila abrió un módulo que reclama el teclado —la lista de canales, el
  analizador—, el TX sigue desactivado hasta que ese módulo se cierre.
- **`VFO scan` desde `MAIN_MEM`** devuelve primero la VFO (lo mismo que hace el
  rojo en la pantalla de memoria) y barre desde ahí: la búsqueda sólo arranca en
  la pantalla de VFO (`core/vfo_search_service.c`).
- **`Scan` no interrumpe una llamada**: el servicio para el escáner al ver el PTT
  y devuelve el receptor donde estaba (`docs/SCAN.md`). Abrir el menú **para un
  scan de memorias en curso**: el menú es la puerta a cambiar de canal, de modo
  o de frecuencia, y `docs/SCAN.md` pide pararlo antes de cualquier cambio
  manual; además la fila `Mode` mostraría el modo del canal que el scan está
  sintonizando en ese momento.
- **`Spectrum` puede negarse**: un modo digital necesita el discriminador y un
  receptor parado no tiene RSSI para barrer; entonces pita y deja la pantalla
  donde estaba (`docs/SPECTRUM.md`).

### Submenú de modos

Cabecera `MODE` y, a la derecha, el modo en uso. Una fila por modo del registro
(`ui/ui_modes.h`), en su orden, con la capacidad a la derecha: `TX` si el modo
puede transmitir en esta imagen, `RX` si es de sólo recepción, para que elegir un
modo de recepción no engañe. Se abre con el cursor sobre el modo en uso, así que
el modo actual es la fila resaltada.

Verde aplica el modo elegido por la puerta de siempre —`state.channel.mode` más
petición de resincronización del RTX—, deja `tx_frequency` igual a
`rx_frequency` en los modos de sólo recepción, lo anuncia por voz y vuelve a la
pantalla principal. Rojo vuelve a la lista sin salir del menú.

## Channel list and editor

P1 long press on the main screens, or Menu → Channels, opens the in-radio
channel bank (`ui_channels.c`, `docs/CHANNELS.md`). The menu entry exists
because the CPS list it used to open is empty on this radio (no codeplug,
`cps_io_stub`). The module takes over the keypad and the screen until it
closes, holds `state.txDisable` true so nothing can transmit from it, and
handles the keys before the screen switch.

It draws the shared header band and 20 px rows: the channel name and its index
in 6 pt on the baseline at +9, the frequency, mode and flags in 5 pt on the
baseline at +19, both inside the inverted rectangle. The title used to be
printed on y = 2 (a baseline, so its ink was at −5..+1) and the rows on y+1,
which is why the title was half off the screen and the highlight was drawn
above its text. It does not draw its own hint bar: `ui_channels_drawHints()`
builds a `ui_hints_t` and calls `ui_hints_draw()`, so the bar is identical to
every other screen.

While the list is open the quick menu does not open: P2 there is EDIT, and the
underlying screen is still `MAIN_VFO`/`MAIN_MEM`, so `qm_open()` is gated on
`!ui_channels_isOpen()`.

The editor holds every mode the registry marks as a channel mode
(`UI_MODE_CHANNEL`): FM, M17 with `CONFIG_M17`, DMR with `CONFIG_DMR`
(`CONFIG_C62_DMR_RX`), TETRA with `CONFIG_TETRA`, NXDN with `CONFIG_NXDN`
(`CONFIG_C62_NXDN_RX`), dPMR with `CONFIG_DPMR` (`CONFIG_C62_DPMR_RX`), P25
with `CONFIG_P25` (`CONFIG_C62_P25`), YSF with `CONFIG_YSF` (`CONFIG_C62_YSF`),
D-STAR with `CONFIG_DSTAR` (`CONFIG_C62_DSTAR`), and airband AM and broadcast
FM always (neither has a build option). There is no list here any more: NXDN and
AM were reachable from the keypad and not from a channel precisely because this
file had its own copy of the modes.

`BW` is per mode, not a plain 12.5/25 toggle: FM walks 12.5 / 25 / 6.25, NXDN
12.5 / 6.25 (its channel spacing) and AM 25 / 12.5 (the wide or narrow IF of the
AM detector), and picking a mode that cannot use the current value resets it to
that mode's default. The record (`chstore_channel_t`) carries the mode, the
bandwidth, both tones, the DMR colour code and timeslot; the editor offers them
all: `DMR CC/TS` is editable, UP/DOWN walks the colour code 0..15 and P1/P2 flip
the timeslot. Inside a field, UP/DOWN change the value (long press moves a
coarser digit) and P1/P2 move the caret through a name or a frequency.

## Contacts

Menu → Contacts opens the in-radio book (`ui_contacts.c`, `core/contact_store.c`),
not the CPS contact list. It holds 32 entries; a contact is what gives a
message its address, so the book is what the composer walks. The list shows the
callsign in 6 pt and the preferred transport in 5 pt on the shared 12 px row;
green or P2 edits, P1 adds, P2 long deletes after a confirmation, red goes back
to the menu.

Fields: `CALL` (10 characters, callsign or DMR id), `NAME` (16 characters) and
`MODE` (the preferred transport, used to pre-select a recipient when
composing). UP/DOWN change the character under the caret, P1/P2 move the caret
or change the transport, green saves, red discards. A contact with an empty
callsign is never written.

A save that fails is a save that did not happen: the record is taken back out of
the book and the count restored, so the list never shows a contact that is not
on flash (`core/contact_store.c`, `tests/unit/user_stores.cpp`). The editor
answers `WRITE FAILED` and stays open, and both answers agree.

The book lives in the OEM channel partition, 0x8000..0xC000 (two 8 KiB
ping-pong slots), next to the channel bank; see
`platform/drivers/NVM/user_store_c62.c`. Every save is CRC-checked and read
back, so a power cut costs the save and never the book.

## Messages

Menu → Messages (with an unread badge: `Messages !3`), or the `MSGS` row of the
quick menu, opens the inbox; the `MSG` row opens the composer directly. The
screens only know the shared message store (`core/messaging.h`): one outbox, one
inbox, one set of states for APRS, DMR SMS and TETRA SDS. `core/msg_service.h`
is the thin layer between the two: it says which transport can send in the
current mode, and turns the recipient on screen into the address the transport
puts on the air.

| Screen | Content | Keys |
|---|---|---|
| Inbox | `*` unread, peer, transport | UP/DOWN, green opens, P1 switches to Sent, P2 replies, red back |
| View | header peer + transport, text in 5 pt | green replies, red back |
| Sent | peer and delivery state | green opens the status page, P2 cancels a queued message, P1 switches to the inbox |
| Compose | rows `TP`, `TO`, `TXT`, `SEND` and a character counter | see below |
| Status | state word, `try n of 3`, text | P2 cancels, green or red back |

Compose: UP/DOWN move between rows. On `TP`, P1/P2 or green cycle through the
transports available for the current mode (APRS needs FM, DMR SMS needs DMR,
SDS needs TETRA); if none is available the row reads `NO LINK` and sending is
refused. On `TO`, P1/P2 walk the contact book and green starts typing. On
`TXT`, green starts multi-tap typing:

| Key | In typing mode |
|---|---|
| `2`..`9` | letters of the group (`ABC`, `DEF`, …), repeated presses cycle within the group; a 1.2 s pause starts a new character |
| `0` | space, `0`, `-`, `_`, `/`, `@` |
| `1` | `.`, `,`, `?`, `!`, `1` |
| long press of a digit | the digit itself, for SSIDs |
| P1 | backspace |
| P2 | space |
| green | end typing, save the draft, move to the next row |
| red | end typing, keep the draft |

Delivery states are `QUEUED`, `TX`, `SENT`, `ACK`, `FAIL` and `CANC`, and they
are the transport's real state. Retries are part of the store (three attempts,
then `FAIL`) and the status page shows the count.

Everything is persistent, and all of it is the one record of the shared store
(`docs/MESSAGING.md`): inbox (6), outbox with its terminal states (6), the
unread flags and the compose draft, in the two alternating sectors at
0x4000/0x5000 of the storage partition.

## Zones

Menu → Zones opens the zone browser (`ui_zones.c`): the repeater zones `R`
(R0..R7) and `U` (U0..U100), and the PMR zone (PMR1..PMR16), each entry with its
nomenclature and its frequency, and the transmit power shown for the entries
under 1 W — the PMR channels are marked `500mW` because their table says 500 mW.

The browser owns no frequency: the tables are the data model's
(`core/zones.h`: `zones_at()`, `zones_channels()`, `zones_channel()`,
`zones_toChannel()`), and a zone is a base frequency, a step, a shift and a
count that the model resolves on demand. `ui_zones_source_t` +
`ui_zones_setSource()` is the seam for a target that gets its tables from
elsewhere (and for the tests, which is how `tests/unit/ui_zones.cpp` exercises
the nomenclature, the scroll and the 500 mW mark). When the header is not in
the tree the list shows `NO ZONES`; the UI never invents a frequency. Green
tunes the entry into the VFO (through `zones_toChannel()`, so the bandwidth and
the shift are the plan's) and leaves; red goes back one level.

## El estado del modo (the mode's state, on the main screen)

The owner's rule, and the reason this section exists:

> el estado del modo tiene que aparecer en la pantalla principal, no en un
> subsubmenu. en los menús están las configuraciones y las opciones.

A receive mode's decoded state — is it locked, to what, who is calling whom, how
good the signal is — is what the operator watches while working the radio. It
used to be read-only rows of each mode's quick menu, which is *also* that mode's
page under Settings, so it could only be read by opening a menu, and the page was
a list of rows that beeped and did nothing when a value key was pressed.

`ui/ui_mode_state.h` is now the one block every digital mode fills, and
`ui_main.c` draws it in the same place in all of them (`_ui_drawModeState()`):

| Line | Content | Examples |
|---|---|---|
| 1 | the call and the received quality | `2317>9 LC98%`, `e4% f1234`, `2317>9 ok99%`, `b2 ED5YAM` |
| 2, left | identifiers and sync state | `CC1 TS2`, `RAN12 VOX`, `NAC293 VOX`, `228-8889`, `EA5ABC>CQCQCQ`, `SRCH` |
| 2, right | the secondary state | `ACC G3` (TETRA registration), `DUID3`, `VCALL`, `CSBK`, `LA1 ACC G3` |

The frequency keeps the 16 pt row between them and the bottom row stays the
S-meter (or the carrier-search message), so the state is the same two bands in
every mode: line 1 at `layout.line1_pos` and line 2 at `layout.line2_pos`, from
the one `layout_t` every screen shares. There is no fourth line: the 16 pt
digits reach y=93 and `layout.line4_pos` is y=97, so a note there is drawn over
the frequency — the simulator showed it (`mode_state_shots.py`, the first version
of this block).

Both halves of line 2 are measured with `gfx_getTextWidth()` before they are
drawn: the note takes its short form (`ACC G3` instead of `LA1 ACC G3`) when the
long one does not fit beside the identifiers, and is dropped when neither does.
The identifiers are never shortened. `gfx_print()` does not clip, so measuring
first is what keeps a long text from wrapping onto the frequency.

Nothing decoded reads as `SRCH` (TETRA: `T searching`) and an identifier that is
not known reads as `-`, never as a zero: a colour code of 0 is a fact and "not
decoded yet" is not. `tests/unit/ui_menu_audit.cpp` checks both the vocabulary
and the positive cases, and rejects any read-only (`QM_INFO`) row in a mode's
table, so the state cannot creep back into a menu.

What each mode publishes, and where its quality figure comes from:

| Mode | Identifiers (line 2) | Call / quality (line 1) | Note (line 2, right) |
|---|---|---|---|
| DMR | `CC n TS n` | `src>dst`, `LC n%` (embedded LC decoded) | data type (`CSBK`, `Idle`…) |
| NXDN | `RAN n VOX` | `src>dst`, `e n% f n` (EVM, frames) | `VCALL` / `TX_REL` / `DISC` |
| dPMR | `dPMR RX` / `SRCH` | — (the PHY publishes no counters) | — |
| P25 | `NAC nnn VOX` | `src>tg`, `ok n% f n` (frames without uncorrectable errors) | `DUID n` |
| YSF | `YSF RX` / `SRCH` | — | — |
| D-STAR | `MY/SFX>UR` | `b n RPT` (Viterbi-corrected bits, access repeater) | slow-data text |
| TETRA | `MCC-MNC` | uplink SSI / SDS / demodulator figures (line 1, radio) | `LA n` + `ACC G n` / `REJ n` |

FM, M17, airband and broadcast FM keep their own lines: FM shows its bandwidth
and tone, M17 the link set-up frame with symbols, source, destination, reflector
and meta text, and the two receivers the tuner's own read-out (the airband
channel designator and TX arm state, the BK1080's stereo state, level and SNR).

Where the state is *not*: the TETRA network broadcast (neighbour count, cell
load, network clock) does not fit beside the cell on a 160 px line and is a
diagnostic, not something the operator works with; the D-STAR slow-data text is
also stored in the message inbox. Everything else that the tables used to show
read-only is above.

## Contextual quick menu

P2 short press, on the main screens only, with the keypad unlocked and not
editing a frequency. `ui_quickmenu.c` owns the keypad while it is open: UP/DOWN
move the cursor, P1/P2 change the highlighted value (long press for the coarse
step of the rows that declare one), green cycles it or opens its screen, ESC
closes, digits 1..9 jump to a row, and PTT closes the popup and keys the radio.
The popup opens unlatched and closes after 15 s without keys; P2 long latches it
-- except on the FM main screen, where a long press is the APRS entry and the
popup closes: the keypad reports the press and, 700 ms later, the long press, so
the popup is open by the time the long press arrives, and the APRS entry is
decided before the popup takes the key. It has to be that way round, or holding
P2 in FM only latches the popup and the station list cannot be reached from the
keypad at all (`tests/unit/ui_aprs_entry.cpp`). Anywhere else P2 long latches
the popup, so it stays until ESC.

The popup covers the screen: header (mode title left, frequency or mode state
right) in the 8 pt font, a rule, **seven rows of 12 px in the 6 pt font** (the
cursor row is inverted), and the hints row.

Rows marked *compile-time* appear only when the platform defines the feature
(`CONFIG_KEY_BACKLIGHT`, `CONFIG_RX_FINE_OFFSET`, `CONFIG_APRS`, `CONFIG_DMR`),
so no row is ever a dead option.

There is no `MODE` row: the mode is changed with the side key, and a row that
rebuilt the whole table under the user's finger was one of the ways the popup
broke.

### FM (13 rows)

| Row | Type | Range / values |
|---|---|---|
| `SQL` | int | 0..15, step 1 |
| `PWR` | int | 1000..5000 mW, step 500, shown as `1W`..`5W` |
| `CTCSS` | enum | OFF / TX / RX / BOTH |
| `TONE` | enum | 0..49 CTCSS tones, shown in Hz (long press: ±10) |
| `BW` | enum | 12.5 / 25 / 6.25 kHz |
| `SHIFT` | enum | OFF / −600k / +600k / −5M / +5M |
| `1750` | toggle | 1750 Hz burst on TX |
| `BKL` | int | 5..100 %, step 5 (never 0: the radio has no orderly shutdown) |
| `KEYLIT` | toggle | keypad backlight, compile-time |
| `LOCK` | toggle | keypad lock |
| `MSGS` | goto | opens Messages, shows the unread count |
| `MSG` | goto | opens the composer, transport chosen by the current mode |
| `APRS` | goto | opens the APRS setup page (`CONFIG_APRS`), red returns to the main screen |

### M17 (10 rows)

`CALL` (opens Settings → Identity → callsign, the station's one callsign), `CAN`
0..15, `CANRX` toggle, `META` (opens the meta text editor),
`PWR`, `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG`.

`CALL` is a rename: the row used to be `DEST` and opened the *own* callsign
editor, which is the source address and not a destination. The destination of an
M17 call is not settable from this radio.

### TETRA, receive only (8 rows)

`AFC` toggle, `FINE` −500..+500 Hz in 10 Hz steps (long press: ±100 Hz),
`CELL UL` (`OFF` / `ON` / `NO UL`: listens to the uplink of the cell — the MS
to BS direction — see docs/TETRA_LIB.md, "Escuchar el uplink de la celda"), then
`BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG`.

The cell, its location area, the registration and the call are state and are on
the main screen (see "El estado del modo"): `MCC-MNC` and `LA`/`CC` on line 2,
`ACC G<n>` or `REJ <cause>` at its right, and the talking party and the called
group on the call screen, which takes lines 1 to 4 while a call is up.

`CELL UL` is a **session of the mode**, not a setting: `ON` tunes the receiver to
the cell's uplink frequency (published by its SYSINFO, or the duplex conversion
of its downlink) and seeds the receiver with the cell's colour code; `OFF` puts
it back on the frequency it came from, moving rx **and** tx together because
the mode is receive-only. With no known uplink frequency the row says `NO UL`
and nothing is retuned — it never invents one. Leaving the mode or tuning
elsewhere ends the session (the periodic part of `ui_updateFSM()` does it, so a
mode that can transmit never inherits an uplink frequency as its TX).

### DMR (8–9 rows)

The call destination — `CALL` (`GROUP` / `USER`) and `DEST` (the talkgroup or
the subscriber ID, 24 bits, step 1, long press ±100) — then `FINE`, `BKL`,
`KEYLIT`, `LOCK`, `MSGS`, `MSG`.

The colour code, the timeslot, the sync state, the call and the link quality are
state and are on the main screen (see "El estado del modo").

`DEST` writes `settings.dmr_dest_id` / `settings.dmr_dest_type` and asks for an
RTX resync; the UI never touches `rtxStatus_t`. The two rows only exist when
the settings record has them (`DMR_ID_MAX` from `core/datatypes.h`, the model
branch) and the build has DMR; until then the table builds without them.

No mode's table carries a read-only row any more: a table is both the popup and
the mode's Settings page, so a read-only row there is state inside a menu.
`tests/unit/ui_menu_audit.cpp` fails when one comes back.

### NXDN, receive only (7–8 rows)

`RAN` (0..63 plus `ANY`, the default: it is a receive *filter*, and the mode
drops voice and layer 3 from any other RAN once a superframe has decoded) and
`BW` (12.5 / 6.25 kHz, the channel spacing the IF filter follows), then `FINE`,
`BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG`.

What the demodulator reports — sync, the RAN it locked to, the EVM, the frames
and the source > destination of the last SACCH message with its type — is on the
main screen (see "El estado del modo").

### dPMR, receive only (5–6 rows)

dPMR is one 6.25 kHz carrier with no timeslots and no channel spacing to choose,
and its PHY publishes no counters to the UI yet, so the table is `FINE`, `BKL`,
`KEYLIT`, `LOCK`, `MSGS`, `MSG`. Whether a call is being followed is on the main
screen: `dPMR RX` while the squelch is open (it opens with the frame grid and
the CCH CRC), `SRCH` otherwise.

### P25 Phase 1, receive only (5–6 rows)

P25 has nothing to configure of its own, so the table is `FINE`, `BKL`,
`KEYLIT`, `LOCK`, `MSGS`, `MSG`. The receiver's state is on the main screen: the
NAC it locked to (three hex digits) on line 2, the data unit of the last frame
at its right, the source > talkgroup of the last link control and the share of
voice frames that needed no uncorrectable repair on line 1.

### YSF, receive only (5–6 rows)

The frame layer does not publish a status block yet (`docs/RX_FUSION.md`), so
the table is `FINE`, `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG` and the main screen
says `YSF RX` while the mode tracks a carrier, `SRCH` otherwise.

### D-STAR, receive only (5–6 rows)

The callsigns are the mode, and they are state: the main screen shows
`MY/SFX>UR` on line 2, the access repeater and the Viterbi-corrected channel bits
of the last header on line 1, and the slow-data text at the right of line 2 (also
stored in the message inbox). The table is `FINE`, `BKL`, `KEYLIT`, `LOCK`,
`MSGS`, `MSG`. Nothing reads `---` any more: with no header the screen says
`DSTAR SRCH`.

### WFM, broadcast FM, receive only (5 rows)

Everything the BK1080 reports is on the main screen, which is this mode's whole
read-out: the stereo state (`STEREO`, `PILOT`, `MONO` or `SRCH`) and the CCIR
channel designator on line 2, the level in dBuV and the chip's own 0..15 SNR on
line 1. There is no `SQL` and no `PWR` row: the tuner's AFC says whether there
is a station, there is no transmitter, and the volume is the analog pot. The
table is `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG`.

### AM, airband, receive only (10–11 rows)

`SQL` is the **carrier** squelch: the same level as FM, but in AM the audio
follows the carrier, so what it thresholds is the RSSI — a noise squelch would
open on the noise the AGC has just brought up. `AGC` turns the audio AGC of the
AM path on and off. `STEP` picks the raster (25 kHz or 8.33 kHz) and moves the
global tuning step with it, `SNAP` puts every step and every typed frequency on
the nearest 8.33 kHz carrier, and `IF` picks the AM IF filter (25 kHz wide /
12.5 kHz narrow). No `PWR` row: there is no transmitter in this mode. Then
`FINE`, `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG`.

The channel designator the receiver is on (`118.005`, whose carrier is
118.00833) is state and is on the main screen next to the raster, with the AGC
state, the squelch level and — with `CONFIG_C62_AIR_TX` — whether the band is
armed to transmit (`TX-ARM`) or locked (`TX-LOCK`).

## Frequency entry

Eight digits, 10 Hz resolution (the BK4819's step): `446.00625`. The dot is
inserted after the third digit. `FREQ_DIGITS` is 8 and
`FREQ_DIGIT_RESOLUTION` is 10, both in `ui_default.h`.

Green is **Set**: it applies what has been typed and returns to the VFO, even
if the field has not reached its eighth digit. In FM, M17 and DMR there are two
fields, RX and TX: the eighth RX digit moves to the TX field, UP/DOWN switch
between them at any time, and an empty TX field takes a copy of RX, which is
what makes a simplex entry a single frequency. In a receive-only mode
(`opmode_canTransmit()` false: TETRA, and DMR only in a build without
`CONFIG_DMR_TX`) there is a single frequency and the eighth digit applies it.
A frequency outside the tuning range is not silently dropped: the radio says
so and stays in the input screen. The simulator's tour asserts both shapes, one
against the other (`phase_dmr_tx`, docs/SIMULATOR.md).

Until the fourth pass green did something else when the RX field was being
edited: it moved to the TX field and applied nothing, so "I type the frequency
and nothing happens" -- the receiver stayed where it was and the field went
blank. `tests/unit/ui_regressions.cpp` holds the entry to the behaviour above.

## Fine RX tuning

`settings.rxFineHz` is a manual offset of the receive frequency, ±500 Hz in
10 Hz steps, offered in the TETRA and DMR quick menus. It is applied at every
retune by `radio_C62.cpp` as `channel + rxOffset + settings.rxFineHz`, where
`rxOffset` is TETRA's automatic correction. Keeping the two apart is what makes
them coexist: the AFC loop only moves its own value, the user only moves the
setting, and neither can overwrite the other.

## Mode registry

The modes this build has are one table, not five lists. `uiModes[]` in
`openrtx/src/ui/default/ui_quickmenu.c` (declared in `openrtx/include/ui/ui_modes.h`)
holds, per mode: the `OPMODE_*` value, its title, its quick menu rows and its
capabilities as flags. Everything that used to be written by hand is filtered
from it:

| Screen | How it reads the registry |
|---|---|
| Channel editor, `MODE` field | `ui_mode_inChannelList()` — `UI_MODE_CHANNEL` |
| Side key, mode cycle | `ui_mode_next()` — `UI_MODE_SELECT`, in table order |
| Quick menu | `qm_findMenu()` → the entry's rows; FM if the mode has none |
| Settings → `<mode>` | every entry with `UI_MODE_PAGE`, in table order (NXDN, dPMR, P25, YSF, DSTAR, AM, WFM in this build) |
| Header of the main screens | `ui_mode_label()` → `chstore_modeName()` |
| Channel records, channel list | `chstore_modeName()` — the only place a mode is spelled |

The mode label sits in the top-left of the main screens' top bar, in the 6 pt
font, and it is measured before it is drawn: it has `UI_MODE_LABEL_MAX_W` (40 px)
and is cut if it does not fit, so it can never reach the clock in the middle.
`tests/unit/ui_modes.cpp` fails if a registered label is wider than that, which
is why the mode is called `DSTAR` and not `D-STAR`.

A mode's page in Settings is **its own quick menu table**, drawn as a list
instead of a popup (`qm_page_open()`, `qm_page_draw()` in `ui_quickmenu.c`). The
rows, their limits, their formatters and their setters are therefore the same
objects, so a value cannot read one way in the popup and another in the menu, and
the "walk every row of every table" test covers both screens.

## Specifying a new mode

A new mode (dPMR, P25, YSF, D-STAR, WFM, DMR packet…) is wired in with **one
table entry plus its rows**. The steps, in order, and nothing outside them:

1. **`OPMODE_*` value.** Append it to `enum opmode` in
   `openrtx/include/rtx/rtx.h`. The value is stored in every channel record that
   carries the mode: append, never insert, and never reuse a gap.
2. **Name.** One line in `chstore_modeName()`
   (`openrtx/src/core/channel_store.c`): two to five characters, because that is
   what the header has room for (`UI_MODE_LABEL_MAX_W`).
3. **Capability flag.** If the build compiles the mode conditionally, add
   `UI_MODE_<X>` to `ui/ui_modes.h` next to the other ones
   (`#if defined(CONFIG_X)`), and use *that* flag — not the `CONFIG_` one — in the
   UI. `UI_TEST_<X>` is how the host build turns a mode's screens on without
   linking its demodulator; see the note at the top of `meson.build`.
4. **Rows.** A `static const qm_item_t qm_items_<x>[]` table in
   `ui_quickmenu.c`, next to the others: the mode's own rows first (what the
   engine can be told to do, then what it reports as `QM_INFO`), then the shared
   tail every table ends with (`FINE` if `CONFIG_RX_FINE_OFFSET`, `BKL`,
   `KEYLIT` if `CONFIG_KEY_BACKLIGHT`, `LOCK`, `MSGS`, `MSG`). Labels are at most
   ten uppercase characters and the value column has to fit; a read-only row
   shows `---` or `SRCH`, never a zero.
5. **Registry entry.** One line in `uiModes[]`:

   ```c
   QM_MODE(OPMODE_DPMR, "dPMR", qm_items_dpmr,
           UI_MODE_CHANNEL | UI_MODE_SELECT | UI_MODE_PAGE),
   ```

   `UI_MODE_TX` only if `opmode_canTransmit()` returns true for it,
   `UI_MODE_CHANNEL` to offer it in the channel editor, `UI_MODE_SELECT` to put
   it in the side key's cycle, `UI_MODE_PAGE` to give it a Settings page.
6. **Settings page (only with `UI_MODE_PAGE`).** One line in `UI_MODE_PAGES` in
   `ui/ui_modes.h`:

   ```c
   #ifdef UI_MODE_DPMR
   #define UI_MODE_PAGE_DPMR(X) X(OPMODE_DPMR, "dPMR")
   #else
   #define UI_MODE_PAGE_DPMR(X)
   #endif
   ```

   and add it to the `UI_MODE_PAGES(X)` expansion. The Settings entry, its index
   and its behaviour are generated from that list (see `enum settingsItems` in
   `ui/ui_default.h` and `settings_items[]` in `ui.c`), so nothing else changes.
7. **Persistent values.** Anything the rows store beyond `state.channel` goes in
   `settings_t` **appended at the end**, with `SETTINGS_VERSION` bumped and the
   version table in `docs/SETTINGS.md` updated. Customs defaults must reproduce
   the behaviour the mode had before the menu reached it.
8. **Status lines and S-meter.** If the mode has something to show on the main
   screen, add a `<MODE>Info()` / `<MODE>Line1()` pair in `ui_main.c` and its two
   call sites; any mode that is not FM already gets the level meter, and a mode
   without a case prints its title on line 2 instead of nothing.
9. **Voice prompt.** One case in `vp_announceRadioMode()` if the mode should be
   announced when the side key lands on it.
10. **Tests.** `allTables()` in `tests/unit/ui_quickmenu.cpp` (guarded by the
    mode's flag) makes the shared walk cover the new rows; the registry tests in
    `tests/unit/ui_modes.cpp` pick the entry up on their own. `meson test` must
    stay green, and the two firmware variants build with the same options as
    before.

The mode's engine (OpMode class, PHY, protocol sources, Kconfig/CMake option) is
a separate concern: `docs/RX_NXDN.md` and `docs/RX_AIRBAND.md` are the two worked
examples, engine and UI both.

## Screens, and who takes the key

The dispatch in `ui_updateFSM()` is a chain, not a switch with fallthrough: the
first block that recognises a key returns, and every block says which one it is
before it does (`KEY_CLAIM()` and `enum uiKeyClaim` in `ui/ui_default.h`, read
back with `ui_lastClaim()` / `ui_lastClaimCount()`). That trace is what
`tests/unit/ui_menu_audit.cpp` uses to check, screen by screen and key by key,
that **exactly one block claims each event**: a screen that claims nothing is a
key that goes nowhere (the "deaf screen" defect) and two claims are two handlers
eating the same event.

The claim order, which is also the order of the blocks in the file:

| # | Block | Claims |
|---|-------|--------|
| 1 | exit from standby | `UI_CLAIM_STANDBY` |
| 2 | carrier search running | `UI_CLAIM_SEARCH` |
| 3 | keypad locked, red held down | `UI_CLAIM_UNLOCK` |
| 4 | quick menu open | `UI_CLAIM_QM` |
| 5 | analyser screen | `UI_CLAIM_SPECTRUM` |
| 6 | P2 short on a main screen | `UI_CLAIM_QM_OPEN` |
| 7 | APRS screens | `UI_CLAIM_APRS` |
| 8 | packet modem screens | `UI_CLAIM_PACKET` |
| 9 | messaging screens | `UI_CLAIM_MSGS` |
| 10 | contact book | `UI_CLAIM_CONTACTS` |
| 11 | zone browser | `UI_CLAIM_ZONES` |
| 12 | P2 long in FM | `UI_CLAIM_APRS_ENTER` |
| 13 | tools menu open (list or mode submenu) | `UI_CLAIM_TOOLS` |
| 14 | MONI, P2 long, macro latch | `UI_CLAIM_MACRO` |
| 15 | P1 long on a main screen | `UI_CLAIM_TOOLS_OPEN` |
| 16 | channel list and editor | `UI_CLAIM_CHANNELS` |
| 17 | the screen's own case in the switch | `UI_CLAIM_SCREEN` |

So the matrix, in the words the operator uses ("who answers this key"):

| Screen | P2 short | P2 long | MONI | P1 long | everything else |
|---|---|---|---|---|---|
| MAIN_VFO / MAIN_MEM, FM | popup | APRS | macro grid | tools menu | the screen |
| MAIN_VFO / MAIN_MEM, other modes | popup | macro grid | macro grid | tools menu | the screen |
| Tools menu, mode submenu | the menu | the menu | the menu | the menu | the menu |
| MAIN_VFO_INPUT | macro grid | macro grid | macro grid | the screen | the screen |
| MENU_TOP, MENU_INFO, MENU_ABOUT, MENU_SETTINGS and every page | macro grid | macro grid | macro grid | the screen | the screen |
| Channel list, channel editor, delete confirmation | the list | the list | the list | the list | the list |
| Contacts list, editor, confirmation | the book | the book | the book | the book | the book |
| Zone browser (both lists) | the browser | the browser | the browser | the browser | the browser |
| Messages: inbox, sent, view, compose, status | the module | the module | the module | the module | the module |
| APRS: stations, detail, inbox, view, editor, setup | the module | the module | the module | the module | the module |
| Packet modem: setup, counters | the module | the module | the module | the module | the module |
| Analyser | its own popup | its own popup (latched) | the analyser | the analyser | the analyser |
| Any screen, popup open | the popup | the popup | the popup | the popup | the popup |
| Any screen, keypad locked | — red held unlocks — | | | | nothing else answers |

Three things are worth reading off that table, because they are the ones that
bit the operator:

* **A screen that owns the keypad wins every key, MONI included.** The macro
  grid is a shortcut of the main screens, not of a list; that is why MONI in the
  contact book does not paint a grid over it.
* **P2 long is the only binding that depends on the mode** (APRS in FM, the grid
  everywhere else) and it is checked after the screens, so it can never steal a
  key from one of them.
* **Red is the only way out of every screen**, from any of its sub-screens, and
  it leaves the transmitter the way it found it: armed only on the two main
  screens, and only with no overlay on top. `_ui_syncTxDisable()` is the one
  place that decides it (it used to be seven copies, one of which re-armed the
  PTT when the inbox went back to the menu).

## The screens the audit walks

`tests/unit/ui_menu_audit.cpp` is built twice (with APRS and the packet modem,
and without them) and covers, for every compiled mode:

| Group | Screens |
|---|---|
| Main | VFO, frequency input, MEM |
| Main menu | top, Banks, Channels, Zones, Contacts, Messages, Settings, Info, About |
| Settings | Display, Time & Date (+ its digit entry), Radio, Audio, M17, TETRA, FM, Accessibility, Default Settings, and one generated page per mode that has `UI_MODE_PAGE` (DMR, NXDN, dPMR, P25, YSF, D-STAR, AM, WFM) |
| Tools | tools menu and its mode submenu, channel list, channel editor, delete confirmation, contacts list/editor/confirmation, zone list, zone entries, inbox, sent, message view, composer, delivery status, analyser, packet setup, packet counters, APRS stations/detail/inbox/view/reply editor/setup |
| Overlays | quick menu (one table per mode plus the analyser's), macro grid, low battery |

The eleven modes (FM, M17, TETRA, DMR, NXDN, dPMR, P25, YSF, D-STAR, AM, WFM)
are all in the host build: `UI_TEST_M17`, `UI_TEST_TETRA` and `UI_TEST_DMR` turn
their screens on without their engines, the same way `UI_TEST_NXDN` already did,
and `platform/targets/linux/emulator/ui_mode_status.c` answers the status getters
of the three that have one (and the packet service) with "nothing decoded".

Not reachable from any row, and therefore not walked: `MENU_BACKUP_RESTORE`,
`MENU_BACKUP`, `MENU_RESTORE` and the two `MODE_VFO`/`MODE_MEM` entries. They are
upstream leftovers: no entry of `menu_items[]` leads to them and `backup.c` needs
the SPI flash of the MD-3x0 family. `MENU_BACKUP_RESTORE` used to walk itself
with `settings_num` (13) instead of `backup_restore_num` (2), which would have
run the cursor past the end of a two-row list; it uses the right count now.

The same five are declared in the simulator tour's `UNREACHABLE` list
(`tools/simulator/tour.py`) and `tests/unit/sim_coverage.cpp` proves they are
still unreachable: it walks the UI with every key and fails if one of them
answers. What the simulator's tour adds to this audit is the *drawn* interface:
one PNG and one text dump per screen, the rows a screen really printed, the
values that change, and a hard check that the list of screens it covers is the
one the firmware has (docs/SIMULATOR.md, "The coverage contract").

## What the walk found

One line per symptom, with the fix and the test that holds it. "Correct" means
the walk exercises it and found nothing to change.

| Symptom | Cause | Fix | Test |
|---|---|---|---|
| Red on the main screen of the emulator crashes (`fseek(NULL)`) | `cps_io_libc` read the codeplug header without checking that `cps_init()` had opened it | `_readHeader()` returns -1 with no file | walk, VFO row, red |
| After leaving a menu the PTT does not key until another key is pressed | only the `MAIN_VFO`/`MAIN_MEM` cases clear `txDisable`, and a key that *returns* to them never runs its case | one `_ui_syncTxDisable()` for the whole dispatch | interactions |
| The radio can be keyed from the main menu after visiting the inbox | the module cleared `txDisable` when its screen was left, and "left" meant "back to the menu" | the same single rule: only the main screens key, and no overlay on top | interactions |
| P1 long opens the channel list and the PA stays armed until the next key | that branch returned before the block that disables TX | it disables TX itself | walk, channel list |
| 6.25 kHz in the FM menu crashes / speaks garbage | `vp_announceBandwidth()` indexed a two-entry table with `BW_6_25 = 2` | three entries and a bound | ASan run + FM BW row |
| The analyser keeps sweeping after a mode change made with the side key | it only closed when the *screen* changed | it also closes when the mode it opened in changes | interactions |
| The APRS reply editor cannot be typed into | it committed on `KEY_RIGHT` and backspaced on `KEY_LEFT`, which this keypad does not have | P1 adds, P2 deletes, long P1 backspaces; the hints say so | APRS reply test |
| Two rows of the APRS setup do nothing (Comment, Path) | they are strings and the page has no editor for them | they are skipped by the cursor and marked `(fixed)` | APRS setup test |
| The 1750 Hz row is a "setting" that changes nothing persistent | it toggled the momentary `tone_enabled` flag from a toggle row | it is an action now (green starts the burst), value column shows ON/OFF | rows test |
| Locking the keypad was a one-way door | the lock blocks the key that opens the menu, so it could not be undone | red held down unlocks | rows test (LOCK) |
| DMR had no page in Settings | its registry entry had no `UI_MODE_PAGE` and `UI_MODE_PAGES` did not list it | generated page, same rows as its popup | walk + registry test |
| TETRA/DMR/M17 screens were only ever built for the radio | their tables were guarded by the engine's `CONFIG_*` | the guards follow the UI registry (`UI_MODE_*`), with `UI_TEST_*` for the host | the whole audit runs on the eleven modes |
| FM settings page carried a duplicated green/red pair | merge residue | removed | settings walk |
| Rows of a mode with no transmitter could move the power | nothing checked the rows against `UI_MODE_TX` | table check in the audit | `[tx]` test |
| Every editable row writes `settings_t` and survives a save/read | — | correct | `[rows]` test (NVM round trip) |
| Quick menu closes on PTT, search stops on any key, analyser gives the receiver back | — | correct | `[interaction]` test |
| Two laps over the eleven modes leave the menus answering | — | correct (also under ASan/UBSan) | `[modes]` test |
