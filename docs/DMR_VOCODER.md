<!--
SPDX-FileCopyrightText: 2026 OpenRTX Contributors

SPDX-License-Identifier: GPL-3.0-or-later
-->

# DMR vocoder on the C62: AMBE+2 decode

The C62 can now decode DMR voice: `lib/dmr_vocoder` turns the 9-byte AMBE+2
frame of a burst into 160 samples of 8 kHz PCM (20 ms).  **Decode only** – the
encoder does not fit in the CSK6011B's Cortex-M33 – and the DMR receive mode
that will call it is not part of this branch yet: what lands here is the
vocoder, its integration in the firmware build, and the measurements that show
it fits.

Measured (details in the sections below): **751 394 instructions per frame**,
i.e. **3.0–4.0 ms per 20 ms frame (15–20 % of one M33 at 300 MHz)**, **+41 KiB
of flash** and **+52 KiB of static RAM** in the firmware image.

## API

```c
#include "dmr_vocoder.h"

/* One AMBE+2 voice frame -> 20 ms of audio.  Canonical order A(24)|B(23)|C(25). */
int  dmr_vocoder_decode(const uint8_t frame[DMR_VOCODER_FRAME_BYTES],
                        int16_t pcm[DMR_VOCODER_PCM_SAMPLES]);

/* New transmission, or loss of synchronisation. */
void dmr_vocoder_reset(void);
```

`dmr_vocoder_decode()` returns 0 on success, -1 for a NULL argument and -2 if the
internal arena is exhausted.  Frames must be decoded in order: the synthesis
window overlaps two frames, so the first frame after a reset fades in from
silence (same behaviour as the reference implementation).  Diagnostics:
`dmr_vocoder_last_errors()` (Golay corrections in the last frame),
`dmr_vocoder_state_bytes()`, `dmr_vocoder_arena_used()`.

### The backend is replaceable

`lib/dmr_vocoder` is a *backend*: the DMR mode only sees `dmr_vocoder.h`.  The
current implementation wraps the decoder half of OpenDMR (mbelib-neo).  An
optimised decoder – fixed-point FFT, DSP intrinsics, tables in flash – can
replace everything inside `lib/dmr_vocoder` without touching the DMR mode,
provided it keeps this contract:

1. the same header and the two functions, same return codes, no allocation per
   frame;
2. one static instance, no locks, `dmr_vocoder_reset()` callable between frames
   from the audio thread;
3. frames consumed in canonical, **de-interleaved** order A(24) | B(23) | C(25);
4. state kept between calls (first frame after reset fades in from silence);
5. no C++, no `__thread`, no libc heap, and flash/RAM in the same order of
   magnitude as the numbers below.

Power-on initialisation is the one thing a replacement should improve: building
the DCT tables and the FFT plan costs 4.22 M instructions (~14 ms at 300 MHz)
the first time a frame is decoded.  A backend with the tables in flash would
start decoding in microseconds.

## Integration

* `platform/targets/c62/Kconfig.board` (inside `menu "DMR"`) defines
  `C62_DMR_VOCODER`, default `n`;
* `platform/targets/c62/CMakeLists.txt` builds `lib/dmr_vocoder` and links it
  when the symbol is set, adding `CONFIG_DMR_VOCODER` for the application:

```sh
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_VOCODER=y
```

* `C62_DMR_VOCODER_FORCE_LINK=ON` additionally links the library when nothing
  calls it yet (`-Wl,-u,dmr_vocoder_decode`), which is how the flash/RAM delta
  below was measured.  It is a measurement aid, off by default;
* the FPU is already on for the C62 (`CONFIG_FPU=y` in
  `platform/mcu/CSK6011B/zephyr.conf`); Zephyr enables CP10/CP11 at boot, which
  the library needs because it is built with `-mfloat-abi=hard`.  The QEMU test
  harness in `lib/dmr_vocoder/tests/qemu` does the same by hand, and faults with
  `NOCP` without it;
