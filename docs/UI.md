# User interface

How the C62's UI is laid out, which key does what, and what the contextual
quick menu offers in each operating mode. The reasoning behind each item and the
comparison with the stock firmware's menus are in `reports/ui_menus_spec.md`,
outside this repository.

The C62 has a 160×128 RGB565 panel and a 20-key matrix plus a side key. There
is no knob and no left/right key: `KEY_LEFT`, `KEY_RIGHT`, `KNOB_LEFT` and
`KNOB_RIGHT` never arrive from `keyboard_c62.c`. Every screen that used them
now uses P1/P2 or UP/DOWN instead; that was the root cause of the channel
editor's caret and of the quick menu's value editing being unreachable.

## Keys

`keyboard_c62.c` scans the matrix as index `2 + row*5 + col` (the stock
numbering) and maps it to `KEY_*` through `keyMap[]`. The two programmable keys
sit in the matrix's second column, above the digits:

| Physical key | Matrix | `KEY_*`     | Meaning in this UI                     |
|--------------|--------|-------------|----------------------------------------|
| Green MENU   | r0 c0  | `KEY_ENTER` | OK / open / cycle / save (per screen)  |
| P1           | r0 c1  | `KEY_F1`    | short: replay the last voice prompt on the main screens; **value −** in the quick menu; **caret left** while editing a field; previous field in the contact editor; previous folder in the message lists; long: in-radio channel list |
| P2           | r1 c1  | `KEY_F2`    | short: contextual quick menu on the main screens; **value +** in the quick menu; **caret right** while editing a field; next field in the contact editor; save in the channel editor; reply / cancel in the message lists; long: quick menu latch, macro grid elsewhere |
| Red A/B      | r3 c0  | `KEY_ESC`   | Back / cancel                          |
| UP / DOWN    | r1 c0 / r2 c0 | `KEY_UP` / `KEY_DOWN` | move the cursor (or the value, in the editors' field mode) |
| Side key     | —      | `KEY_MONI`  | MONI; short or long opens the macro grid |
| Digits       | —      | `KEY_0`..`KEY_9` | frequency entry; 1..9 jump to a quick menu row; multi-tap text in the composer (long press inserts the digit) |

The side key is a second PTT, not a menu key: PTT always closes the quick menu
and keys the radio. It also re-arms TX when a screen that had disabled it
(channel editor, contacts, messages) is left.

Per screen:

| Screen | Green | P1 | P2 | Red |
|---|---|---|---|---|
| VFO frequency input | Set | — | — | Cancel |
| Main VFO / MEM | — | replay prompt (long: channels) | quick menu (long: macro grid) | — (S-meter) |
| Main VFO / MEM, FM | — | replay prompt (long: channels) | quick menu (long: APRS screens) | — |
| Menu (top, banks, settings, backup, info, about) | OK | — | — | Back |
| CPS channel list (other targets) | Load | — | — | Back |
| Settings pages | Edit | — | — | Back |
| Default settings | Reset | — | — | Back |
| Quick menu (popup) | cycle / open | value − | value + (long: latch) | Close |
| Channel list | Tune | New | Edit (long: delete) | Exit |
| Channel editor, field mode | Edit | — | Save | Cancel |
| Channel editor, editing a field | OK | caret left | caret right | Back |
| Contacts list | Edit | New | Edit (long: delete) | Back |
| Contacts editor | Save | caret left / mode − | caret right / mode + | Cancel |
| Messages inbox | Open | swap to Sent | Reply | Back |
| Messages sent | Status | swap to Inbox | Cancel | Back |
| Message view | Reply | — | — | Back |
| Compose, row mode | OK | target − | target + | Back |
| Compose, typing | Done | Delete | Space | Exit typing |
| Delivery status | Back | — | Cancel | Back |

## Layout

`_ui_calculateLayout()` fixes these sizes for the 128 px height branch:

| Element | Size | Font |
|---|---|---|
| Top bar | 16 px | 8 pt |
| Menu and list rows | **12 px** | **6 pt** |
| Bottom S-meter | 23 px | 8 pt |
| Key hints row | **11 px** | **5 pt** |
| Frequency input | 12 pt | 12 pt |
| Quick menu title bar | **14 px** | **8 pt** |
| Quick menu rows | **10 px** | **5 pt** |
| Contact and message rows | 10–12 px | 5–6 pt |

Ten menu rows fit above the hints row (128 − 16 − 11 = 101 px), which is more
than any list this radio has today. Menus keep the 8 pt top bar so the screen
title stays readable next to the 6 pt entries.

Lists reserve the hints row: `_ui_drawMenuList()` and
`_ui_drawMenuValueList()` stop at `CONFIG_SCREEN_HEIGHT - UI_HINTS_H`, and the
scroll window is computed from that limit, so a long list never draws over the
hints.

The hints row is drawn by `ui_hints.c` for every screen that returns a hint
table, at the end of `ui_updateGUI()` (`ui_hints_draw_for_screen()`). Each entry
is `KEY:action` in the 5 pt font, the key in yellow and the action in white;
four short hints fit in the 152 px of usable width. `P1`/`P2` are drawn with
their physical names, the green and red keys without a prefix, because that is
how the stock manual names them.

### Quick menu metrics

The popup uses the 5 pt font, the same as the hint row, and these numbers are
pinned by `tests/unit/ui_quickmenu.cpp`:

| Quantity | Value | Why |
|---|---|---|
| `QM_TOP_Y` | 16 px | below the top bar |
| `QM_HEADER_H` | 14 px | title (8 pt) and the frequency on one line, rule at 29 px |
| `QM_ROW_H` | **10 px** | 5 pt glyph box (5 px) plus 2 px above and 3 px below |
| `QM_ROW_BASELINE` | **8 px** | baseline measured from the row top: glyphs land at +3..+7, inside the 10 px rectangle |
| `QM_TEXT_X` | 2 px | label margin |
| `QM_VALUE_X` | 2 px | right margin of the values |
| Visible rows | **8** | (128 − 11 − (16 + 14)) / 10 = 8.7 → 8, the ninth would touch the hints |

The old popup used the 6 pt font on a baseline one pixel below the row top and
a 12 px rectangle starting one pixel above it: the glyphs sat five pixels
*above* the selection rectangle, so the black-on-white text fell on black and
only its bottom slice was visible. Both the baseline and the rectangle now come
from the same `qm_rowTop()`.

Values are right-aligned and measured: if a value does not fit between the
label and the right margin it is shortened from the left, where a frequency
keeps its least significant digits, and prefixed with `<` so a shortened value
never looks like a real one. At 5 pt (4 px per character) the widest row here
is `SEARCHING` at 36 px, so this only triggers with future longer values.

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
6. APRS screens, then messaging screens, then the contact book: each consumes
   everything while its screen is on.
7. P2 long on FM main screen: APRS. Then MONI / P2 long: macro grid.
8. Channels: P1 long opens, and the module consumes everything while open.
9. The screen switch (main screens, menus, settings).

Transitions, briefly:

| FSM | States | Enter | Leave |
|---|---|---|---|
| Quick menu | closed → open (latched / unlatched) | P2 short on `MAIN_VFO`/`MAIN_MEM`, unlocked, not editing | ESC, PTT, unlatched timeout (`QM_TIMEOUT_MS` = 15 s), `QM_GOTO` row |
| Channels | `CHS_CLOSED`/`LIST`/`EDIT`/`CONFIRM` | P1 long, or Menu → Channels | ESC on the list; green on the list tunes and closes |
| Contacts | `CTS_CLOSED`/`LIST`/`EDIT`/`CONFIRM` | Menu → Contacts | ESC on the list |
| Messages | inbox / view / sent / compose / status | Menu → Messages, quick menu `MSGS` row, or reply from a received message | ESC from the inbox (to the menu), from sent (to the inbox), from compose (inbox, draft kept) |

The quick menu no longer has an edit mode. A modal editor made sense on a
radio with arrow keys; here it meant that the only way to change an integer was
ENTER followed by UP/DOWN, and enumerations could not be reached at all. Now
P1/P2 change the value of the highlighted row, integers clamp at `min`/`max`,
enumerations and toggles wrap, read-only rows beep, and green is exactly P2
except on `QM_GOTO` rows, where it opens the screen. Green never closes the
popup.

## Channel list and editor

P1 long press on the main screens, or Menu → Channels, opens the in-radio
channel bank (`ui_channels.c`, `docs/CHANNELS.md`). The menu entry exists
because the CPS list it used to open is empty on this radio (no codeplug,
`cps_io_stub`). The module takes over the keypad and the screen until it
closes, holds `state.txDisable` true so nothing can transmit from it, and
handles the keys before the screen switch, so ENTER and ESC reach the list and
the editor untouched.

It draws its own header and rows (20 px list rows, 12 px editor fields) but not
its own hint bar: `ui_channels_drawHints()` builds a `ui_hints_t` from its four
labels and calls `ui_hints_draw()`, so the bar is identical to every other
screen and `CH_HINTS_H` is `UI_HINTS_H`.

While the list is open the quick menu does not open: P2 there is EDIT, and the
underlying screen is still `MAIN_VFO`/`MAIN_MEM`, so `qm_open()` is gated on
`!ui_channels_isOpen()`.

Inside a field, UP/DOWN change the value (long press moves a coarser digit) and
P1/P2 move the caret through a name or a frequency. The caret used to be driven
by LEFT/RIGHT, which this keypad does not have.

## Contacts

Menu → Contacts opens the in-radio book (`ui_contacts.c`, `core/contact_store.c`),
not the CPS contact list. The list shows callsign and preferred transport;
green or P2 edits, P1 adds, P2 long deletes after a confirmation, red goes back
to the menu.

Fields: `CALL` (10 characters, callsign or DMR id), `NAME` (16 characters) and
`MODE` (FM / M17 / DMR / TETRA, the preferred transport, used to pre-select a
recipient when composing). UP/DOWN change the character under the caret, P1/P2
move the caret or change the transport, green saves, red discards. A contact
with an empty callsign is never written.

The book lives in the OEM channel partition, 0x8000..0xC000 (two 8 KiB
ping-pong slots), next to the channel bank and the message store; see
`platform/drivers/NVM/user_store_c62.c`. Every save is CRC-checked and read
back, so a power cut costs the save and never the book.

## Messages

Menu → Messages (with an unread badge: `Messages !3`) or the `MSGS` row of the
quick menu opens the inbox. The screens are transport agnostic: APRS over FM is
wired today, DMR SMS and TETRA SDS are thin hooks (`msg_dmr_sms_*`,
`msg_tetra_sds_*` in `core/msg_service.h`) for their ports.

| Screen | Content | Keys |
|---|---|---|
| Inbox | `*` unread, peer, transport | UP/DOWN, green opens, P1 switches to Sent, P2 replies, red back |
| View | header peer + transport, text in 5 pt | green replies, red back |
| Sent | peer and delivery state | green opens the status page, P2 cancels a queued message, P1 switches to the inbox |
| Compose | rows `TP`, `TO`, `TXT`, `SEND` and a character counter | see below |
| Status | state word, `try n of 3`, text | P2 cancels, green or red back |

Compose: UP/DOWN move between rows. On `TP`, P1/P2 or green cycle through the
transports that are available for the current mode (APRS needs FM, DMR SMS
needs DMR, SDS needs TETRA); if none is available the row reads `NO LINK` and
sending is refused. On `TO`, P1/P2 walk the contact book and green starts
typing, so a callsign and its SSID can also be entered by hand. On `TXT`, green
starts multi-tap typing:

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

Green on `SEND` queues the message and shows the status page. The length limit
is the transport's (APRS 67, DMR SMS 160, SDS 140) and the counter in the
header shows `used/max`.

Delivery states are `QUEUED`, `TX`, `SENT`, `ACK`, `FAIL` and `CANC`; APRS
retries three times on its own schedule and the status page shows the attempt
count. Cancelling stops the retries for that message.

Everything is persistent: inbox (16), sent (8) and two drafts, in the same
partition at 0x10000..0x14000, written through the two-slot CRC container. The
draft is flushed 3 s after the last edit and when leaving the composer, so a
battery pull does not lose it. The inbox is mirrored from the APRS service
(peer plus message number is the deduplication key), so a message read before
a power cycle stays read.

## Contextual quick menu

P2 short press, on the main screens only, with the keypad unlocked and not
editing a frequency. `ui_quickmenu.c` owns the keypad while it is open: UP/DOWN
move the cursor, P1/P2 change the highlighted value, green cycles it or opens
its screen, ESC closes, digits 1..9 jump to a row, and PTT closes the popup and
keys the radio. The popup opens unlatched and closes after 15 s without keys;
P2 long latches it so it stays until ESC.

The popup covers the screen: header (mode title left, frequency or mode state
right) in the 8 pt font, a rule, **eight rows of 10 px in the 5 pt font** (the
cursor row is inverted), and the hints row. The geometry table above has the
exact numbers.

Rows marked *compile-time* appear only when the platform defines the feature
(`CONFIG_KEY_BACKLIGHT`, `CONFIG_RX_FINE_OFFSET`), so no row is ever a dead
option.

### FM (12 rows)

| Row | Type | Range / values |
|---|---|---|
| `SQL` | int | 0..15, step 1 |
| `PWR` | int | 1000..5000 mW, step 500, shown as `1W`..`5W` |
| `CTCSS` | enum | OFF / TX / RX / BOTH |
| `TONE` | enum | 0..49 CTCSS tones, shown in Hz |
| `BW` | enum | 12.5 / 25 / 6.25 kHz |
| `SHIFT` | enum | OFF / −600k / +600k / −5M / +5M |
| `1750` | toggle | 1750 Hz burst on TX |
| `BKL` | int | 5..100 %, step 5 (never 0: the radio has no orderly shutdown) |
| `KEYLIT` | toggle | keypad backlight, compile-time |
| `LOCK` | toggle | keypad lock |
| `MSGS` | goto | opens Messages, shows the unread count |
| `MODE` | enum | FM / M17 / TETRA / DMR (the compiled ones) |

### M17 (11 rows)

`DEST` (opens Settings → M17 → callsign), `CAN` 0..15, `CANRX` toggle, `SRC`
(read only), `META` (opens the meta text editor), `PWR`, `BKL`, `KEYLIT`,
`LOCK`, `MSGS`, `MODE`.

### TETRA, receive only (9 rows)

`AFC` toggle, `FINE` −500..+500 Hz in 10 Hz steps, `CELL` (MCC-MNC, read only),
`LA/CC`, `CALL` (talker > destination, group and encryption flags, read only),
`BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MODE`.

### DMR, receive only (9 rows)

`CC` (colour code), `TS` (timeslot), `SYNC` (sync state and EVM), all read only
from the demodulator, then `FINE`, `BKL`, `KEYLIT`, `LOCK`, `MSGS`, `MODE`.

A read-only row shows `---`, `SEARCHING` or `SRCH` while there is no data,
never a zero that would read as a real value. P1/P2 on such a row beeps and
changes nothing.

## Frequency entry

Eight digits, 10 Hz resolution (the BK4819's step): `446.00625`. The dot is
inserted after the third digit. `FREQ_DIGITS` is 8 and
`FREQ_DIGIT_RESOLUTION` is 10, both in `ui_default.h`.

In FM and M17 the entry asks for RX and then TX; ENTER on an empty TX field
copies RX. In a receive-only mode (`opmode_canTransmit()` false: TETRA, DMR)
there is a single frequency: the TX field is never shown and the eighth digit,
or ENTER, applies what was typed.

A frequency outside the tuning range is not silently dropped: the radio says so
and stays in the input screen so the digits can be corrected.

## Fine RX tuning

`settings.rxFineHz` is a manual offset of the receive frequency, ±500 Hz in
10 Hz steps, offered in the TETRA and DMR quick menus. It is applied at every
retune by `radio_C62.cpp` as `channel + rxOffset + settings.rxFineHz`, where
`rxOffset` is TETRA's automatic correction. Keeping the two apart is what makes
them coexist: the AFC loop only moves its own value, the user only moves the
setting, and neither can overwrite the other. The setting is persisted, so it
survives a reboot without being pushed back down at start-up.
