# El estado del modo: the mode's state on the main screen

Branch `c62/mode-status`, base `cb779871`. The rule this work implements, as the
owner wrote it:

> el estado del modo tiene que aparecer en la pantalla principal, no en un
> subsubmenu. en los menús están las configuraciones y las opciones.

A mode's quick menu table is drawn twice: as the popup over the main screen and
as that mode's page under Settings. So every read-only row in it was state
inside a menu — the operator opened a submenu to see whether the radio had
locked, and the Settings page offered rows that beeped and did nothing.

## 1. Inventory: what each mode knew and where it was shown

| Mode | State the radio holds | Where it was (before) |
|---|---|---|
| DMR | sync, colour code, timeslot, link-control quality, last burst type | rows `CC`, `TS`, `SYNC` of the DMR table |
| NXDN | sync, RAN, EVM, frames, last SACCH message (source > destination, type) | rows `SYNC`, `CALL`, `TYPE` |
| dPMR | "a call is being followed" (squelch opens on the frame grid + CCH CRC) | row `SYNC` |
| P25 | sync, NAC, DUID, source > talkgroup, IMBE frames clean/bad | rows `SYNC`, `NAC`, `TYPE`, `CALL` |
| YSF | "a carrier is tracked" (the frame layer publishes nothing else) | row `SYNC` |
| D-STAR | sync, MY/SFX > UR, access repeater, Viterbi-corrected bits, slow-data text | rows `SYNC`, `MY`, `UR`, `RPT`, `TXT` |
| TETRA | sync, MCC-MNC, colour code, LA, downlink/uplink, registration, network broadcast, call, SDS, demodulator phase error | rows `CELL`, `LA/CC`, `REG`, `NET`, `UL`, `CALL` |
| M17 | LSF source/destination/link/meta text (and the configured destination when there is no LSF) | row `SRC` (the *own* callsign) |
| airband AM | channel designator of the frequency in use, AGC, squelch, TX arm state | row `CH` |
| broadcast FM | stereo state, dBuV, chip SNR, CCIR channel | rows `ST`, `LVL`, `SNR`, `CH` |

`platform/targets/linux/emulator/ui_mode_status.c` is the host double that
answered "nothing decoded" so the screens could be built and walked; it is now
also the way the simulator injects a receiver's state (§5).

## 2. The layout decision, and why

One block, drawn by one function (`ui_main.c: _ui_drawModeState()`), filled by
one module (`openrtx/src/ui/default/ui_mode_state.c`, header
`openrtx/include/ui/ui_mode_state.h`). Same place in every digital mode:

| Line | Content | Examples |
|---|---|---|
| 1 (`layout.line1_pos`, 8 pt) | the call and the received quality | `2317>9 e4% f214`, `2317>9 LC98%`, `b2 ED5YAM` |
| 2 left (`layout.line2_pos`, 8 pt) | identifiers and sync state | `CC1 TS2`, `RAN12 VOX`, `NAC293 VOX`, `228-8889`, `EA5ABC>CQCQCQ`, `DMR searching`, `SRCH` |
| 2 right | the secondary state | `ACC G3`, `DUID3`, `VCALL`, `LA1 ACC G3` |

- **Line 2 is one status line, not two rows.** The first version put the note on
  `layout.line4_pos`; the screenshot showed it drawn over the frequency. The C62
  layout has no fourth row: the 16 pt digits of the frequency reach down to
  y=93 and `line4_pos` is y=97, so the note was inside the digits. The state is
  therefore two bands around the frequency: what the receiver is locked to on
  the left, what else it learnt on the right.
- **Both halves are measured before drawing.** `gfx_print()` does not clip: a
  string that is not measured first wraps onto the line below, over the
  frequency. The note takes its short form (`ACC G3` instead of `LA1 ACC G3`)
  when the long one does not fit beside the identifiers, and is dropped when
  neither does; the identifiers never are.
- **Nothing decoded is `SRCH`** (`T searching` in TETRA), and an identifier that
  is not known is `-`: a colour code of 0 is a fact and "not decoded yet" is
  not a zero.
