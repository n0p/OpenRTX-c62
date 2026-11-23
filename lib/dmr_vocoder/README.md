# dmr_vocoder — DMR AMBE+2 voice decoder backend

Decodes one DMR voice frame (9 bytes, 72 bits, 20 ms) to 160 samples of 8 kHz
PCM.  Built for the Retevis C62 (CSK6011B, Cortex-M33) and linked into the
firmware only when `CONFIG_C62_DMR_VOCODER=y`.

This directory is the **backend**: the DMR receive mode calls the two functions
below and knows nothing else.  The current implementation wraps the decoder half
of [OpenDMR](../opendmr) (mbelib-neo, GPL-2.0-or-later); an optimised decoder
can replace it without touching the callers, as long as it keeps the API and its
semantics (see "Backend contract").

## API

```c
#include "dmr_vocoder.h"

/* 9-byte AMBE+2 frame (canonical order A|B|C) -> 160 samples, 8 kHz, s16 */
int  dmr_vocoder_decode(const uint8_t frame[9], int16_t pcm[160]);

/* New transmission / loss of sync: forget pitch, phase and error history */
void dmr_vocoder_reset(void);
```

`dmr_vocoder_decode()` returns 0 on success, -1 on a NULL argument and -2 if the
internal arena is exhausted.  Diagnostics: `dmr_vocoder_last_errors()` (Golay
FEC corrections in the last frame), `dmr_vocoder_state_bytes()` and
`dmr_vocoder_arena_used()` (RAM accounting).

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
| `dmr_vocoder.h/.c` | public API, frame unpacking, FEC, static arena, decoder state |
| `dmr_golay.h/.c` | Golay(24,12)/Golay(23,12) syndrome decoders |
| `dmr_golay_table.c` | generated syndrome → error-pattern table (`tools/gen_golay_table.py`) |
| `dmr_math.h/.c` | deterministic cosf/sinf/cos/sin/logf/log2f/expf/exp2f (optional, on by default) |
| `dmr_math_redirect.h` | force-included in the OpenDMR sources when the above is on |
| `dmr_alloc.h` | declarations of the arena allocator handed to the OpenDMR sources |
| `tests/pcm_dump.c` | decode a file of frames, dump PCM (host and bare metal) |
| `tests/qemu/` | Cortex-M33 build, QEMU run, instruction count, PCM comparison |

## What is compiled, and what is not

Compiled from `lib/opendmr/decoder`: `mbelib.c`, `mbe_adaptive.c`,
`mbe_unvoiced_fft.c`, `ambe3600x2450.c`, `ambe_common.c`, `ecc.c`,
`ecc_const.c`, `pffft.c`.

Not compiled: the whole `lib/opendmr/encoder` (OP25 IMBE encoder, does not fit in
the M33: 4.2 M instructions per frame), `decoder/fftpack.c` (dead code, the
unvoiced synthesis uses pffft) and OpenDMR's C++ wrapper `opendmr.cpp`.
The C++ wrapper's decode path is reimplemented in `dmr_vocoder.c` /
`dmr_golay.c`: it is ~200 lines of bit unpacking plus the Golay decoders.

## Build hooks needed in the submodule

The OpenDMR sources are **not modified or relicensed**.  Three compile-time
hooks, added as a separate commit in the submodule (branch `c62/static-decode`,
commit `85bdbb2`), let this layer build them for a bare-metal MCU:

| Hook | Why |
|---|---|
| `-DMBE_THREAD_LOCAL=` | `__thread` becomes emutls on ARM, which needs malloc for the 23 KiB DCT cache; plain statics are what a single-instance firmware wants |
| `-DPFFFT_MALLOC/PFFFT_FREE` | the transform plan is allocated once; routing it to the static arena keeps the decoder off the C heap |
| `-DMBE_SINCOSF=dmr_sincosf` | avoids `__builtin_sincosf`, i.e. libm's `sincosf`, in favour of `dmr_math.c` |

`-DNDEBUG` silences pffft's asserts (they would pull in `printf`), and
`decoder/fftpack.c` is simply not compiled.

## Measured on the C62

+41 444 B of flash, +52 104 B of RAM, and 751 394 instructions per frame on the
emulated Cortex-M33 (3.0–4.0 ms of a 20 ms frame at 300 MHz).  See
`docs/DMR_VOCODER.md` for the breakdown and the verification procedure.
