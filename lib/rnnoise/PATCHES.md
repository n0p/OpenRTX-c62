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

## Local patch (1 file, `src/denoise.c`)

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

## Updating

`git diff` this directory against a fresh `v0.1.1` checkout: the only
differences must be `CMakeLists.txt`, this file and the `[C62 patch]` hunks of
`src/denoise.c`. Do not take v0.2+: its model is 2.88 M parameters and does not
fit this hardware (see `reports/npu_rnnoise.md`).
