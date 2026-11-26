# HACKING: how this firmware is put together and how to build it

Everything below is specific to the C62 fork. `AGENTS.md` has the rules and the
traps; this file is the map: what lives where, how it flows, and how to build,
test and flash. The numbered docs in `docs/` go deeper on each subsystem and are
linked from the section they belong to.

---

## 1. The project in one screen

```
openrtx/            the firmware, platform independent
  src/main.c          entry point: platform init, then openrtx_run()
  src/core/           state, threads, settings, stores, audio, PC protocol,
                      black box, scanner, VFO search, spectrum, messaging
  src/rtx/            the radio thread: rtx.cpp, one file per mode
  src/protocols/      the digital protocols (DMR, NXDN, P25, YSF, dPMR,
                      D-STAR, TETRA, APRS) and their demodulators/vocoders
  src/ui/default/     the interface: screens, menus, quick menu, layout
  include/            the headers, same tree
platform/
  targets/c62/        THIS radio: devicetree, board config, platform.cpp,
                      PC protocol transport, bench namespace, black box port,
                      backup port, airband TX, DSP bring-up
  drivers/            baseband (BK4819, BK1080), audio (codec), display (ST7735R),
                      keyboard, NVM (settings, channel bank, user stores)
  mcu/CSK6011B/       delays and the bare-metal bits
lib/                third-party and ported engines: DMR package and vocoders,
                    IMBE, D-STAR, libtetra (in the build workspace), minmea
tests/              unit tests, host harnesses, QEMU cost runs
tools/              cps (PC client), bench (measurements), simulator (tour)
docs/               one file per subsystem; this file is the index to them
scripts/            build helpers, host builds of reference engines
```

The rule from upstream still holds: `openrtx/` is hardware-agnostic where it
can be, `platform/` holds everything that touches this board. When you add
something, ask which side of that line it belongs to.

---

## 2. The threads, and why it matters

The firmware is a handful of threads and almost every "the radio froze" bug has
been one of them waiting for another.

| thread | stack | what it does |
|---|---:|---|
| main | 2048 | platform init, then the state machine that feeds the watchdog; owns the shutdown path |
| UI / state | 2048 | `ui_updateFSM()`, draws the screen, applies the VFO and settings to `state` |
| RTX | 9216 (C62) | `rtx_task()`: configures the radio, runs the current mode's `update()` at the audio block rate, keys and unkeys |
| codec worker | ~21 KiB | the audio engine: pulls PCM from the active path, feeds the DSP/codec |
| pcproto | 4096 | reads the serial port, answers the PC, runs the bench namespace |
| black box supervisor | 3072 | watches the heartbeats, writes the post-mortem, reboots |
| keyboard scan | — | matrix scan; feeds key events to the UI |

Rules that come out of this:

- **No unbounded wait.** `state_mutex`, the audio route table, the codec locks
  and the display's SPI transfer have all frozen the radio at some point. Use
  deadlines, `trylock`, and make the failure visible.
- **The RTX stack is sized by what runs inside it**, not by the selected mode:
  DMR TX encodes AMBE+2 there. See `docs/TX_SAFETY.md`.
- **The protocol answers always**, including while transmitting. Its state
  snapshot takes no lock for that reason (`docs/PC_PROTO.md`).

---

## 3. The data flow

Receive, for any digital mode:

```
BK4819 discriminator (48 kHz)  ->  mode's demodulator  ->  protocol layer
      |                                                        |
      +-> audio stream (DMA) --> codec --> speaker            +-> status / UI
                                                               +-> messaging
```

Transmit:

```
mode's TX path (RTX thread)
   -> frame builders + FEC + vocoder
   -> dibits / symbols -> modulator -> audio stream -> BK4819 deviation
   -> PA enable + APC (power) + LED
```

Audio does not go through the CPU as a sample stream in the digital modes: the
codec and the DSP move blocks by DMA, and the mode only sees them per block.
That is why buffers handed to the audio engine are **DMA** and cannot move to
PSRAM (`docs/MEMORY.md` classifies every buffer in the image).

