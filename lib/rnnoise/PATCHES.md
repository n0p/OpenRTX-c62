# RNNoise v0.1.1 — vendored library

Vendored copy of **RNNoise v0.1.1** (`https://github.com/xiph/rnnoise`,
tag `v0.1.1`, commit `6cbfd53eb348a8d394e0757b4025c6ded34eb2b6`, the head of
the tag on 2026‑11‑24), by Jean‑Marc Valin, Xiph.Org, Mozilla, Amazon and
Octasic.

* **Licence:** BSD‑3‑Clause, see `COPYING` and the header of every source file.
  No patent grant clause, no copyleft: it links into GPL‑3 firmware without
  changing the firmware's licence (GPL‑3‑or‑later is compatible with BSD‑3).
* **Model:** the v0.1.1 network is inside `src/rnn_data.c` as **87 503 int8
  weights** (`rnn_weight` is `signed char`, scaled by 1/256 in `rnn.c`), i.e.
  **87.5 KiB of flash**, not the multi-megabyte float tables of v0.2+.
* **Frame:** `rnnoise_get_frame_size() == 480`, i.e. **10 ms at 48 kHz**. This
  radio's codec already delivers exactly 480-sample DSP frames at 48 kHz, so
  the stage needs no re-blocking and adds no buffering delay.
* **Not vendored** on purpose: `rnn_reader.c` (needs a file system for external
  models), the training code, `examples/`, autotools scaffolding.

## Local patch 1 — working set out of the stack (1 file, `src/denoise.c`)

The library as published needs **35 400 bytes of stack** per
`rnnoise_process_frame()` call (two 960-point complex FFT buffers plus the
pitch buffers are automatic variables). No thread of this firmware has that,
and the internal RAM budget does not allow it (see `docs/NOISE.md`).

The patch adds the buffers to `DenoiseState` (`scratch_X`, `scratch_P`,
`scratch_x`, `scratch_y`, `scratch_xf`, `scratch_p`, `scratch_pitch`,
`scratch_band`, `scratch_band2`, `scratch_gain`) and threads `DenoiseState *`
through `forward_transform()` / `inverse_transform()`. The state is placed in
PSRAM by the firmware, so nothing moves into internal RAM.

Verified: **bit-identical output** against an unmodified v0.1.1 build on
7146 frames of 48 kHz speech (`cmp` of the raw output, 2026‑11‑24). Stack after
the patch: 2 856 B. State after the patch: 62 312 B (18 492 B unmodified).

## Local patch 2 — no `double` in the per-frame path

`src/denoise.c`, `src/pitch.c`, `src/rnn.c`. The Cortex-M33 (FPv5-SP) has no
double-precision FPU, so every `double` operation was a libgcc call and every
`sqrt`/`log10`/`floor` a newlib call in double. That was ~21 % of the stage's
1.80 M instructions per 10 ms frame.

Kept the algorithm and the block sizes; changed the precision of the per-frame
maths only:

| file | change |
|---|---|
| `denoise.c` | `biquad()` accumulates in `float` instead of casting the taps to `double`; `sqrt` → `sqrtf` in `compute_frame_features()` and `pitch_filter()`; `log10` → `log10f` for the band log-energies; `sqrt(2/22)` in the DCT replaced by the `DCT_SCALE` float literal |
| `pitch.c` | `compute_pitch_gain()` uses `sqrtf` |
| `rnn.c` | `tansig_approx()` uses `floorf` |

Not changed, on purpose: the tables built by `check_init()` (`half_window`,
`dct_table`) and the FFT twiddles in `kiss_fft.c` still use `double` `sin`/
`cos`. They run once per `rnnoise_create()`, not per frame, and changing them
would change every output sample rather than the last bits of it.

Measured on QEMU `mps2-an505` with the firmware's flags (`-O2
-fsingle-precision-constant -fno-math-errno`), 20 frames after 10 warm-up:
**1 807 180 → 1 360 819 instructions/frame (median, −24.7 %)**; zero
double-precision symbols left in the steady state.

Verified against the patch‑1 build on 10 s of speech-like material plus noise
(1000 frames): 72.9 % of the samples bit-identical, max |Δ| 29 LSB, RMS Δ
1.08 LSB, SNR 72.3 dB. PESQ‑WB and STOI on a real 8 kHz voice and an FM
broadcast capture differ by ≤ 0.0002 and ≤ 0.0001 respectively. Full tables in
`docs/NOISE.md`, "No `double` in the frame path".

## Updating

`git diff` this directory against a fresh `v0.1.1` checkout: the only
differences must be `CMakeLists.txt`, this file and the `[C62 patch]` hunks of
`src/denoise.c`, `src/pitch.c` and `src/rnn.c`. Do not take v0.2+: its model is
2.88 M parameters and does not fit this hardware (see `reports/npu_rnnoise.md`).
