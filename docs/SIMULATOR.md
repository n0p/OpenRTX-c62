# The C62 simulator

The radio's own user interface, running on a PC, driven by scripts: a menu tour
for every screen, and a way to look at what the firmware thinks it is showing
without a radio on the desk.

`openrtx_c62_sim` is the host emulator of OpenRTX built with the C62's UI
configuration (all eleven modes, the keypad backlight, the fine RX tuning, APRS
and the packet modem page) plus three things that exist only on the host:

- a **control channel** (`OPENRTX_SIM_HEADLESS`), a line protocol on stdin or on
  a UNIX socket, one command per line and one answer per command;
- a **dump** of the current screen and of the radio as text or JSON, taken from
  the firmware's own structures;
- a **PNG** writer for screenshots (SDL2 can only save BMP).

It is a host target: no firmware image compiles any of it. The recorder call
sites in `core/graphics.c` and `ui/ui_layout.c` are compiled only when
`UI_CAPTURE` is defined, which only the simulator defines.

## Building

```sh
meson setup build_sim
meson compile -C build_sim openrtx_c62_sim
```

Needs SDL2, readline, zlib, codec2 and a C/C++ toolchain — the same host
dependencies the rest of the test build has.

## Running it

Headless, commands on stdin:

```sh
OPENRTX_SIM_HEADLESS=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    build_sim/openrtx_c62_sim
```

With a window (a person at the keyboard, the readline shell of the plain
emulator is replaced by the control channel, so say what you want):

```sh
OPENRTX_SIM_HEADLESS=1 build_sim/openrtx_c62_sim
```

On a socket, so a tool can connect and disconnect without restarting the
simulator:

```sh
OPENRTX_SIM_SOCKET=/tmp/c62.sock build_sim/openrtx_c62_sim
nc -U /tmp/c62.sock
```

Environment:

| Variable | Meaning |
|---|---|
| `OPENRTX_SIM_HEADLESS` | read commands from stdin (any value but `0`) |
| `OPENRTX_SIM_SOCKET` | create this UNIX socket and serve one client at a time |
| `OPENRTX_SIM_SHOTS` | directory for auto-numbered screenshots (`shot-0001.png`) |
| `OPENRTX_SIM_STORE` | file holding the channel bank, the contact book and the message store (default `.emulator_userstore.img`) |
| `XDG_STATE_HOME` | where the settings record lives (`$XDG_STATE_HOME/OpenRTX/state.bin`) |
| `SDL_VIDEODRIVER=dummy` | no window |

The last two matter for a test: the settings record and the user store are
*state*, and a run that reuses them starts from whatever the previous run left
behind. `tools/simulator/tour.py` gives itself a temporary directory for both,
creates it empty, and asserts on the first screen that the radio it got has no
channel, no contact and no channel name.

Without `OPENRTX_SIM_HEADLESS` and `OPENRTX_SIM_SOCKET` the emulator keeps the
readline shell it always had (`key`, `screenshot`, `sleep`, `show`, `quit`).

## The protocol

One command per line. Every answer is framed, because the firmware prints its
own driver log to the same stdout from other threads:

```
#begin
#| <payload line>
#| <payload line>
#end ok
```

`#end err` means the command was understood and failed; the payload says why.
When the simulator comes up it prints `#ready openrtx-simulator 160x128`. A
client reads until `#begin`, then until `#end`, and ignores everything else.

| Command | What it does |
|---|---|
| `key <NAME>` | press and release a key, then wait for the UI |
| `key <NAME> long` | hold it past the long-press threshold (700 ms) |
| `keys <NAME>...` | several presses in a row |
| `keycombo <NAME>...` | press them together (`keycombo MONI 5` cycles the mode) |
| `keydown <NAME>` / `keyup` | hold a key across commands |
| `hold <NAME> <ms>` | feed the key for N ms (what a long press really is) |
| `ptt on\|off\|toggle` | the PTT line |
| `volume <N\|+N\|-N>` | the analog volume pot, 0..255 |
| `knob left\|right` | `KNOB_LEFT` / `KNOB_RIGHT` events |
| `rssi <f>`, `vbat <f>`, `mic <f>`, `channel <f>` | the emulated front end |
| `inject aprs-station <CALL> <LAT> <LON> <COMMENT>` | one APRS position heard on the air |
| `inject aprs-message <FROM> <TEXT>` | one APRS message addressed to this station |
| `inject message <SRC> <TEXT>` | one message in the shared inbox |
| `screenshot [file]` | PNG of the screen; auto-numbered without a name |
| `dump [text\|json]` | the screen and the radio |
| `sync` | wait until the UI has consumed every key and drawn |
| `sleep <ms>` | wait |
| `probe` | frame counter, queued keys and events, last key event |
| `quit` | power the radio off |

Key names are the radio's: `GREEN` (`KEY_ENTER`), `RED` (`KEY_ESC`), `P1`
(`KEY_F1`), `P2` (`KEY_F2`), `SIDE` (`KEY_MONI`), `UP`, `DOWN`, `LEFT`,
`RIGHT`, `0`..`9`, `STAR`, `HASH`, `KNOB_LEFT`, `KNOB_RIGHT`. The `KEY_` prefix
is optional.