The mode interface is `openrtx/include/rtx/OpMode.hpp`: `enable()`, `disable()`,
`update(rtxStatus_t*)`, `setRxStatus()`. Everything a mode does on the air goes
through `radio_enableTx()`/`radio_disableRtx()` in the driver, which is where the
keying gate, the air clock and the reflections are enforced.

---

## 4. Where to look for what

| You want to change... | Start at |
|---|---|
| a screen, a menu row, the quick menu | `openrtx/src/ui/default/ui.c`, `ui_menu.c`, `ui_quickmenu.c`, tables in `docs/UI.md` |
| how a key behaves on a screen | `ui.c` (`ui_updateFSM`), `ui_hints.c`, the screen's own module (`ui_channels.c`, `ui_tools.c`, ...) |
| which settings exist and what they mean | `openrtx/include/core/settings.h`, `docs/SETTINGS.md` |
| how settings and VFO reach the radio | `openrtx/src/core/threads.c` (the `sync_rtx` block), `rtx/rtx.cpp` |
| a channel, zone, contact or message | `core/channel_store.c`, `core/zones.c`, `core/contact_store.c`, `core/messaging.c`, `docs/CHANNELS.md` |
| a mode's receive chain | `openrtx/src/rtx/OpMode_<MODE>.cpp` + `protocols/<MODE>/` + `docs/RX_<MODE>.md` |
| a mode's transmit chain | same files, the TX half + `docs/TX_<MODE>.md` |
| the keying rules | `rtx/KeyGate.h`, `rtx/TxGuard.c`, `rtx/TxWatch.h`, `docs/TX_SAFETY.md` |
| the PC protocol and the CPS | `platform/targets/c62/pc_proto_c62.c`, `core/pc_proto.c`, `docs/PC_PROTO.md`, `tools/cps/` |
| the bench (tune/key/capture from the PC) | `platform/targets/c62/pc_proto_bench_c62.c`, `docs/BENCH.md`, `tools/bench/` |
| a hardware detail of this board | `platform/targets/c62/c62.dts`, `hwconfig.h`, `platform.c` |
| the BK4819 registers | `platform/drivers/baseband/BK4819.c` and its header |
| why the radio froze | `docs/BLACKBOX.md`, then read the record |
| memory limits | `docs/MEMORY.md` (classes and the arena) |

`docs/` is the reference: if you change behaviour, the file that describes it
changes in the same commit.

---

## 5. Build

### Host build (the fast loop)

```sh
meson setup build_linux
meson compile -C build_linux          # or: ninja -C build_linux -k 0
```

Builds the unit tests, the host harnesses and the emulator. Some ARM targets of
other radios fail on a host without their toolchain; `ninja -k 0` keeps going
and the tests are still built.

### Firmware image

Only in the container, never on the host:

```sh
cd /home/n0p/src/c62/build/c62-ws-mega
sudo docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/work openrtx-c62 c62-build \
  -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y -DCONFIG_C62_NXDN_RX=y -DCONFIG_C62_YSF=y \
  -DCONFIG_C62_P25=y -DCONFIG_C62_DPMR_RX=y -DCONFIG_C62_DSTAR=y -DCONFIG_DMR_TX=y \
  -DCONFIG_NXDN_TX=y -DCONFIG_DPMR_TX=y -DCONFIG_P25_TX=y -DCONFIG_YSF_TX=y \
  -DCONFIG_C62_KISS=y -DC62_PC_PROTO=ON -DC62_DEBUG_PROTO=ON -DC62_BENCH_PROTO=ON -DC62_AIR_TX=ON
```

