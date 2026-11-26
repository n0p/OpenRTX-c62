<!--
SPDX-FileCopyrightText: 2026 OpenRTX Contributors

SPDX-License-Identifier: GPL-3.0-or-later
-->

# DMR vocoder on the C62: AMBE+2 decode

The C62 can decode DMR voice: `lib/dmr_vocoder` turns the 9-byte AMBE+2 frame
of a burst into 160 samples of 8 kHz PCM (20 ms).  **Decode only** – the encoder
does not fit in the CSK6011B's Cortex-M33 – and the DMR receive mode that will
call it is not part of this branch yet: what lands here is the vocoder, its
integration in the firmware build, and the measurements that show it fits.

The backend is a pure C reimplementation of the decoder half of OpenDMR
(mbelib-neo): no C++, no heap, no thread-local storage, no libm, every table in
flash and the whole decoder state in one 8.5 KiB object.  Nothing from
`lib/opendmr` is compiled; the submodule stays in the tree as the reference this
decoder was verified against.

Measured (details in the sections below): **113 370 instructions per frame**,
i.e. **0.45–0.60 ms per 20 ms frame (2.3–3.0 % of one M33 at 300 MHz)**,
**+38 680 B of flash** and **+8 552 B of static RAM** in the firmware image,
against **+41 444 B / +52 104 B** and **751 394 instructions per frame** for the
previous backend, which compiled the OpenDMR decoder sources.

## API

```c
#include "dmr_vocoder.h"

/* One AMBE+2 voice frame -> 20 ms of audio.  Canonical order A(24)|B(23)|C(25). */
int  dmr_vocoder_decode(const uint8_t frame[DMR_VOCODER_FRAME_BYTES],
                        int16_t pcm[DMR_VOCODER_PCM_SAMPLES]);

/* New transmission, or loss of synchronisation. */
void dmr_vocoder_reset(void);
```

`dmr_vocoder_decode()` returns 0 on success and -1 for a NULL argument; the state
is initialised on the first call, so an explicit reset before the first frame is
optional.  Frames must be decoded in order: the synthesis window overlaps two
frames, so the first frame after a reset fades in from silence (same behaviour
as the reference implementation).  Diagnostics: `dmr_vocoder_last_errors()`
(Golay corrections in the last frame), `dmr_vocoder_state_bytes()` (8 540 B) and
`dmr_vocoder_arena_used()` (always 0: nothing is allocated).

### The two halves, for encrypted calls

A caller that has to take a keystream off the coded bits between the FEC and
the synthesis (DMR Basic/Enhanced Privacy, NXDN) uses the per-instance header
and the two halves of the same call:

```c
#include "dmr_decoder.h"   /* dmr_vocoder_t, the per-instance API */

int n = dmr_vocoder_unpack(frame, d);       /* Golay FEC: frame -> 49 bits   */
/* ... d[0..48] ^= keystream ... */
dmr_vocoder_decode_data(&v, d, pcm);        /* synthesis, no FEC             */
```

`dmr_vocoder_unpack()` returns the corrected-bit count that
`dmr_vocoder_decode_frame()` reports, and `dmr_vocoder_decode_data()` is the
mirror of mbelib's `mbe_processAmbe2450Dataf()` (and of the second half of
`mbe_processAmbe3600x2450Framef()`). The DMR mode places its instance in DTCM
through this header (`dmr_voice.c`); the single-instance `dmr_vocoder.h` above
is the same decoder in plain `.bss`.

### The backend is replaceable

`lib/dmr_vocoder` is a *backend*: the DMR mode only sees `dmr_vocoder.h`, which
owns the single instance of the decoder core declared in `dmr_decoder.h`.  A
replacement must keep this contract:

1. the same header and the two functions, same return codes, no allocation per
   frame;
2. one static instance, no locks, `dmr_vocoder_reset()` callable between frames
   from the audio thread;
3. frames consumed in canonical, **de-interleaved** order A(24) | B(23) | C(25);
4. state kept between calls (first frame after reset fades in from silence);
5. no C++, no `__thread`, no libc heap, and flash/RAM in the same order of
   magnitude as the numbers below.

