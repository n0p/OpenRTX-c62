# dmr_vocoder — DMR AMBE+2 voice decoder backend

Decodes one DMR voice frame (9 bytes, 72 bits, 20 ms) to 160 samples of 8 kHz
PCM.  Built for the Retevis C62 (CSK6011B, Cortex-M33) and linked into the
firmware only when `CONFIG_C62_DMR_VOCODER=y`.

This directory is the **backend**: the DMR receive mode calls the two functions
below and knows nothing else.  The implementation is a pure C reimplementation of
the decoder half of [OpenDMR](../opendmr) (mbelib-neo, GPL-2.0-or-later): no
C++, no heap, no thread-local storage, no libm, every table in flash, 4 492 B of
frame-to-frame state and a 4 052 B per-frame scratch that the caller can put in
slower memory.

## API

```c
#include "dmr_vocoder.h"

/* 9-byte AMBE+2 frame (canonical order A|B|C) -> 160 samples, 8 kHz, s16 */
int  dmr_vocoder_decode(const uint8_t frame[9], int16_t pcm[160]);

/* New transmission / loss of sync: forget pitch, phase and error history */
void dmr_vocoder_reset(void);
```

`dmr_vocoder_decode()` returns 0 on success and -1 on a NULL argument.
Diagnostics: `dmr_vocoder_last_errors()` (Golay FEC corrections in the last
frame), `dmr_vocoder_state_bytes()` and `dmr_vocoder_arena_used()` (always 0:
this backend allocates nothing).

## Backend contract

Any replacement must:

1. keep the header `dmr_vocoder.h` and the two functions above, with the same
   return codes and no allocation per frame;
2. be usable from an audio thread without locks, from a single static instance
   (`dmr_vocoder_reset()` may be called at any time between frames);
3. consume frames in the canonical, de-interleaved order A(24) | B(23) | C(25);
   undoing DMR's 36-dibit over-the-air interleaving is the caller's job;
4. keep state between calls (the synthesis window overlaps two frames), so the
   first frame after a reset fades in from silence;
5. avoid C++, `__thread`, and the C library heap; static RAM and flash counts
   must stay in the same order of magnitude (see docs/DMR_VOCODER.md).

## Layout

| File | Purpose |
|---|---|
| `dmr_vocoder.h/.c` | public contract and the firmware's single instance |
| `dmr_decoder.h/.c` | decoder core: FEC, frame unpacking, MBE parameters, synthesis, PCM |
| `dmr_fft.h/.c` | fixed 256-point real FFT (2 × 128-point complex + untangling) |
| `dmr_math.h` | sin/cos/exp2/log2/sqrt in single precision, no libm |
| `dmr_tables.h` | generated ROM tables (~28 KiB, `tools/gen_tables.py`) |
| `tests/pcm_dump.c` | decode a file of frames, dump PCM (host and bare metal) |
| `tests/qemu/` | Cortex-M33 build, QEMU run, instruction count, PCM comparison |
| `tools/gen_tables.py` | regenerates `dmr_tables.h` from the reference sources |
| `tools/make_ref.sh`, `ref/` | rebuilds `tests/data/speech_ref.pcm` with the reference decoder |

Only these three C files are compiled.  Nothing comes from `lib/opendmr`, which
stays in the tree as the reference this decoder was verified against: no
encoder, no `opendmr.cpp`, no pffft, no libstdc++.

## Build options

Each source carries its own options, set per file in `CMakeLists.txt` so that
they land after the firmware-wide `-Os`:

* `-O2 -fno-math-errno` for all three;
* `-ffp-contract=off` on `dmr_decoder.c` (fidelity: the AMBE phase accumulator
  reaches ~1e6 rad, where FMA fusion changes the reconstructed phase);
* `-ffp-contract=fast` on `dmr_fft.c`, which accumulates no phase (measured: no
  effect on the output).

## Measured on the C62

+39.5 KiB of code and tables, 8.5 KiB of static RAM (4 492 B of state in DTCM
and 4 052 B of per-frame scratch in PSRAM on the C62), and ~114 000 instructions
per frame on the emulated Cortex-M33 (0.46–0.61 ms of a 20 ms frame at 300 MHz,
2.2–3.0 % of one core).  See `docs/DMR_VOCODER.md` for the breakdown and for the
verification procedure (`tests/qemu/run_qemu.sh`).