Output: `OpenRTX/build_c62/zephyr/zephyr.bin` (the image) and
`.../zephyr/dsp_firmware.bin` (the DSP core's own firmware). The build prints the
usage of every region:

```
FLASH 813456 B / 1 MB (77,58%)   RAM 186624 B / 256 KB (71,19%)
ITCM 8468 B (51,68%)   DTCM 12640 B (77,15%)   PSRAM 658778 B (35,90%)
```

DTCM is the region that hurts first; nothing DMA can go to PSRAM.

**The workspace builds the last commit, not the working tree**:

```sh
git -C OpenRTX fetch <worktree> <branch> && git -C OpenRTX reset --hard FETCH_HEAD
```

so commit before you build, or you will test the previous version and believe it.

### Options

Each `-DCONFIG_*` switches a receive or transmit chain in; the CMake options
(`C62_PC_PROTO`, `C62_DEBUG_PROTO`, `C62_BENCH_PROTO`, `C62_AIR_TX`, `C62_KISS`,
`C62_MESSAGING`, `C62_SCAN`, `C62_VFO_SEARCH`, ...) switch subsystems. The
release image is all of them: there are no per-feature variants.

---

## 6. Test

```sh
meson test -C build_linux              # everything, ~8 minutes
meson test -C build_linux 'Simulator Menu Tour'      # just the UI
```

What runs:

- **Unit tests** (`tests/unit/`): gates, protocol layers, stores, settings,
  demodulators, the black box, the backup record, the UI state machines.
- **Host harnesses** (`tests/host/`): the DMR transmitter decoded by the
  reference receiver, the KISS modem in loopback, the PC protocol driven through
  a pty by the real Python clients.
- **QEMU cost runs** (`tests/qemu/`): instructions per frame of a mode, for the
  CPU budget.
- **The simulator tour** (`tools/simulator/tour.py`): every screen, walked with
  keys, with a PNG and a state dump each. It fails on a blank screen, a row that
  cannot be selected, a value that does not change and a key nobody claims.
- **The coverage contract** (`tests/unit/sim_coverage.cpp`): modes, menu tables
  and screens against what the tour declares. Add a mode or a row without
  declaring it in `tour.py` and this fails on purpose.

Run the simulator by hand:

```sh
meson compile -C build_linux openrtx_c62_sim
python3 tools/simulator/tour.py --out /tmp/tour
OPENRTX_SIM_HEADLESS=1 SDL_VIDEODRIVER=dummy build_linux/openrtx_c62_sim
```

`docs/SIMULATOR.md` has the control protocol (keys, screenshots, dumps).

What no host test covers is the radio itself: the PA, the tuner, the DSP and the
real signals. That is what the bench and the black box are for.

---

## 7. Flash and verify

1. Power off with the volume knob; hold the secondary PTT and power on (burn
   mode, blank screen).
2. ```sh
   cskburn -C venus -s /dev/ttyUSB0 -b 1500000 --no-reset --verify-all \
       0x000000 zephyr.bin 0x100000 dsp_firmware.bin
   ```
3. Power cycle. Check, in this order: the TX LED stays off, the PC protocol
   answers, `diag` reports the regions, and the mode the radio boots in is the
   one you expect (the VFO is restored from flash).

`tools/cps/cps.py status|diag|log|mem|reg` and `tools/bench/bench.py` are the
instruments. `docs/BLACKBOX.md` explains what to do when it freezes.

---

## 8. Recipes

**Add a mode.** `rtx/rtx.cpp` has the mode table: enum value, name, size, and
the filters for where it appears. Write `OpMode_<MODE>.cpp/.hpp`, the protocol
under `protocols/<MODE>/`, add a `docs/RX_<MODE>.md`, the settings page and the
quick-menu rows, then declare all of it in `tools/simulator/tour.py` — the
coverage test will tell you what you missed.

**Add a screen.** New module under `ui/default/`, a member of the `uiScreen`
enum, the key handler in `ui.c`, the hints in `ui_hints.c`, the row in whatever
menu opens it, and a tour phase with a capture and a dump.

**Add a setting.** The field in `settings.h` with its default, the load/store in
`settings_record.c` (and a version bump if the layout changes), the row in the
settings page or quick menu, the validation next to the setter, the CPS tag if
the PC has to see it, `docs/SETTINGS.md`, and the tour step.

**Add a protocol message.** The ID in `pc_proto.h` with its capability bit, the
handler in the table, the TLVs, the client in `tools/cps/pcproto.py`, the host
harness case, and `docs/PC_PROTO.md`.