Start-up is no longer a concern for a replacement: the tables are constants in
flash, and the first frame costs 162 420 instructions in total (init plus the
fade-in frame), not the 4.22 M of the previous backend.

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
* the library sets its own per-source compile options, because the flags
  inherited from `zephyr_interface` (including the firmware-wide `-Os`) are
  appended after a target's options and would win:
  `-O2 -fno-math-errno` on all three sources, `-ffp-contract=off` on the model
  path and `-ffp-contract=fast` on the FFT.

### What the DMR receive mode still has to do

The vocoder takes the *canonical* 72-bit frame.  A burst carries the voice as 36
dibits in the slots 12–47 and 108–143, interleaved; the mapping must be undone
before calling `dmr_vocoder_decode()` (36-entry table, see
`reports/opendmr_port.md` §6.1 and dsd-neo's `ambe_interleave.h`).  Per burst of
30 ms there are two voice frames in one slot and one in the next (2 bursts = 3
frames), so a one-frame jitter buffer keeps the 20 ms cadence.  Demodulation,
sync, CACH and the embedded LC are still missing: they are the next step, and
they are independent of this library.

### Stack

`-fstack-usage` with the firmware's flags puts the peak frame at **1 592 B**, all
in `dmr_vocoder_decode_frame()` (`dmr_vocoder_decode()` adds 24 B and the FFT
helpers are inlined; there is no recursion).  The DMR receive branch decodes on
its own playback thread (`DmrSpeech.c`, `THREAD_STACK` = 8 192 B), so the 2 KiB
of the audio worker and of the main thread are not in the path — with 2 KiB it
would also fit, but with only ~430 B to spare.

## Size in the firmware

Build with `-DCONFIG_C62_DMR_VOCODER=y -DC62_DMR_VOCODER_FORCE_LINK=ON`,
compared with the same branch without the symbol:

| Region | Baseline | With vocoder | Delta |
|---|---|---|---|
| FLASH | 395 000 B (37.67 % of 1 MiB) | **433 680 B (41.36 %)** | **+38 680 B (+37.8 KiB)** |
| RAM | 143 144 B (54.61 % of 256 KiB) | **151 696 B (57.87 %)** | **+8 552 B (+8.4 KiB)** |
| `zephyr.bin` | 395 000 B | 433 680 B | +38 680 B |

Against the previous backend:

| | Previous backend | This backend | Delta |
|---|---|---|---|
| FLASH | 436 444 B (41.62 %) | **433 680 B (41.36 %)** | −2 764 B |
| RAM | 195 248 B (74.48 %) | **151 696 B (57.87 %)** | −43 552 B (−42.5 KiB) |
| instructions/frame | 751 394 | **113 370** | −84.9 % |

Section by section (from `arm-none-eabi-size -A`): `text` +10 688 B, `rodata`
+27 992 B (all tables, `const`, in flash), `bss` +8 548 B, `datas` +0; the extra
4 B of the RAM region is section alignment.

Per object (`arm-none-eabi-size`, text includes rodata):

| Object | text | .bss | Notes |
|---|---|---|---|
| `dmr_decoder.c` | 35 642 | 0 | FEC, parameters, synthesis, tables |
| `dmr_fft.c` | 2 968 | 0 | 256-point real FFT, twiddles in flash |
| `dmr_vocoder.c` | 132 | 8 548 | API + the one instance (8 540 B) |
| **Total** | **38 742** | **8 548** | linked: +38 680 B, −62 B dropped by `--gc-sections` |

The largest tables are the pre-computed DCT/HOC cosines (7 140 B), the AMBE
quantisers (6 144 + 2 048 B), the Golay(23,12) syndromes (4 096 B), the
synthesis/WOLA windows (2 944 B) and the FFT twiddles with the bit-reversal
permutation (1 536 B).  The reference computed the DCT cache at run time into
23.6 KiB of thread-local `.bss`; here it is flash, and the first frame no longer
pays 5 832 `cosf`.

RAM is at 58 % of SRAM with the vocoder in, which leaves ~104 KiB for the
demodulator and the DMR mode (with the previous backend it was 74 %, 60 KiB).

## Cost on the Cortex-M33

Measured by decoding the 1000 lab frames in QEMU (`mps2-an505`, Cortex-M33, no
cache model, single-precision FPU) and counting executed instructions with a TCG
plugin.  Init and steady state are separated by differential runs (0, 1 and 1000
frames).  Two builds are measured: the objects the Zephyr firmware produced
(`FIRMWARE_BUILD=...`, the number quoted everywhere in this document) and a
fresh `-O2` rebuild with the same per-file flags:

| | Firmware objects | Rebuild |
|---|---|---|
| start-up, no frame | 14 446 | 14 449 |
| first frame (init + decode) | 162 420 | 162 542 |
| steady state, per frame | **113 370** | 114 134 |
| at 300 MHz, CPI 1.2 … 1.6 | **0.45 … 0.60 ms** | 0.46 … 0.61 ms |
| share of a 20 ms frame | **2.3 … 3.0 %** | 2.3 … 3.0 % |

The one-off init inside the first frame is about 49 000 instructions (~0.16 ms
at 300 MHz).  The previous backend needed **4.22 M** (~14 ms) there, because it
built the DCT tables and the FFT plan at run time.

For DMR, with 1.5 voice frames per 30 ms slot on average (2.3–4.5 % when two
frames land in the same slot), the vocoder is no longer the constraint: it
leaves the M33 free for the demodulator.  CPI 1.2–1.6 is the published range for
an in-order M33 with zero-wait-state SRAM; if code runs XIP from PSRAM, add
30–80 %.

## Verification

Everything below is reproduced by

```sh
cd lib/dmr_vocoder/tests/qemu
./run_qemu.sh                                      # sources rebuilt here
FIRMWARE_BUILD=../../../build_c62 ./run_qemu.sh     # objects from the firmware build
```

The script decodes `tests/data/speech.ambe` (1000 frames, 20 s of speech from
this repository's encoder) on x86-64 and on the emulated Cortex-M33, compares
both with `tests/data/speech_ref.pcm` — the same frames decoded by OpenDMR's own
decoder, `tools/make_ref.sh` — and counts instructions with QEMU's TCG counter.

* **fidelity against the reference**, 1000 frames: **803/1000 frames bit-exact
  from the host build and 811/1000 from the firmware objects, the rest differing
  by 1 LSB; SNR 106.39 dB (host) and 106.48 dB (ARM)**, max |Δ| = 1.  That is
  the envelope the reference has against itself under a different compiler, and
  it matches what the delivered decoder measured;
* **host against target**: 970/1000 frames bit-exact, the rest 1 LSB, SNR
  115.95 dB.  The difference comes from the FFT, where `-ffp-contract=fast` lets
  GCC fuse multiplications into VFMA on the M33 (x86-64 baseline has no FMA).
  The FFT accumulates no phase, so the effect is 1 LSB and slightly *closer* to
  the reference (811 vs 803 exact frames).  Building `dmr_fft.c` with
  `-ffp-contract=off` makes host and target **bit-identical** (1000/1000) at
  +2.8 % instructions (116 588/frame): one line in `CMakeLists.txt` if that
  invariant is preferred;
* **FEC layer**: the Golay(23,12) syndrome table in `dmr_tables.h` is generated
  from the generator polynomial `g(x) = 0xC75` by `tools/gen_tables.py`, and the
  decoder is complete for the perfect code (2048 syndromes ↔ 2048 patterns of
  weight ≤ 3), as the reference's table-driven decoder is;
* **robustness**: the decoder was fuzzed with 21 000 frames (real, bit-flipped
  and random) under ASan/UBSan with no findings and no NaN; frame unpacking and
  FEC are reached here through `dmr_vocoder_decode()`.

Nothing here validates the decoder against over-the-air DMR, and the frames
available are the ones produced by the encoder in this repository.  Those decode
an octave high and about 4× louder than the input (`reports/opendmr_port.md` §2
already measured a pitch correlation of 0.085 for that encoder, i.e. it is the
weak half of the pair).  The decoder reproduces the reference, so this is not
something this integration changes, but it is the open question for the DMR
receive mode: real burst frames are needed to judge the decoder, and the
demodulator that would produce them does not exist yet.  The plumbing — frame
layout, FEC, parameter bits, PCM length — is verified; the voice quality is not.

## What changed with respect to the previous backend

The previous backend compiled the decoder half of OpenDMR (mbelib-neo, pffft)
for the M33 through three compile-time hooks in the submodule.  This one is a
reimplementation, so nothing from the submodule is compiled at all:

| Removed | Why |
|---|---|
| `lib/opendmr/decoder/**` from the build | replaced by `dmr_decoder.c` + `dmr_fft.c`, which keep the model and the arithmetic of the reference |
| pffft | runtime-planned generic transform, 11 KiB of heap; replaced by a fixed 256-point real FFT with its twiddles in flash |
| libm (`cosf`, `exp2f`, …) | newlib's `cosf` reduces the phase in double: 79 % of the reference decode on the M33.  `dmr_math.h` does it in float |
| the 23.6 KiB thread-local DCT cache | pre-computed at build time into `dmr_tables.h` |
| `dmr_math.c` and its redirect header, `dmr_alloc.h`, the arena | no libm and no allocation left to redirect |
| `dmr_golay.c` and its 8 KiB table | the Golay(23,12) syndromes are part of `dmr_tables.h` |
| the C++ wrapper and the encoder | already out; the submodule is now reference material only |

The three fidelity-critical details are documented in the sources: the phase
reduction is a correct Cody-Waite reduction (not a wrap), `-ffp-contract=off`
protects the phase accumulator, and FMA is used explicitly only where it cannot
change a decision (`__builtin_fmaf` in the polynomials).

## Limitations

* **No encoder.**  TX DMR needs a different engine; the API has no `encode()`.
* **One instance, not reentrant**: one decoder state per device, no locking.
* **Decoder quality** is mbelib-neo's: intelligible, but it is a
  reimplementation of AMBE+2, and nothing was validated against live repeaters.
* **RAM**: 8.5 KiB of `.bss` (the three parameter sets, the WOLA history and the
  per-instance FFT/PCM scratch).
* **PSRAM XIP** would slow everything down; the numbers above assume code and
  data in SRAM.

## Licensing

| Part | Licence |
|---|---|
| `lib/dmr_vocoder/dmr_vocoder.[ch]` (this layer) | **GPL-3.0-or-later** |
| `lib/dmr_vocoder/dmr_decoder.*`, `dmr_fft.*`, `dmr_math.h`, `dmr_tables.h` | **GPL-2.0-or-later** |
| OpenRTX / this firmware | GPL-3.0-or-later |

The decoder core is derived from mbelib-neo (GPL-2.0-or-later), so it can be
taken under GPLv3 and linked with the rest of the firmware: the combination is
distributed as **GPL-3.0-or-later**.  The OpenDMR submodule is no longer linked,
which also removes the two files of its C++ wrapper that had no SPDX header and
inherited the repository's GPL-2.0-only `LICENSE`.  `dmr_tables.h` is generated
from the AMBE+2 model equations by `tools/gen_tables.py`, not copied.

Distributing a C62 with this firmware means: publish the complete corresponding
source (OpenRTX and the derived decoder), keep the copyright notices of mbelib
(Pavel Yazev, ISC/GPL-2.0-or-later) and of OP25 (Max H. Parke KA1RBI), whose
model the tables encode, and — GPLv3 §6 — keep the device flashable (`cskburn`
over the programming pads, which this firmware does not block).

**Patents are a separate risk.** AMBE/AMBE+2 is a DVSI proprietary vocoder and no
free licence grants patent rights over it.  GPL compliance does not cover that;
shipping a product with this decoder is a business decision (DVSI licence, or
accept the risk), independent of the software licence.
