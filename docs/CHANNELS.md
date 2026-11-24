# Channels on the radio

In-radio channel bank and editor: channels can be created, edited, deleted and
tuned without a CPS, and the selection survives a power cycle.

- `openrtx/include/core/channel_store.h`, `openrtx/src/core/channel_store.c` —
  the bank, its record format and its writes.
- `platform/drivers/NVM/channel_store_c62.c` — the C62 flash backend.
- `openrtx/include/ui/ui_channels.h`, `openrtx/src/ui/default/ui_channels.c` —
  the list and the editor screens.
- `tests/unit/channel_store.cpp` — host test, including simulated power cuts.

## Where it lives

The bank uses the OEM channel bank of the SPI NOR flash, `0x3C0000`-`0x3FFFFF`
(256 KiB), declared in `platform/targets/c62/c62.dts` as
`channels_partition`. The factory memories are not preserved on purpose:
returning to the stock firmware means reflashing the whole flash, so the OEM
codeplug does not have to survive, and reusing its region keeps the rest of the
flash map untouched (in particular the 640 KiB OpenRTX storage partition that
holds settings, the VFO and the TETRA SDS history).

Only the first 16 KiB of the region are used: two 8 KiB slots. The remaining
240 KiB are free for a larger bank (the current record of 32 bytes per channel
caps the bank at 128 channels; a second pair of slots would allow 256).

Each slot is two 4 KiB erase sectors, the erase unit of the chip
(`erase-block-size = <4096>` in the devicetree).

## Record format

Little endian, packed, 32 byte header followed by `count` 32 byte records.

```c
struct chstore_hdr {          /* 32 B, at offset 0 of a slot */
    uint32_t magic;           /* 0x53545243, "CRTS"           */
    uint16_t version;         /* 1                            */
    uint16_t count;           /* channels, 0..128             */
    uint32_t sequence;        /* +1 on every write            */
    uint16_t last_used;       /* index or 0xFFFF              */
    uint16_t hcrc;            /* CRC16 of this header         */
    uint16_t pcrc;            /* CRC16 of the records         */
    uint8_t  reserved[14];    /* zero                         */
};

struct chstore_channel {      /* 32 B */
    char     name[16];        /* NUL terminated, 15 chars max */
    uint32_t rx_freq;         /* Hz                           */
    uint32_t tx_freq;         /* Hz                           */
    uint16_t power;           /* mW, 1000 / 2500 / 5000       */
    uint8_t  mode;            /* enum opmode                  */
    uint8_t  bandwidth;       /* enum bandwidth               */
    uint8_t  rx_tone_en : 1, rx_tone : 7;   /* CTCSS index      */
    uint8_t  tx_tone_en : 1, tx_tone : 7;
    uint8_t  dmr_cc : 4, dmr_ts : 4;        /* colour code, 0=TS1 */
    uint8_t  flags;           /* bit0 RX only, bit1 simplex   */
};
```

`hcrc` covers the header with `hcrc` and `pcrc` zeroed, `pcrc` covers
`count * 32` bytes of records, so the unused part of a sector does not matter.
A full bank (128 records) is exactly 4096 bytes and fits in one sector; the
second sector of a slot is spare.

`flags & SIMPLEX` means the TX frequency follows the RX one: the editor sets it
when the two are equal and clears it as soon as TX is edited on its own.

## Writing: ping-pong, atomic

The bank is kept in RAM (4 KiB) and written as a whole:

1. erase the slot that does **not** hold the current bank,
2. write the header, then the records,
3. read the header back and compare magic, sequence and both CRCs,
4. only then does that slot become the current one.

Power lost at any point of 1-3 leaves the other slot intact and the sequence
numbers make the older slot win, so the previous bank comes back at the next
boot. If both slots fail to validate, the bank is empty: the radio boots on the
VFO and the user adds channels from the radio (the factory codeplug is never
imported). `tests/unit/channel_store.cpp` walks a power cut across a whole save
(erase included) and checks the invariant at every step.

Erasing and rewriting 8 KiB costs tens of milliseconds, so a channel *tune*
does not write immediately: the new `last_used` index is only flagged, and
`chstore_task()` (called from `state_task()`, 100 ms cadence) writes it after
`CHSTORE_LASTUSED_DELAY_MS` (2 s, deliberately off the 3 s debounce that the
settings and the VFO use, so the two erases do not land in the same pass).
Structural changes (add, edit, delete) are written at once because they are
explicit user actions. `state_terminate()` flushes a pending index where the
platform runs it: the C62 cuts power with the volume knob, so there the 2 s
debounce is the only chance to save it.

## API

```c
int  chstore_init(void);                 /* load; empty bank if blank        */
int  chstore_count(void);
int  chstore_get(uint16_t idx, chstore_channel_t *out);
int  chstore_set(uint16_t idx, const chstore_channel_t *ch);   /* write-through */
int  chstore_add(const chstore_channel_t *ch);   /* returns the new index    */
int  chstore_delete(uint16_t idx);               /* shifts the rest down     */
int  chstore_loadDefaults(void);                 /* empty bank, on flash too */
int  chstore_lastUsed(void);                     /* -1 if none               */
void chstore_setLastUsed(uint16_t idx);          /* RAM, debounced write     */
int  chstore_commit(void);                       /* flush if pending         */
void chstore_task(void);                         /* periodic flush           */
bool chstore_valid(const chstore_channel_t *ch); /* editable and storable?   */
bool chstore_freqValid(freq_t f);                /* 18-660 / 840-1300 MHz    */
int  chstore_toChannel(uint16_t idx, channel_t *out);
void chstore_fromChannel(const channel_t *in, chstore_channel_t *out);
```