- **The bottom row stays the S-meter** (and the carrier-search message), the
  frequency keeps the 16 pt row, and the fonts are the ones the screen already
  used (8 pt for the state lines, from the same `layout_t` every screen shares).
- **Consistent, not identical**: FM (bandwidth/tone), M17 (link set-up frame
  with symbols, source, destination, reflector, meta text), airband (AGC,
  squelch, TX arm state) and broadcast FM (stereo, dBuV, SNR) keep their own
  lines: their state *is* the tuner read-out, and there is no decoded frame to
  put in the block.
- **Dropped on purpose**: the TETRA network broadcast (neighbour count, cell
  load, network clock) does not fit beside the cell on a 160 px line and is a
  diagnostic, not something the operator works with. The D-STAR slow-data text
  is also stored in the message inbox and is drawn when it fits.

Quality figures are the ones each mode actually publishes: DMR's share of
embedded link controls that decoded (`LC n%`, per mille from the package's
stats), NXDN's EVM and frame count, P25's share of voice frames without an
uncorrectable repair, D-STAR's Viterbi-corrected channel bits of the last header
with its access repeater, TETRA's phase error / carrier offset / overruns /
resyncs on line 1 (`_ui_drawTetraLine1`, which also pages SDS and the uplink),
broadcast FM's dBuV and SNR.

## 3. What moved where, per mode

| Mode | Moved to the main screen | Removed from the table (and its page) |
|---|---|---|
| DMR | `CC1 TS2` (line 2), `src>dst` and `LC n%` (line 1), burst type (line 2 right) | `CC`, `TS`, `SYNC` |
| NXDN | `RAN12 VOX` (line 2), `src>dst` + `e n% f n` (line 1), `VCALL`/`TX_REL`/`DISC` (line 2 right) | `SYNC`, `CALL`, `TYPE` |
| dPMR | `dPMR RX` / `SRCH` (line 2) | `SYNC` |
| P25 | `NAC293 VOX` (line 2), `src>tg` + `ok n% f n` (line 1), `DUIDn` (line 2 right) | `SYNC`, `NAC`, `TYPE`, `CALL` |
| YSF | `YSF RX` / `SRCH` (line 2) | `SYNC` |
| D-STAR | `MY/SFX>UR` (line 2), `b n RPT` (line 1), slow-data text (line 2 right) | `SYNC`, `MY`, `UR`, `RPT`, `TXT` |
| TETRA | `MCC-MNC` + `LA n` / `CC n` and `ACC G n` / `REJ n` (line 2); the call screen keeps lines 1–4 while a call runs; uplink/SDS/demodulator figures stay on line 1 (radio) | `CELL`, `LA/CC`, `REG`, `NET`, `UL`, `CALL` |
| M17 | source, destination, link, reflector and meta text already on the main screen (LSF block); the own callsign is a setting under Settings → Identity | `SRC` |
| airband | channel designator next to the raster (line 2, `_ui_drawAirbandInfo`) | `CH` |
| broadcast FM | stereo state + designator (line 2), dBuV + SNR (line 1) | `ST`, `LVL`, `SNR`, `CH` |

Supporting change: DMR's link control now copies source and destination into
`struct dmrStatus` (`idsValid`, `group`, `src`, `dst`), so a DMR call can read
`2317>9` on the main screen — `dmr_proto` already kept them per slot.
`DmrDefs.c` (burst type names) is now linked by the host build too.

## 4. Commits (branch `c62/mode-status`, not pushed)

```
da3825c1 threads: drop the boot marker that broke the simulator build
cc86e9d5 ui: the mode's state is on the main screen, not in its menu
db130b85 tests: a read-only row may not come back into a mode's menu
b735f0e5 sim: inject a receiver's state so the main screen can be looked at
199c6170 tests: the tools list ends with the accessory row
9bb06c97 sim: the tour declares the accessory row; threads: guard the necklace call
```

The last two are pre-existing defects of the base commit, found by running the
suite and the tour (they are needed for the verification to be green): the
`Puerto` row of the tools menu broke `ui_tools_test` and `tour.py`, and
`threads.c` called `c62_portNecklace()`, defined only with `C62_PC_PROTO`
(default OFF), so every image without the programming protocol failed to link.

