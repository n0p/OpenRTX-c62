# C62 integration branch: upstream Retevis-c62 + TETRA

`c62/integration` joins the reviewed C62 support of upstream OpenRTX
(PR #566) with the TETRA receive work of the fork, so that one branch carries
FM, M17 and TETRA. Nothing was pushed: the branch lives only in the local
repository (`/home/n0p/src/c62/work/OpenRTX-integ`).

## Merge graph

```
bb2ad6f0 (merge base, OpenRTX master)
├── upstream/Retevis-c62  e7c42d2e  (43 commits: reviewed C62 base + fixes)
└── sp5wwp/c62/tetra      1218449b  (85 commits: C62 base + TETRA + M17)
        │
        ├── 77ac317e  merge upstream/Retevis-c62 into c62/integration
        └── c46cc057  keep the fork's diagnostics (C62_WIDE_RX, REG_43 sweep)
```

Direction: **TETRA first, upstream merged in**. The TETRA mode is the large
feature (protocols, rtx, UI, settings, speech playback) and merges cleanly on
the fork; the C62 hardware layer is where the two branches diverge, and
upstream's is the reviewed one, so the merge takes upstream's HAL as the base
and re-applies the handful of TETRA additions on top. The other direction
(upstream first) produces the same conflicts but would leave TETRA to be
re-applied by hand across the whole platform layer.

`sp5wwp/c62/dsp-firmware` was **not merged** (see below).

## DSP firmware (48 kHz): already a requirement, already in the tree

The fork's TETRA/M17 audio pipeline runs the CSK6 DSP at 48 kHz
(`DSP_SAMPLE_RATE 48000` in `platform/drivers/audio/csk6_codec.c`) and pins
the matching image: `C62_DSP_FIRMWARE_URL` points at
`OpenRTX/lsf-zephyr-sdk@2ac605527` (branch `feature/6012_nano_recorder_48k`),
sha256 `9579b617...`, flashed at 0x100000 into the `dsp_firmware` partition.
That is a **requirement**, not an improvement: the radio does not boot without
it and the image must match the audio code.

`sp5wwp/c62/dsp-firmware` is an older lineage (branched from v0.4.5, 27
commits, no libtetra/OpenACELP) whose only unique contribution, `e124b958`,
pins the **16 kHz** image from the same SDK at `ffc89631` and says so in its
own message ("switching to it is a change of URL and hash once the 16 kHz
audio path is tested"). Merging it would downgrade the sample rate and drop
the TETRA libraries, so `c62/integration` keeps the 48 kHz pin that tetra
already carries.

## Conflicts (28 files, all resolved by hand or by a documented side)

**Upstream version taken whole** (reviewed superset: cleaner API, fixes,
whitespace): `platform/drivers/GPIO/gpio_bk4819.c`,
`platform/drivers/audio/csk6_codec.{c,h}` (same 48 kHz codec + mic peak),
`platform/drivers/baseband/BK4819.{c,h}` (+ new `BK4819_priv.h`),
`platform/drivers/display/display_c62.c`, `platform/mcu/CSK6011B/drivers/delays.c`,
`platform/targets/c62/{Kconfig.board,Kconfig.defconfig,board.cmake,c62.dts,c62.yaml,c62_defconfig,c62_pinctrl.dtsi,controller.h,dsp.c,dsp.h,lsf.c}`,
`platform/targets/c62/services/{CMakeLists.txt,Kconfig}`.
The .dts keeps upstream's APC PWM at 100 kHz/prescaler 1, the RAM-less
`retevis,c62-st7735r` binding (SWD safe) and the keyboard backlight.

**Fork version taken whole**: `openrtx/include/core/threads.h` (4 KiB rtx
stack for libtetra), `west.yml` (libtetra + OpenACELP projects).

**Merged semantically**:

| File | Resolution |
|---|---|
| `platform/targets/c62/hwconfig.h` | upstream + `CONFIG_TETRA_AFC`, `CONFIG_TETRA_EQ_INIT`, `c62_setRxOffset()/c62_getRxOffset()`; upstream's `NVMEM_DEBOUNCED_WRITE` replaces `CONFIG_NVM_SAVE_ON_CHANGE` |
| `platform/mcu/CSK6011B/zephyr.conf` | upstream layout + `MAX_PTHREAD_COUNT=8`, `MAX_PTHREAD_MUTEX_COUNT=16` (UI, rtx, main, TETRA speech playback) |
| `platform/targets/c62/CMakeLists.txt` | fork's TETRA/OpenACELP/dump/sweep options + upstream's `calib_C62.c`, `nvmem_C62.c` (replacing `nvmem_c62.c`, `eeep.c`, `flash_zephyr.c`), SWD option |
| `platform/targets/c62/platform.c` | upstream's DSP-terminate/reboot, mic peak, `restore_dsp_overwrites()`; dump/remote start after the console restore; `C62_REMOTE` PTT override kept |
| `platform/drivers/audio/audio_c62.c` | upstream restructure (enum codec mode, speaker mux); dump init moved to `platform.c` |
| `platform/drivers/baseband/radio_C62.cpp` | upstream's calibration/BK4819 API + TETRA: flat 25 kHz RX path, RX only, `rxOffset`, RX IF DC filter, `c62_rx_reg43`, REG_43 sweep |
| `openrtx/src/core/state.{c,h}` | auto-merged into a tree with both persistence schemes; reset to upstream (debounced nvmem writes), `CONFIG_NVM_SAVE_ON_CHANGE` dropped |
| `openrtx/src/rtx/OpMode_M17.cpp`, `meson.build`, `.github/workflows/main.yml` | auto-merged; both sides' changes kept |

Deleted: `platform/drivers/NVM/nvmem_c62.c` (case-only twin of upstream's
`nvmem_C62.c`; upstream's also reads the factory calibration journal).

## Build (verified)

Workspace `/home/n0p/src/c62/build/c62-ws` (west, Zephyr ListenAI 3.4 fork,
modules already fetched) with `OpenRTX` checked out at `c62/integration`;
Docker image `openrtx-c62`, script `/home/n0p/src/c62/build/build_integ.sh`:

```
sudo docker run --rm --user $(id -u):$(id -g) -v $PWD:/work openrtx-c62
```

Result (log `/home/n0p/src/c62/build/integ_build.log`,
artifacts `OpenRTX/build_c62_integ/`):

```
zephyr.bin         395000 B  sha256 e0a52760b6cf0b2a77d0aeeb184be6f7e4bac857ea26d95d23e8602e8df7eb19
dsp_firmware.bin   466832 B  sha256 9579b617f048c646465118f6d0b9e1b765b4d7d811ff9c8649c5754fe45a8903

Memory region         Used Size  Region Size  %age Used
           FLASH:      395000 B         1 MB     37.67%
             RAM:      143144 B       256 KB     54.61%
            ITCM:        8468 B        16 KB     51.68%
            DTCM:          0 GB        16 KB      0.00%
         PSRAMAP:     1427640 B      1792 KB     77.80%
```

Options on in that build: `C62_TETRA=ON`, `C62_TETRA_SPEECH=ON`,
`C62_TETRA_IF_DC_FILTER=ON`, `C62_BASEBAND_DUMP=ON`, `C62_WIDE_RX=ON`,
`C62_REMOTE=OFF`, `C62_REG43_SWEEP=OFF`. A second build with
`-DC62_REMOTE=ON -DC62_REG43_SWEEP=ON` also compiles (407288 B, log
`integ_build_opt.log`), so the fork's diagnostics survived the merge.
Host unit tests (`meson test`, e.g. `tests/unit/C62_calib.cpp`,
`tests/platform/c62_tetra_host.c`) were not run.

## Modes after the merge

- **FM**: 12.5/25 kHz, CTCSS/DCS, TX power and deviation from the factory
  calibration journal (upstream), settings/VFO persisted by debounced writes.
- **M17**: RX and TX, codec2 through the DSP at 48 kHz, C62 TX phase invert,
  160 ms preamble, flat AF path; `C62_WIDE_RX` (default ON) keeps the fork's
  25 kHz / `0x7e28` M17 RX filter, `OFF` gives upstream's `0x7808`.
- **TETRA RX**: TMO downlink, libtetra demodulator/FEC/MAC at 48 kHz, clear
  speech through OpenACELP, frequency correction (AFC) with the fine RX offset,
  TETRA settings menu, diagnostics and remote-control counters.

Key 5 cycles FM → M17 → TETRA; TETRA has no TX path in `OpMode_TETRA`.

## What is missing for TETRA TX

- No modulator: pi/4-DQPSK burst builder, TX randomizer, interleaver and
  convolutional encoder (libtetra is a receiver: `tmo_rx`/`dmo_rx` only).
- `radio_enableTx()` returns early for `OPMODE_TETRA`; no TX slot timing,
  power ramping, or 25 kHz TX deviation calibration for the BK4819 path.
- No uplink MAC/LLC (random access, attach), so no registration or SDS TX.

## Lost or changed from the fork

- `nvmem_c62.c` and `CONFIG_NVM_SAVE_ON_CHANGE` / `state_nvmSaves`
  (upstream's debounced nvmem writes instead). `scripts/c62_remote.py`'s
  `sv=` field now always reports 0.
- The raw M17 register table (`m17Fields`/`fmFields`) in `radio_C62.cpp`:
  replaced by upstream's `BK4819_AUDIO_BYPASS` + explicit setters. Covered:
  REG_2B/31/47/48/4B/70/7D/7E bypass fields, TX deviation, RF filters, gains.
  TETRA-only additions re-applied explicitly (25 kHz + 7/7 filters, IF DC
  filter, RX offset).
- Frequency scan / VOX / interrupt helpers of the fork's BK4819 driver had no
  call sites outside the driver; upstream's API replaces them (camelCase).
- Nothing from upstream was dropped: `git diff upstream/Retevis-c62 HEAD`
  deletes no file.

## Risks

- The TETRA equalizer taps in `CONFIG_TETRA_EQ_INIT` were measured on the
  fork's radio code; the merged code reaches the same REG_43/REG_7E state, but
  has not been re-measured on hardware here.
- `C62_WIDE_RX` now also gates M17's REG_43 (the fork applied it unconditionally
  in M17 RX); default matches the fork.
- Settings records are **not compatible** with the fork's `nvmem_c62.c`
  (upstream adds a magic/CRC record header); after flashing this build, the
  fork's stored settings and VFO are ignored and defaults are loaded once.