* the vocoder is compiled with `-O2 -ffp-contract=off -fno-math-errno` instead of
  the firmware-wide `-Os`: it is the hot path of DMR RX, and FP contraction would
  make the results depend on the target's FMA support.

### What the DMR receive mode still has to do

The vocoder takes the *canonical* 72-bit frame.  A burst carries the voice as 36
dibits in the slots 12–47 and 108–143, interleaved; the mapping must be undone
before calling `dmr_vocoder_decode()` (36-entry table, see
`reports/opendmr_port.md` §6.1 and dsd-neo's `ambe_interleave.h`).  Per burst of
30 ms there are two voice frames in one slot and one in the next (2 bursts = 3
frames), so a one-frame jitter buffer keeps the 20 ms cadence.  Demodulation,
sync, CACH and the embedded LC are still missing: they are the next step, and
they are independent of this library.

## Size in the firmware

Build with `-DCONFIG_C62_DMR_VOCODER=y -DC62_DMR_VOCODER_FORCE_LINK=ON`,
compared with the same branch without the symbol:

| Region | Baseline | With vocoder | Delta |
|---|---|---|---|
| FLASH | 395 000 B (37.67 % of 1 MiB) | **436 444 B (41.62 %)** | **+41 444 B (+40.5 KiB)** |
| RAM | 143 144 B (54.61 % of 256 KiB) | **195 248 B (74.48 %)** | **+52 104 B (+50.9 KiB)** |
| `zephyr.bin` | 395 000 B | 436 444 B | +41 444 B |

Section by section (from `arm-none-eabi-size -A`): `text` +18 528 B, `rodata`
+22 912 B (the two tables: Golay syndromes 8 192 B, mbelib ECC 8 272 B, plus the
float constants), `bss` +52 097 B, `datas` +4 B.  The library archive holds
~52 KiB of objects; `--gc-sections` drops the ~10 KiB no caller reaches (tone
synthesis, dump helpers), so 41.4 KiB is what the DMR mode will really add.
RAM is at 74 % of SRAM with the vocoder in, which still leaves ~60 KiB for the
demodulator and the DMR mode.

Per object (Zephyr flags, `arm-none-eabi-size`):

| Object | text/rodata | .bss | Notes |
|---|---|---|---|
| `ambe3600x2450.c` | 15 263 | 23 656 | AMBE 2450 parameter decode + the DCT cosine cache |
| `pffft.c` | 9 072 | | 256-point transform (scalar path, no NEON on the M33) |
| `ecc_const.c` | 8 272 | | Golay tables of the mbelib ECC |
| `dmr_golay_table.c` | 8 192 | | syndrome → error pattern, generated from g(x) |
| `mbelib.c` | 4 092 | 4 | synthesis, spectral enhancement |
| `dmr_math.c` | 2 360 | | deterministic transcendentals |
| `mbe_unvoiced_fft.c` | 2 276 | | unvoiced synthesis |
| `dmr_vocoder.c` | 580 | 28 433 | API, FEC unpacking, state (7 824) + arena (20 480) |
| `mbe_adaptive.c` | 898 | | comfort noise, adaptive smoothing |
| `ecc.c`, `ambe_common.c` | 876 | | |
| `dmr_golay.c` | 82 | 4 | |
| **Total** | **~52 KiB** | **~52 KiB** | linked, after `--gc-sections`: +41.4 KiB flash |

The RAM is three quarters of one frame of history (3 × `mbe_parms`, 7 824 B), the
23 656 B DCT cosine cache and the 20 480 B arena that replaces the heap (the FFT
plan lives there).  Two ways to trim it, neither done here: the DCT cache is a
constant table (23 656 B) that could be generated at build time into flash, and
only 1 785 of its 5 832 floats are ever read; the arena could be sized to the
measured 11 200 B high-water mark.

## Cost on the Cortex-M33

Measured by decoding 100 frames in QEMU (`mps2-an505`, Cortex-M33, no cache
model, single-precision FPU) and counting executed instructions with a TCG
plugin.  Init and steady state are separated by differential runs (0, 1 and 101
frames):

| | Instructions | At 300 MHz |
|---|---|---|
| start-up, no frame | 15 611 | |
| first frame: DCT tables + FFT plan + decode | 4 219 606 | 14–22 ms once |
| steady state, per frame | **751 394** | **3.01 ms (CPI 1.2) … 4.01 ms (CPI 1.6)** |
| share of a 20 ms frame | | **15.0 % … 20.0 %** |

For DMR, with 1.5 voice frames per 30 ms slot on average (21–28 % when two
frames land in the same slot), the vocoder fits with room for the demodulator.
CPI 1.2–1.6 is the published range for an in-order M33 with zero-wait-state
SRAM; if code runs XIP from PSRAM, add 30–80 %.

`dmr_math.c` (the deterministic transcendentals) is why this is 751 k and not
the 784 k instructions per frame measured for the plain libm build in
`reports/opendmr_port.md` §4: it removes 423 `cosf` and one `sincos` per frame
from newlib.

## Verification

Reproduce everything with:

```sh
cd lib/dmr_vocoder/tests/qemu
./run_qemu.sh /path/to/frames.ambe 100
```

* **FEC layer**: `dmr_golay_table.c` is generated from the generator polynomial
  `g(x) = 0xC75` by `tools/gen_golay_table.py` and is byte-for-byte identical to
  `CGolay24128`'s decoding table (2048/2048 entries); all 2048 syndromes are
  covered by weight ≤ 3 patterns, as Golay(23,12) being perfect requires;
* **the C layer against OpenDMR's own wrapper**: decoding 100 frames with
  `opendmr.cpp` + `cgolay24128.cpp` and with `dmr_vocoder.c` + `dmr_golay.c`,
  same sources and flags, gives **byte-identical PCM**;
* **host against target**: the same 100 frames decoded on x86-64 and on the
  emulated Cortex-M33 give PCM that differs in **4 samples out of 16 160, by
  1 LSB** (`max |Δ| = 1`).  The difference is not in `dmr_vocoder.c` or
  `dmr_math.c` (those are bit-identical on both targets, checked value by value),
  it comes from the rounding of the remaining float code; the firmware must be
  compared against a reference built with the same compiler anyway.  Without
  `dmr_math.c` (`-DDMR_VOCODER_DETERMINISTIC_MATH=OFF`) the same comparison
  differs in 25 samples, because newlib's and glibc's `cosf` do not round alike;
* **against the reference decoder tool**: the pre-built `dmr_codec` of
  `work/opendmr-port` (same sources, different compiler) produces PCM that
  differs from this build in 2 samples out of 16 160, by 1 LSB.

Nothing here validates the decoder against over-the-air DMR, and the frames
available are the ones produced by the encoder in this repository.  Those decode
an octave high and about 4× louder than the input (`reports/opendmr_port.md` §2
already measured a pitch correlation of 0.085 for that encoder, i.e. it is the
weak half of the pair).  Since the decoder sources are used unmodified, this is
not something this integration changes, but it is the open question for the DMR
receive mode: real burst frames are needed to judge the decoder, and the
demodulator that would produce them does not exist yet.  The plumbing — frame
layout, FEC, parameter bits, PCM length — is verified; the voice quality is not.

## What changed with respect to OpenDMR, and why

The OpenDMR sources are used unmodified; the work is in what is *not* built and
in three compile-time hooks:

| Removed | Reason |
|---|---|
| `lib/opendmr/encoder/**` (18 C++ files) | OP25-derived encoder: 4.21 M instructions per frame, 84–112 % of the M33. Not viable; TX is a separate project (HiFi4 DSP or a fixed-point encoder) |
| `opendmr.cpp` (C++ wrapper) | pulls `libstdc++` (`operator new`, `__gxx_personality_v0`) and links the encoder. Its decode path is ~200 lines, reimplemented in C |
| `encoder/cgolay24128.cpp` | C++ and a GPL-2.0-only file; replaced by `dmr_golay.c` with a table generated from the standard's generator polynomial |
| `decoder/fftpack.c` | dead code: `mbe_unvoiced_fft.c` uses pffft. Would add 10 KiB and double-precision transcendentals |
| `__thread` / emutls | the 23 KiB DCT cache became plain `.bss`; emutls would need malloc |
| heap | the FFT plan allocation (11 KiB) goes to a static arena |
| `assert()`/`printf` | `-DNDEBUG`; pffft's asserts would pull stdio into the image |
| libm | `dmr_math.c` (optional, see `DMR_VOCODER_DETERMINISTIC_MATH`) and `-fno-math-errno` turn `sqrtf` into a `VSQRT` instruction |

Hooks added in the submodule (branch `c62/static-decode`, commit `85bdbb2`, not
pushed): `MBE_THREAD_LOCAL` can be defined away, `PFFFT_MALLOC`/`PFFFT_FREE` can
be redirected, and `MBE_SINCOSF` can replace `__builtin_sincosf`.  All three are
no-ops for existing users of the library.

## Limitations

* **No encoder.**  TX DMR needs a different engine (see above); the API has no
  `encode()` and the firmware has no path to it.
* **One instance, not reentrant**: one decoder state per device, no locking.
* **Decoder quality** is mbelib-neo's: intelligible, but it is a
  reimplementation of AMBE+2, and nothing was validated against live repeaters.
* **RAM**: 52 KiB of `.bss`, of which 23.6 KiB is a table that belongs in flash.
* **Init** costs 4.2 M instructions the first time a frame is decoded; do it when
  entering the mode, not in the audio callback.
* **PSRAM XIP** would slow everything down; the numbers above assume code and
  data in SRAM.

## Licensing

| Part | Licence |
|---|---|
| `lib/opendmr/decoder/**` (mbelib-neo, pffft) | **GPL-2.0-or-later** (pffft/FFTPACK-derived parts are BSD-like, GPL-compatible) |
| `lib/dmr_vocoder/**` (this layer) | **GPL-3.0-or-later** |
| OpenRTX / this firmware | GPL-3.0-or-later |

The decoder is GPL-2.0-**or-later**, so it can be taken under GPLv3 and linked
with the rest of the firmware: the combination is distributed as
**GPL-3.0-or-later**.  Unlike the plan sketched in `reports/opendmr_port.md` §5,
this integration does **not** compile `opendmr.cpp` or `encoder/cgolay24128.cpp`,
the two files without an SPDX header that inherited the repository's
GPL-2.0-only `LICENSE`.  The C++ wrapper is replaced by `dmr_vocoder.c`/
`dmr_golay.c` (GPL-3.0-or-later), and the Golay table is generated from the
generator polynomial by `tools/gen_golay_table.py`, so no GPL-2.0-only code is
linked in.  The encoder, which is GPL-3.0-or-later upstream, is not built either.

Distributing a C62 with this firmware means: publish the complete corresponding
source (OpenRTX, OpenDMR, the `c62/static-decode` submodule commit), keep the
copyright notices of mbelib (Pavel Yazev, ISC/GPL-2.0-or-later), OP25 (Max H.
Parke KA1RBI) and PFFFT/FFTPACK (J. Pommier, P. Swarztrauber/UCAR), and — GPLv3
§6 — keep the device flashable (`cskburn` over the programming pads, which this
firmware does not block).

**Patents are a separate risk.** AMBE/AMBE+2 is a DVSI proprietary vocoder and no
free licence grants patent rights over it.  GPL compliance does not cover that;
shipping a product with this decoder is a business decision (DVSI licence, or
accept the risk), independent of the software licence.