## 5. Verification

Host suite (all 80 test binaries of `build_linux`, minus `linux_inputStream_test`
which has not compiled since the `inputStream_*` rename): **0 failures**,
including `ui_menu_audit_test` and `ui_menu_audit_plain_test` with the two new
cases, `ui_modes_test`, `ui_quickmenu_test`, `Simulator Coverage`.

Firmware image, `-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`:

```
FLASH   594336 B / 1 MB   (56.7 %)
RAM     165368 B / 256 KB (63.1 %)
ITCM      8468 B / 16 KB  (51.7 %)
DTCM     12640 B / 16 KB  (77.2 %)
PSRAMAP 466044 B / 1792 KB (25.4 %)
```

Simulator menu tour: **314 screens, no blank screen, every row selectable,
every editable value changes** (`python3 tools/simulator/tour.py --out /tmp/tour2`).

### The state screenshots

`python3 tools/simulator/mode_state_shots.py --out /tmp/modestate-final`

It drives the simulator through its control channel (`docs/SIMULATOR.md`) — the
same protocol a person uses over a socket — injecting the state a locked
receiver holds, capturing each main screen and checking it:

```
OPENRTX_SIM_HEADLESS=1 SDL_VIDEODRIVER=dummy build_linux/openrtx_c62_sim
  inject identity 2143017                         # else DMR shows NO TX
  inject dmr 1 2 evm 980 call 2317 9
  inject nxdn 12 evm 40 frames 214 call 2317 9 1
  inject p25 0x293 talkgroup 2317 9 duid 3 frames 120 bad 1
  inject dstar EA5ABC CQCQCQ ED5YAM viterbi 2 text hola desde el aire
  inject tetra-cell 228 8889 1 430500000 0
  inject tetra-registration 1 3 0
  inject tetra-call 1 1 9 2317 166 1 0
  inject mode-state clear
  keycombo MONI 5      # the side key cycles the modes
  dump json            # every line the screen drew, with x, y, w, font, align
  screenshot <file>
```

The script's checks, per mode: the expected texts are on the screen; no line
runs off the 160 px panel (`x` + `w`, with the anchor rules of `gfx_print()`);
no two strings on one baseline overlap. The run printed
`every mode's main screen is ok`.

The pictures (160x128, from the simulator):

| File | Mode | What it shows |
|---|---|---|
| `main-DMR.png` | DMR | `CC1 TS2` · `2317>9 LC98%` |
| `main-DMR-nothing.png` | DMR | `DMR searching`, no fake colour code |
| `main-NXDN.png` | NXDN | `RAN12 VOX` `VCALL` · `2317>9 e4% f214` |
| `main-P25.png` | P25 | `NAC293 VOX` `DUID3` · `2317>9` |
| `main-DSTAR.png` | D-STAR | `EA5ABC>CQCQCQ` · `b2 ED5YAM` |
| `main-TETRA.png` | TETRA | `228-8889` `LA1` |
| `tetra-registration.png` | TETRA | `228-8889` `ACC G3` |
| `tetra-call.png` | TETRA | the call screen: talker 2317, group 9, cell, `Voice, call 166` |
| `main-dPMR.png`, `main-YSF.png` | dPMR, YSF | `SRCH` |
| `main-M17.png` | M17 | `M17 #ALL` (its own line; LSF block when a frame arrives) |
| `main-AM.png` | airband | `AM 25` · `AGC- sql6` |
| `main-WFM.png` | broadcast FM | `SRCH 144.97` |
| `main-FM.png` | FM | unchanged |

## 6. Where the state cannot be seen

- TETRA's network broadcast (neighbours, load, network time) and the exact
  uplink SSI on the host build: the first does not fit the line, the second is
  drawn by `_ui_drawTetraLine1`, which needs the radio's SDS service.
- YSF and dPMR publish no counters at all (their PHY has no status block yet),
  so their line carries sync only.
- The popup still shows the frequency (or `MCC-MNC` for TETRA) in its header;
  the popup is an overlay, not a page of settings.
