# User interface

How the C62's UI is laid out, which key does what, and what the contextual
quick menu offers in each operating mode. The reasoning behind each item and the
comparison with the stock firmware's menus are in `reports/ui_menus_spec.md`,
outside this repository.

The C62 has a 160×128 RGB565 panel and a 20-key matrix plus a side key. There
is no knob: `KNOB_LEFT`/`KNOB_RIGHT` never arrive and every screen that handles
them also handles UP/DOWN.

## Keys

`keyboard_c62.c` scans the matrix as index `2 + row*5 + col` (the stock
numbering) and maps it to `KEY_*` through `keyMap[]`. The two programmable keys
sit in the matrix's second column, above the digits:

| Physical key | Matrix | `KEY_*`     | Meaning in this UI                     |
|--------------|--------|-------------|----------------------------------------|
| Green MENU   | r0 c0  | `KEY_ENTER` | OK / Edit / open / load                |
| P1           | r0 c1  | `KEY_F1`    | short: replay the last voice prompt; long: in-radio channel list. The hints row shows it as P1 |
| P2           | r1 c1  | `KEY_F2`    | short: contextual quick menu; long: the legacy 3×3 macro grid |
| Red A/B      | r3 c0  | `KEY_ESC`   | Back / cancel                          |
| UP / DOWN    | r1 c0 / r2 c0 | `KEY_UP` / `KEY_DOWN` | move the cursor, change the value while editing |
| Side key     | —      | `KEY_MONI`  | MONI; long press also opens the macro grid |
| Digits       | —      | `KEY_0`..`KEY_9` | frequency entry; 1..9 jump to a quick menu row |

The side key is a second PTT, not a menu key: PTT always closes the quick menu
and keys the radio.

## Layout

`_ui_calculateLayout()` fixes these sizes for the 128 px height branch:

| Element | Size | Font |
|---|---|---|
| Top bar | 16 px | 8 pt |
| Menu and list rows | **12 px** | **6 pt** |
| Bottom S-meter | 23 px | 8 pt |
| Key hints row | **11 px** | **5 pt** |
| Frequency input | 12 pt | 12 pt |

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

| Screen | Green | P1 | P2 | Red |
|---|---|---|---|---|
| VFO frequency input | Set | — | — | Cancel |
| Main VFO / MEM | — | — | — | — (S-meter) |
| Menu (top, banks, settings, backup, info, about) | OK | — | — | Back |
| Channel and contact lists | Load | — | — | Back |
| Settings pages (display, radio, audio, M17, TETRA, FM, accessibility, time) | Edit | — | — | Back |
| Default settings | Reset | — | — | Back |
| Quick menu (popup) | Edit | — | Close | Close |

## Channel list and editor

P1 long press on the main screens opens the in-radio channel bank
(`ui_channels.c`, `docs/CHANNELS.md`). The module takes over the keypad and the
screen until it closes, holds `state.txDisable` true so nothing can transmit
from it, and handles the keys before the screen switch, so ENTER and ESC reach
the list and the editor untouched.

It draws its own header and rows (20 px list rows, 12 px editor fields) but not
its own hint bar: `ui_channels_drawHints()` builds a `ui_hints_t` from its four
labels and calls `ui_hints_draw()`, so the bar is identical to every other
screen and `CH_HINTS_H` is `UI_HINTS_H`.

While the list is open the quick menu does not open: P2 there is EDIT, and the
underlying screen is still `MAIN_VFO`/`MAIN_MEM`, so `qm_open()` is gated on
`!ui_channels_isOpen()`.

## Contextual quick menu

P2 short press, on the main screens only, with the keypad unlocked and not
editing a frequency. `ui_quickmenu.c` owns the keypad while it is open: UP/DOWN
move or change the value, ENTER edits, cycles or opens, ESC or P2 close, digits
1..9 jump to a row, PTT closes it and keys the radio. A short press latches the
popup; an unlatched one closes after 10 s without keys.

The popup covers the screen: header (mode title left, frequency or mode state
right) in the 8 pt font, a rule, six rows of 12 px in the 6 pt font (the cursor
row is inverted), and the hints row. Six rows is the height available: 16 px
header + 14 px title bar + 6 × 12 px + 11 px hints = 113 px of 128.

Rows marked *compile-time* appear only when the platform defines the feature
(`CONFIG_KEY_BACKLIGHT`, `CONFIG_RX_FINE_OFFSET`), so no row is ever a dead
option.

### FM (11 rows)

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
| `MODE` | enum | FM / M17 / TETRA / DMR (the compiled ones) |

### M17 (10 rows)

`DEST` (opens Settings → M17 → callsign), `CAN` 0..15, `CANRX` toggle, `SRC`
(read only), `META` (opens the meta text editor), `PWR`, `BKL`, `KEYLIT`,
`LOCK`, `MODE`.

### TETRA, receive only (8 rows)

`AFC` toggle, `FINE` −500..+500 Hz in 10 Hz steps, `CELL` (MCC-MNC, read only),
`LA/CC`, `CALL` (talker > destination, group and encryption flags, read only),
`BKL`, `KEYLIT`, `LOCK`, `MODE`.

### DMR, receive only (8 rows)

`CC` (colour code), `TS` (timeslot), `SYNC` (sync state and EVM), all read only
from the demodulator, then `FINE`, `BKL`, `KEYLIT`, `LOCK`, `MODE`.

A read-only row shows `---`, `SEARCHING` or `SRCH` while there is no data,
never a zero that would read as a real value.

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