### Filling the radio without a receiver

A screen that only exists once something has been received -- a station, a
message -- cannot be reached from an empty store, and an empty store is what a
reproducible run starts from. `inject` puts a packet on the service's own
receive path (`core/aprs_service.c`, `aprs_sim_rxFrame()`, which is compiled
only in the simulator): our own packets are dropped, the station list is
updated, a message addressed to `aprs_ownCallsign()` lands in the mailbox and
is acknowledged. Nothing is written into the UI.

```sh
inject aprs-station EA5ABC 38.87 -0.07 Bench station   # lat/lon: 38.87 or 3852.00N
inject aprs-message EA5XYZ hola desde el aire          # to N0CALL-0 by default
inject message 1234 hola                               # a message in the inbox
```

A short press is two entries in the keypad queue (press, release); a long press
is the same key *held* for 700 ms, which is why `hold` re-feeds the queue: the
firmware emits the long-press event itself, and no command can fake it.

### Writing a key script

```sh
OPENRTX_SIM_HEADLESS=1 SDL_VIDEODRIVER=dummy build_sim/openrtx_c62_sim <<'EOF'
key GREEN
key DOWN
key GREEN
dump
screenshot /tmp/menu-settings.png
quit
EOF
```

Each command waits for the UI before the next one, so the lines of a script do
not need `sleep`. In Python, `tools/simulator/simlib.py` wraps the protocol:

```python
from simlib import Simulator

with Simulator(shots="/tmp/shots") as sim:
    sim.key("GREEN")                     # open the menu
    print(sim.dump()["screen"]["name"])  # MENU_TOP
    sim.key("DOWN", "GREEN")              # Settings
    sim.shot("settings.png")
```

## Reading a dump

`dump` prints a text report; `dump json` prints the same snapshot as one JSON
object. Both come from the firmware's structures, never from the bitmap:

- **`lines`** are the strings the screen drew, in drawing order, with their
  position, font and colour. A line drawn in black is inside the inverted
  selection rectangle, i.e. it is the row under the cursor (`selected: true`).
  The APRS setup page marks its row with a hollow rectangle and yellow text
  instead, so look at `selection` there.
- **`header`** is the band of a list screen: the title and the right-hand
  status ("1/32", "dec off").
- **`selection`** is the rectangle the screen drew: its top, its height, and
  whether it is filled (selected) or hollow (being edited).
- **`quickmenu`** is the popup or the mode page: its table title, the cursor
  and every row with its label and its value, read from the table itself.
- **`input`** is the dispatch state: which block claimed the last key
  (`claim`, `uiKeyClaim`), whether the keypad is locked, whether a value is
  being edited, the frame counter, and the last key event (`keys`, `long`).
- **`radio`** is `state`: operating mode, RX/TX frequency, channel index and
  name, bandwidth, power, squelch, volume, RSSI, battery, step, bank and contact
  counts, `tx_disable`, `ptt`.
- **`settings`**, **`aprs`**, **`packet`** are the settings blocks the screens
  write.
- **`modules`** says which overlay owns the screen (channel list, contacts,
  zones, messages, tools).
- **`tools`** is the tools menu (P1 held down): whether it is open, which of its
  two screens is on (`modes` is true in the mode submenu), the cursor and every
  row with its right-hand value as the screen draws it.

```json
{"screen":{"id":5,"name":"MENU_TOP"},
 "input":{"claim":15,"claims":1,"edit":false,"locked":false,"frames":412,
          "menu":6,"last_key":{"keys":4096,"long":false}},
 "header":{"present":true,"title":"Menu","right":""},
 "selection":{"present":true,"top":87,"height":12,"filled":true},
 "lines":[{"y":11,"x":2,"font":2,"align":0,"rgb":"fab413",
           "selected":false,"text":"Menu"}, ...],
 "radio":{"mode":"FM","rx":145500000,"name":"","bandwidth":1,"power":1000,...},
 "modules":{"channels":false,"contacts":false,"zones":false,"messages":false},
 "tools":{"open":false}}
```

## The menu tour

```sh
python3 tools/simulator/tour.py --out /tmp/tour
```

or, from a configured build, `meson test -C build_sim "Simulator Menu Tour"`.