The store is storage agnostic: it talks to a `struct chstorePort` (read, erase,
write, size, erase unit). The platform provides one through the weak
`chstore_platformPort()`; the weak default returns `NULL`, so every other
target (and the Linux emulator) reports an empty, read-only bank instead of
failing to build. The host test binds its own port with `chstore_bind()`.

## Frequency limits

`chstore_freqValid()` accepts 18-660 MHz and 840-1300 MHz, the coverage of the
BK4819, rather than the 137-174 / 400-480 MHz of `hwInfo_t`. This is the
"unlocked" range: nothing in the front end filters outside the amateur bands,
but sensitivity and spurious response there are not characterised.

## Using the editor

P1 long press (or the hook in `ui.c`) opens the list from the VFO or the MEM
screen.

| Key | List | Editor |
|-----|------|--------|
| UP / DOWN | move the selection | previous / next field |
| UP / DOWN (field being edited) | | previous / next value, long press moves a coarser digit |
| LEFT / RIGHT | | move inside the name or the frequency |
| green (ENTER) | tune the channel and close | enter / leave the field |
| P1 (F1) | new channel, copied from the selection or the VFO | |
| P2 (F2) | edit the selected channel; long press deletes | save |
| red (ESC) | close | cancel, or leave the field |

Tuning copies the record into `state.channel` and asks the main loop for a
synchronisation: `rtx_configure()` then applies frequency, mode, bandwidth,
power and tones, and a mode change goes through the usual OpMode switch,
exactly as when the mode is changed from the VFO.

## Integration points

`ui_channels.c` is modal and self contained; the main UI only needs the hooks
below. Line numbers are those of commit `512ae76`; every block is preceded by a
comment that names the channel UI, so `git log -S ui_channels_isOpen` finds them
again after a rebase.

| File | Site | Lines | What it does |
|------|------|-------|--------------|
| `openrtx/src/ui/default/ui.c` | includes | 59 | `#include "ui/ui_channels.h"` |
| `openrtx/src/ui/default/ui.c` | `ui_updateFSM()`, before `int priorUIScreen = state.ui_screen;` | 1516-1544 | opens the list on a P1 long press from `MAIN_VFO`/`MAIN_MEM`; while open, every key goes to `ui_channels_handleEvent()`, `state.txDisable` is held true and the FSM returns |
| `openrtx/src/ui/default/ui.c` | `ui_updateGUI()`, after the layout is ready | 2759-2764 | `ui_channels_draw()` and `return true` while open |
| `openrtx/src/core/state.c` | includes | 18 | `#include "core/channel_store.h"` |
| `openrtx/src/core/state.c` | `state_init()`, after the VFO is loaded | 81-93 | `chstore_init()`, then the last used channel into `state.channel` |
| `openrtx/src/core/state.c` | `state_terminate()` | 135-136 | `chstore_commit()` |
| `openrtx/src/core/state.c` | `state_task()`, after the TETRA SDS task | 199-200 | `chstore_task()` |
| `platform/targets/c62/c62.dts` | `flash0` partitions | 285-294 | `factory_chan` becomes `channels_partition` (same address, no `read-only`) |
| `CMakeLists.txt` | `target_sources(app)` | 121, 139 | `channel_store.c`, `ui_channels.c` |
| `platform/targets/c62/CMakeLists.txt` | `target_sources(app)` | 26 | `channel_store_c62.c` |
| `meson.build` | `openrtx_src`, `ui_src_default`, tests | 59, 83, 1270, 1324 | host build and `channel_store_test` |

### Merging with the UI branch (`c62/ui-ux`)

The merge was rehearsed at the time of writing: `c62/channels` and `c62/ui-ux`
merge without conflicts (both touch `state.c` and `ui.c`, in different hunks),
and the merged `state.c` and `ui.c` compile.

- `c62/ui-ux` fixes the C62 keypad matrix (`platform/drivers/keyboard/keyboard_c62.c`):
  on `c62/all` without it, ENTER/ESC/UP/DOWN are misreported, so the editor can
  only be used once that fix is in. Its key map keeps P1 = `KEY_F1` and
  P2 = `KEY_F2`, which is what the editor assumes, and ENTER = the MENU key,
  ESC = the A/B key, matching the green/red hint labels.
- `c62/ui-ux` guards the settings/VFO write-back in `state_task()` with
  `NVMEM_DEBOUNCED_WRITE`. `chstore_task()` does not depend on it; the two
  debounces are 2 s and 3 s so their erases do not land in the same pass.
- If the UI branch takes P1 long press for something else, only the first hook
  block changes: call `ui_channels_open()` wherever the list should be reachable
  from (a menu entry works as well, the module does not care how it is opened).
- `ui_channels_drawHints()` is the only drawing function the shared hint bar has
  to replace; it receives the four labels (green, P1, P2, red). **Done on the
  merged tree**: the body is now `ui_hints_draw()`, and `CH_HINTS_H` is
  `UI_HINTS_H` (11 px) so both bars are the same height. The quick menu is
  gated on `!ui_channels_isOpen()` because P2 is EDIT here.

## Limitations

- CTCSS only. The RTX tone path takes an index into `ctcss_tone[]`; DCS has no
  decoder/encoder in this fork, so the editor does not offer it.
- DMR colour code and timeslot are read only in the editor: there is no DMR
  transmitter to take them from, and they are copied from the channel the new
  one was created from.
- The list has no banks, scanning, or contact/group lists: one flat bank of 128
  channels.
- 15 characters of name (the 16th byte is the terminator).
