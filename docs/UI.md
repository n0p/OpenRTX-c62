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
| Main VFO / MEM | cursor (FM: step) | menu | replay prompt (long: channels) | quick menu (long: macro grid; FM: APRS) | — (S-meter) |
| Menu (top, banks, settings, backup, info, about) | previous / next | open | — | — | back |
| Settings pages | previous / next | edit | — | — | back (leaves edit first) |
| Settings page, editing | change value | confirm | — | — | leave edit |
| Default settings | — | Reset, then Reset again | — | — | back |
| Quick menu | previous / next row | cycle value / open screen | value − (long: coarse) | value + (long: latch) | close |
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
take theirs from `ui_layout.h`:

| Element | Size | Font |
|---|---|---|
| Top bar of the main screens | 16 px | 8 pt |
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
7. P2 long on the FM main screen: APRS. Then MONI / P2 long: macro grid.
8. Channels: P1 long opens, and the module consumes everything while open.
9. The screen switch (main screens, menus, settings).

Everything an overlay opens goes through `ui_open_screen(screen, item)` and
nothing else. That one function remembers the main screen to come back to,
clears a stale `edit_mode` (a leftover edit mode made the first key on the new
screen mean "leave edit" instead of "move"), calls the module's own `open()`,
selects the requested row — the quick menu's `DEST` and `META` used to land on
the first field of the settings page — and arms or disarms the transmitter the
way the destination screen needs. `MSGS` and `MSG` from the popup therefore
behave exactly like the same entries in the menu.

Transitions, briefly:

| FSM | States | Enter | Leave |
|---|---|---|---|
| Quick menu | closed → open (latched / unlatched) | P2 short on `MAIN_VFO`/`MAIN_MEM`, unlocked, not editing | ESC, PTT, unlatched timeout (`QM_TIMEOUT_MS` = 15 s), `QM_GOTO` row |
| Channels | `CHS_CLOSED`/`LIST`/`EDIT`/`CONFIRM` | P1 long, or Menu → Channels | ESC on the list; green on the list tunes and closes |
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

The editor holds any mode the firmware was built with — FM always, M17 with
`CONFIG_M17`, DMR with `CONFIG_DMR` (`CONFIG_C62_DMR_RX`), TETRA with
`CONFIG_TETRA` — because the list is built from the compile-time modes instead
of a fixed table that offered modes the build did not have. The record
(`chstore_channel_t`) carries the mode, the bandwidth, both tones, the DMR
colour code and timeslot; the editor offers them all: `DMR CC/TS` is editable,
UP/DOWN walks the colour code 0..15 and P1/P2 flip the timeslot. Inside a field,
UP/DOWN change the value (long press moves a coarser digit) and P1/P2 move the
caret through a name or a frequency.

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

## Contextual quick menu

P2 short press, on the main screens only, with the keypad unlocked and not
editing a frequency. `ui_quickmenu.c` owns the keypad while it is open: UP/DOWN
move the cursor, P1/P2 change the highlighted value (long press for the coarse
step of the rows that declare one), green cycles it or opens its screen, ESC
closes, digits 1..9 jump to a row, and PTT closes the popup and keys the radio.
The popup opens unlatched and closes after 15 s without keys; P2 long latches it
so it stays until ESC.

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

### M17 (11 rows)

`DEST` (opens Settings → M17 → callsign), `CAN` 0..15, `CANRX` toggle, `SRC`
(read only), `META` (opens the meta text editor), `PWR`, `BKL`, `KEYLIT`,
`LOCK`, `MSGS`, `MSG`.

### TETRA, receive only (10 rows)

`AFC` toggle, `FINE` −500..+500 Hz in 10 Hz steps (long press: ±100 Hz),
`CELL` (MCC-MNC, read only), `LA/CC`, `CALL` (talker > destination, group and
encryption flags, read only), `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG`.

### DMR (12–13 rows)

`CC` (colour code), `TS` (timeslot), `SYNC` (sync state and EVM), all read only
from the demodulator. Then the call destination — `CALL` (`GROUP` / `USER`) and
`DEST` (the talkgroup or the subscriber ID, 24 bits, step 1, long press ±100) —
then `FINE`, `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MSG`.

`DEST` writes `settings.dmr_dest_id` / `settings.dmr_dest_type` and asks for an
RTX resync; the UI never touches `rtxStatus_t`. The two rows only exist when
the settings record has them (`DMR_ID_MAX` from `core/datatypes.h`, the model
branch) and the build has DMR; until then the table builds without them.

A read-only row shows `---`, `SEARCHING` or `SRCH` while there is no data,
never a zero that would read as a real value. P1/P2 on such a row beeps and
changes nothing.

## Frequency entry

Eight digits, 10 Hz resolution (the BK4819's step): `446.00625`. The dot is
inserted after the third digit. `FREQ_DIGITS` is 8 and
`FREQ_DIGIT_RESOLUTION` is 10, both in `ui_default.h`.

In FM and M17 the entry asks for RX and then TX; ENTER on an empty TX field
copies RX. In a receive-only mode (`opmode_canTransmit()` false: TETRA, DMR)
there is a single frequency. A frequency outside the tuning range is not
silently dropped: the radio says so and stays in the input screen.

## Fine RX tuning

`settings.rxFineHz` is a manual offset of the receive frequency, ±500 Hz in
10 Hz steps, offered in the TETRA and DMR quick menus. It is applied at every
retune by `radio_C62.cpp` as `channel + rxOffset + settings.rxFineHz`, where
`rxOffset` is TETRA's automatic correction. Keeping the two apart is what makes
them coexist: the AFC loop only moves its own value, the user only moves the
setting, and neither can overwrite the other.