It walks 282 screens and writes a PNG plus a text dump per screen: the main
screen, the frequency entry, the top menu and every row of it, the eighteen
Settings pages (and the clock's digit entry), **each of the eleven modes one by
one** -- its main screen, its P2 popup with every row selectable and a value
edited, its own Settings page with all of its rows, and out again -- the FM
popup in depth (every editable value, LOCK and the red held down that unlocks
it), the DMR transmit screens (the two-step Rx/Tx entry, against TETRA's
single one), the tools menu of P1 held down (its five rows walked, the row
labels and values checked, the mode submenu with the TX/RX mark of all eleven
modes, opened, walked and applied), the channel bank (create, edit the
frequency, save, tune, open the stored record again), the contact book (create,
save, reopen), the messages (inbox, a received message, the view, the
composer, typing, SEND and the delivery page, the sent folder), the zone
browser (list, entries, tune), APRS (station list with P2 held, a station's
detail, the APRS inbox, a message, the reply editor, the setup page), the
packet modem (configuration and counters), the spectrum analyser, and the low
battery screen.

It fails -- with the step, the screen and the reason -- when

- a screen comes out blank (ink below 0.5 % or no text at all);
- a row of a list or of the popup cannot be selected;
- a value that should be editable does not change when its key is pressed;
- a key press is claimed by no block of the UI (`claim == 0`, the "deaf
  screen" defect);
- after leaving a screen, the main screen no longer answers the keys;
- the firmware draws a row, a menu or a mode that the tour does not declare,
  or a declared screen was not visited.

### The coverage it declares

The constants at the top of `tour.py` are the declaration: `MODES` (the eleven
modes with their quick menu rows, TX/RX and Settings page), `MODE_ORDER`,
`TOP_MENU`, `SETTINGS_PAGES`, `SCREENS` (every screen walked) and
`UNREACHABLE` (the screens the enum has and no key reaches, each with its
reason). The tour checks the declaration against what the firmware draws while
it runs, and `tests/unit/sim_coverage.cpp` checks it against what the firmware
registers. See "The coverage contract" below.

Both directions are enforced, so the list cannot rot in silence: a row added to
a table changes what the popup draws (`... has [...], the coverage declares
[...]`), and a screen walked without being declared is a failure too.

### Adding a step

Steps are methods of `Tour`. The helpers do the bookkeeping:

```python
def phase_spectrum(self):
    self.step_name = "spectrum"
    self.goto_mode("FM")
    self.home()                       # red until the main screen
    if self.qm_goto("ESPECTRO"):      # open the popup and green on that row
        d = self.capture("spectrum", screen="SPECTRUM")
        self.check("ESPECTRO" in self.text_of(d),
                   "the analyser is not on the screen")
        self.key("RED")
    self.home()
    self.assert_alive("after the analyser")
```

`capture(name, screen=..., need=[...])` takes the screenshot and the dump,
writes both, and checks the screen name, the ink and that every string in
`need` appears on the screen. `self.check()` records a failure and keeps going,
so one run reports everything; `self.finding()` records a firmware issue that
the tour does not fail on.

A new screen goes in two places: a step that walks it, and its name in
`SCREENS`. Nothing else -- if you forget, `Simulator Coverage` says which one.

## Known pending issues

Found by the tour and left alone because they are cosmetic or need a decision:

- **A settings page of a mode draws its hint bar twice.** `SETTINGS_MODE` draws
  the hints and `ui_updateGUI()` draws them again for the same screen; the
  picture is the same, the dump shows the row twice.
- **The APRS setup page marks the cursor differently from every other list**
  (hollow rectangle and yellow text instead of the inverted black-on-white
  row), so the shared "selected" rule of the dump does not apply there.
- **`invalidReason()` in the channel editor is coarse**: it distinguishes an
  empty name and a frequency out of range, and calls everything else "INVALID
  CHANNEL". A save refused for the colour code or the power says nothing.
- **`state.tuner_mode` is only written by the CPS path and by the scanner**;
  other screens that tune a channel do not set it. The bank does now.
- **Banks and Channels of the top menu draw nothing on the host**: they read
  the classic codeplug (`cps_readBankHeader`, `cps_readChannel`), which the
  emulator has no image of. The tour opens both and says they are empty; the
  in-radio bank of the tools menu is the one with content.
- **The TETRA Settings page is one row** (AFC) while the TETRA popup has
  thirteen. It is not a defect of the tour: `_ui_drawSettingsTetra` was written
  by hand and only ever had the one row.
- **A receive-only mode applies the typed frequency at the eighth digit** while
  a mode with a transmitter asks for Rx and then Tx and applies at the
  sixteenth; the tour asserts both, and it is the only place the difference is
  visible on a host with no PA.

## What this does not cover

The simulator runs the firmware's UI, state machine and stores; it has no
baseband, no audio and no RF. It cannot tell whether a transmission is correct,
whether the demodulator decodes, or whether a driver talks to the hardware —
`docs/BENCH.md` and the QEMU tests are for that. What it does cover is the part
that the operator touches: every screen, every key, every menu and every value,
without a radio on the desk.

That is also the limit of the DMR transmit coverage: the simulator is built
with `CONFIG_DMR_TX` (the image transmits DMR), so the mode is marked TX, the
PTT is not blocked and the frequency entry asks for the two frequencies, but
pressing the PTT on the host keys nothing — there is no transmitter to key. The
DMR bursts themselves are covered by `DMR TX Host Sequence`
(tests/host/dmr_tx_host), which runs the mode's own slot machine and decodes the
baseband with the reference receiver.
