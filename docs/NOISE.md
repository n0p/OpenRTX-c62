# Noise suppression on the receive path (RNNoise)

An optional speech-enhancement stage on the **analog receive path**, behind a
menu switch, with no cost while it is off. It is built from
**RNNoise v0.1.1** (BSD‑3‑Clause), vendored in `lib/rnnoise`, and it runs on the
Cortex‑M33 on 480‑sample (10 ms) blocks at 48 kHz — the block size the CSK6
codec already delivers, so the stage needs no re‑blocking of its own.

Measured headline: **RNNoise improves PESQ‑WB by +0.10…+0.43 and STOI by up to
+0.08 on noisy speech, removes 23 dB of engine noise and 16 dB of airband hiss,
adds 10 ms of latency — and costs 54 % of the M33 at 300 MHz** (66–73 % before
the `double` arithmetic was removed from the frame path), so it ships
**off** and off by default. The DSP (HiFi4) route that would make it affordable
is designed in [The HiFi4 route](#the-hifi4-route) with the kernel measured on
the DSP core; it is not implemented in this branch.

| | |
|---|---|
| Library | `lib/rnnoise` — RNNoise **v0.1.1**, `xiph/rnnoise` tag `v0.1.1` (`6cbfd53`), BSD‑3‑Clause |
| Model | 87 503 int8 weights (87.5 KiB of flash), 3 GRU (24/48/96) + 3 dense layers, 86 952 MAC per 10 ms frame |
| Frame | 480 samples @ 48 kHz, fixed |
| Where it runs | `core/noise_suppress.c`, called from the CSK6 codec capture path |
| Switch | Settings ▸ **Audio** ▸ *RX Noise* (`settings.noiseSuppression`) |
| Build option | `-DCONFIG_C62_NOISE_SUPPRESS=y`, **default n** |
| Measured cost | 1.35–1.37 M instructions per 10 ms frame (54 % of the M33); 1.60–1.91 M (66–73 %) before the `float` patch; 62 KiB of state in PSRAM |
| Added latency | 10 ms (one block), measured |

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y \
                                   -DCONFIG_C62_NOISE_SUPPRESS=y
```

Then on the radio: **Settings → Audio → RX Noise**, and turn it on. The setting
is stored with the rest of `settings_t` and survives a power cycle; the stage is
armed from it in `state_init()`.

With the build option off, none of the library is compiled or linked: the
feature does not exist. With the option on but the menu switch off, the cost is
one test of a cached `bool` per DSP frame inside `captureFrame()` — no
conversion, no buffers touched, nothing allocated (the 62 KiB state is a
`.psram_section` block that is only initialised on the first enable).

The row, the setting and the hook are all inside
`#ifdef CONFIG_C62_NOISE_SUPPRESS`; `platform/targets/c62/CMakeLists.txt` is
what builds `lib/rnnoise` and `core/noise_suppress.c` and links them into the
image, so a build without the option has neither the row nor the symbols.

## Where it sits

```
 BK4819 discriminator ──► ADC01 ──► DSP (HiFi4) capture ─┐
                                                         │  ICStream, 480 samples, 10 ms, 48 kHz
                                    csk6_codec captureFrame()
                                                         │
             ┌───────────────────────────────────────────┴──────────────┐
             │                                                          │
   SOURCE_RTX → SINK_SPK (analog FM RX)              SOURCE_RTX → SINK_MCU (demodulators)
             │                                                          │
   noiseSuppress_process()  ← the stage              DMR / TETRA / M17 / APRS taps
             │                                                          │
        FM queue → playback → speaker                     untouched
```

The hook lives in `captureFrame()` (`platform/drivers/audio/csk6_codec.c`) and
fires only when the radio audio is routed to the speaker
(`outputSource[SINK_SPK] == SOURCE_RTX`), i.e. **analog FM receive**. The
streams the demodulators read are never touched: running a speech enhancer over
a 4‑FSK discriminator would destroy DMR/TETRA/M17, and the DMR/TETRA voice
paths come from the vocoder as `SOURCE_MCU`, not from the discriminator.

**Transmit is not processed, on purpose.** The stock firmware runs iFlytek's CAE
(NS + AGC + VAD) at 16 kHz in the DSP on both directions; here the decision is
receive only, because (a) the TX chain's deviation and limiter are calibrated
against the current waveform and a non‑linear spectral gate would invalidate
that calibration, (b) RNNoise's VAD/gain model is trained on noisy *receive*
material, and (c) at 54 % of the M33 the stage cannot be afforded twice.

## Cost on the Cortex‑M33 (measured)

Bare‑metal harness, QEMU `mps2‑an505`, `arm-none-eabi-gcc`, per‑frame counts from
the TCG plugin (`work/nr-probe`, `measure.py`); payload is 480‑sample frames of
real 48 kHz speech mixed with real idle‑engine noise at 10 dB SNR, 20 frames
after 10 warm‑up frames. Cycles are estimated from the executed instruction mix
with the ARMv8‑M timing table, so they are an estimate; the instruction counts
are exact and deterministic. The numbers below were measured with `measure.py`'s
`REPO` pointing at this tree and the library's own flags
(`-O2 -fsingle-precision-constant -fno-math-errno`).

| build | instructions/frame (median) | min…max | est. cycles/frame | % of 300 MHz |
|---|---|---|---|---|
| `-O2` | **1 907 028** | 1 881 871…1 918 524 | 2 196 624 (CPI 1.15) | **73.2 %** |
| `-O2 -fsingle-precision-constant` | 1 807 180 | 1 782 846…1 818 817 | 2 091 122 | 69.7 % |
| `-O3 -funroll-loops` | 1 699 284 | 1 673 733…1 709 000 | 2 098 147 (CPI 1.24) | 69.9 % |
| `-O3 -funroll-loops -fsingle-precision-constant` | **1 599 912** | 1 575 182…1 609 769 | 1 993 630 | **66.5 %** |
| `-Os` | 2 192 946 | 2 166 919…2 207 305 | 2 606 676 | 86.9 % |
| `-O2 -fsingle-precision-constant`, **no `double` in the frame path** | **1 360 819** | 1 350 221…1 368 164 | 1 620 918 | **54.0 %** |
| `-Os`, no `double` in the frame path | 1 723 130 | 1 706 494…1 733 108 | 2 109 325 | 70.3 % |
| `-O3 -funroll-loops -fsingle-precision-constant`, no `double` | 1 151 496 | 1 141 089…1 157 060 | 1 522 456 | 50.7 % |

The last three rows are the current library: the per-frame `double` arithmetic
is gone (§ "No `double` in the frame path" below). The other rows are the
upstream algorithm and are kept as the reference the change is compared
against.

The firmware therefore builds the library at `-O2` (per source file: the
`zephyr_interface` `-Os` is appended after a target's options and would win),
which is **17 % cheaper** than the firmware‑wide `-Os` on the patched library
(1 427 148 against 1 723 130 instructions per frame). `-O3 -funroll-loops`
would take another 15 % off (1 151 496), and is not used: the library's output
was verified against the reference at the flags it ships with, and at either
number the stage does not fit the M33 in real time.

The upstream profile (before the local `float` patch) is kept here because it
says where the money was:

| instructions | share | function |
|---|---|---|
| 722 448 | 38.1 % | `compute_gru` (the three GRU layers) |
| 231 483 | 12.2 % | `rnn_fft_impl` (1920‑point complex FFT) |
| 168 176 | 8.9 % | `__aeabi_dadd` |
| 89 686 | 4.7 % | `__ieee754_sqrt` |
| 87 563 | 4.6 % | `__muldf3` |
| 81 629 | 4.3 % | `rnnoise_process_frame` |
| 78 139 | 4.1 % | `rnn_pitch_xcorr` |
| 65 946 | 3.5 % | `__divdf3` |
| 51 870 | 2.7 % | `rnn_fft_c` |

**~21 % of the instructions were double‑precision library calls** (~380 k of
1.80 M). They came from `biquad()` (whose coefficients are `double` by an
explicit cast) and from `sqrt()`/`log10()`/`floor()` promotions in the gain,
pitch and activation paths. They are gone; see the next section.

### No `double` in the frame path

The C62's M33 is FPv5-SP: a `double` add, multiply or divide is a libgcc call
(`__aeabi_dadd`, `__muldf3`, `__divdf3`) and `sqrt`/`log10`/`floor` are newlib
calls in double precision. The local patch (the only one besides moving the
scratch out of the stack, `PATCHES.md`) keeps the algorithm but does every
per‑frame operation in `float`:

| change | why it was double | saving |
|---|---|---:|
| `biquad()`: drop the `(double)` casts, accumulate in `float` | the casts promoted both taps and the state | ~200 k |
| `sqrt()` → `sqrtf()` in `compute_frame_features`, `pitch_filter`, `compute_pitch_gain` | no `f` suffix | ~90 k |
| `log10()` → `log10f()` for the band log‑energies | no `f` suffix | ~4 k |
| `floor()` → `floorf()` in `tansig_approx` | no `f` suffix | ~19 k |
| `sqrt(2/22)` → a `float` literal in the DCT | the compiler folded the root but kept the product in double | ~1 k |

Measured on QEMU `mps2‑an505` with the firmware's own flags (`-O2
-fsingle-precision-constant -fno-math-errno`), 20 frames after 10 warm‑up, same
payload as the table above:

| | before | after |
|---|---:|---:|
| instructions/frame, median | 1 807 180 | **1 360 819** (−24.7 %) |
| instructions/frame, min…max | 1 782 846…1 818 817 | 1 350 221…1 368 164 |
| estimated cycles/frame (CPI 1.19) | 2 091 122 | **1 620 918** (−22.5 %) |
| share of a 300 MHz M33 | 69.7 % | **54.0 %** |
| double/libm symbols in the steady state | 11 (`__aeabi_dadd`, `__muldf3`, `__ieee754_sqrt`, …) | **0** (`logf`/`log10f` remain, float) |

Quality and equivalence. PESQ‑WB and STOI were run with `pesq`/`pystoi` on two
real recordings (the material in `scripts/rnnoise/` is not in the tree: one
10 s FM‑broadcast capture at 48 kHz and a 14 s 8 kHz radio call resampled to
48 kHz), mixed with engine‑like noise at 10 and 5 dB SNR, output realigned by
the 480‑sample delay:

| material | SNR | PESQ‑WB in → old → new | STOI in → old → new |
|---|---:|---|---|
| radio call, 8 kHz voice | 10 dB | 1.208 → 1.795 → **1.795** | 0.892 → 0.903 → **0.903** |
| radio call, 8 kHz voice | 5 dB | 1.091 → 1.523 → **1.523** | 0.825 → 0.848 → **0.848** |
| FM broadcast, 48 kHz | 10 dB | 3.127 → 1.137 → **1.137** | 0.962 → 0.382 → **0.382** |
| FM broadcast, 48 kHz | 5 dB | 2.168 → 1.063 → **1.063** | 0.897 → 0.431 → **0.431** |

The old/new difference is ≤ 0.0002 PESQ and ≤ 0.0001 STOI, inside the ±0.02
criterion with three orders of magnitude to spare.  (The broadcast row is low
because the v0.1.1 model is trained on speech and the capture is music; it is
there to show the *delta*, not the absolute score.)

Sample by sample, 10 s of speech‑like material plus noise, `rn_demo` old
against new:

| metric | value |
|---|---|
| identical samples | 349 998 / 480 000 (72.9 %) |
| max \\|Δ\\| | **29 LSB** (0.09 % of full scale, −61 dBFS) |
| RMS Δ | **1.08 LSB** |
| samples within ±2 LSB | 95.7 % |
| SNR old → new | **72.3 dB** |
| noise attenuation (noise‑only, 10 s) | −16.7 dB both |

The difference is the biquad's rounding: in `double` the state kept 53 bits
between taps, in `float` it keeps 24. It is a noise floor 72 dB below the
signal, inaudible, and the filter is stable in both.

Memory: **62 288 bytes** of state (`rnnoise_get_size()` on this build: 18 492 B
unmodified, the local patch moves the per‑frame scratch in, which is why the
`ns` block in `core/noise_suppress.c` is 64 KiB) and **~8 KiB of stack**: with
`-fstack-usage` on the library as this tree builds it, the deepest chain the
stage puts on its caller is `rnnoise_process_frame` 2 832 B + the `compute_rnn`
frame it calls 3 632 B + `compute_gru` 1 592 B = 8 056 B, which against the
codec worker's own 0.9 KiB chain is 8.9 KiB. The state lives in
`.psram_section`; nothing of it is charged to the internal RAM budget. The
codec worker's stack grows from 2 KiB to 12 KiB only when the option is built
in (`core/stack_budget.h`), not the 4 KiB the first version of this stage
assumed: at 2 KiB the first processed frame would have overrun the guard
(`CONFIG_HW_STACK_PROTECTION`) and rebooted the radio.

### Why the M33 is not viable

5.4 ms of CPU per 10 ms of audio in a worker thread that also has to feed the
DAC, next to the `rtx`, UI and messaging threads on a 300 MHz core without
cache: enabling it produces dropouts.  The `float` patch takes it from 69.7 %
to 54.0 % but does not make it fit.  It is kept as a measured, switchable path
(and as the reference implementation the DSP port must match), and the
production route is the HiFi4.

## Quality (measured, real audio)

Method (`scripts/rnnoise/evaluate.py`, material and `quality.json` in the same
directory): 30 s of speech from
LibriSpeech `dev‑clean` (6 utterances, resampled to 48 kHz, −20 dBFS), mixed
with two noises; the mixture goes through **the same library build the firmware
uses**; the output is realigned by the measured 480‑sample delay before scoring.
The table predates the `float` patch; running both builds on the material
available here gives the same PESQ‑WB and STOI to ±0.0002
(§ "No `double` in the frame path").

* **engine**: Freesound `idle_engine` (CC0), looped with 30 ms crossfades —
  a real recording.
* **airband**: synthesised VHF‑AM receiver noise: 250–3400 Hz band‑limited noise
  with a pink tilt plus Poisson static crashes (3/s). No free *recording* of
  airband noise was available offline; the model is stated so it can be
  criticised.
* Metrics: **PESQ‑WB** (ITU‑T P.862.2, narrowband/wb variant on 16 kHz
  downsampled material), **STOI**, segmental SNR against the clean reference,
  noise floor on non‑speech frames, speech level change, per‑frame gain
  dispersion (pumping). POLQA (P.863) is a licensed ITU tool and is not used.

| noise | SNR in | PESQ‑WB in → out | STOI in → out | segSNR in → out | noise floor (speech pauses) | speech level | pumping σ |
|---|---|---|---|---|---|---|---|
| engine | 10 dB | 1.42 → **1.85** | 0.932 → 0.931 | 8.1 → 10.1 dB | −28.6 → **−41.0 dBFS** | −0.7 dB | 1.0 dB |
| engine | 5 dB | 1.19 → **1.47** | 0.870 → 0.872 | 3.1 → 7.5 dB | −24.7 → **−43.0 dBFS** | −1.8 dB | 2.6 dB |
| engine | 0 dB | 1.10 → **1.20** | 0.771 → 0.757 | −1.9 → 3.6 dB | −20.6 → **−44.9 dBFS** | −4.5 dB | 7.4 dB |
| airband | 10 dB | 1.40 → **1.72** | 0.898 → **0.948** | 25.3 → 12.9 dB | −35.4 → −36.3 dBFS | −0.2 dB | 0.9 dB |
| airband | 5 dB | 1.25 → **1.42** | 0.841 → **0.908** | 20.3 → 12.3 dB | −34.9 → −36.5 dBFS | −0.3 dB | 1.3 dB |
| airband | 0 dB | 1.18 → **1.27** | 0.771 → **0.853** | 15.3 → 11.2 dB | −33.9 → −36.3 dBFS | −0.3 dB | 1.9 dB |
| clean | ∞ | 4.64 → 4.37 | 1.000 → 0.999 | 35 → 14.6 dB | — | −0.1 dB | 5.1 dB |

Noise alone, at the level it has in the 5 dB mixture (no speech present):
**engine −23.2 dB, airband −15.9 dB** of total RMS.

How to read it:

* **It works where speech enhancement is needed.** PESQ‑WB improves in every
  noisy case (+0.10 at 0 dB, +0.28 at 5 dB, +0.43 at 10 dB), and STOI improves
  where the score is not already saturated.
* **It is not transparent on clean audio**: PESQ 4.64 → 4.37 and 14.6 dB of
  residual error against the clean reference. That distortion sets the ceiling
  on the segSNR column — which is why the airband rows show a *worse* segSNR
  (12–13 dB) even though PESQ and STOI improve: at 10 dB airband SNR the
  mixture is already better than the denoiser's own distortion floor. Judge
  those rows by PESQ/STOI, not by segSNR.
* **No pumping at usable SNRs**: gain dispersion is ~1 dB at 10 dB SNR and
  1.3 dB at 5 dB. At 0 dB with engine noise it reaches 7.4 dB and speech drops
  4.5 dB — that is the point where the model starts gating speech, and it is
  audible as a breathing artefact. Below ~3 dB SNR the stage does more harm to
  the voice than it removes from the noise.
* **It does not eat voice at normal levels**: speech level change is ≤ 0.7 dB
  down to 5 dB SNR with engine noise, and ≤ 0.3 dB with airband noise.
* **Very quiet input bypasses.** `compute_frame_features()` returns early when
  the frame energy is below 0.04 (`E < 0.04`), and the stage then passes the
  frame through untouched. That is why the "noise floor on speech pauses" column
  barely moves for the airband rows: those pauses are below the gate. Noise the
  receiver actually has to fight (engine rows) is well above it and drops
  12–24 dB.

## Latency

Measured by cross‑correlation of input and output on 12 s of speech: the output
lags the input by **exactly 480 samples = 10.0 ms**, in all six mixtures. That is
the algorithm's own one‑frame delay (RNNoise synthesises the previous analysis
frame; the reference demo throws the first output frame away for the same
reason), and it is the only latency the stage adds: the block is processed in
place inside the DSP frame that produced it, so there is no re‑blocking,
resampling or queueing.

## Flash and RAM

Same configuration for both images:
`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y` on top of the board's defaults
(TETRA, the message store, scan and VFO search on), Zephyr's `Memory region`
report:

| | FLASH | RAM | PSRAM |
|---|---|---|---|
| without the option | 608 816 B (58.06 %) | 170 328 B (64.97 %) | 471 372 B (25.69 %) |
| `CONFIG_C62_NOISE_SUPPRESS=y` | **709 728 B** (67.68 %) | **184 440 B** (70.36 %) | **541 724 B** (29.52 %) |
| delta | **+100 912 B** | **+14 112 B** | **+70 352 B** |

ITCM (8 472 B) and DTCM (11 664 B) do not move: nothing of the stage is a
DTCM/ITCM object.  The absolute FLASH figure moves by a few bytes from build to
build, because `git describe` is baked into `.rodata` and its length changes
with the commit count (the same commit measured 931 856 B one commit earlier
and 931 860 B at the tip); the deltas do not move.

Where the deltas come from, by `arm-none-eabi-size` on the objects:

* **FLASH +100 912 B** — `rnn_data.c` 87 683 B (the 87 503 int8 weights plus a
  little code), the five RNNoise objects 12 768 B of `.text` and the stage
  (`core/noise_suppress.c`) 532 B, less 71 B of section padding.
* **RAM +14 112 B** — the codec worker's stack 10 240 B (2 KiB → 12 KiB,
  § "Memory" above) and 3 864 B of `.bss`: RNNoise's per-frame scratch, which
  the local patch moved out of the C stack into a static block.  That block is
  in internal RAM; it could be moved to PSRAM, and is left where the library
  puts it.
* **PSRAM +70 352 B** — the `ns` block (64 KiB of `DenoiseState` + the two
  480-sample `float` conversion buffers, 69 392 B) and the 960 B de-interleaving
  buffer `nsSamples`.

RAM ends at 70 % in this configuration and at 79.13 % in the release image
(all modes, DMR TX included), under the 80 % ceiling that build must respect.

The release image, for the record, moves from FLASH 830 952 B / RAM 193 328 B /
PSRAM 676 590 B to FLASH 931 856 B / RAM 207 440 B / PSRAM 746 942 B with the
option on — and with the option off it is **byte for byte** the image the same
tree builds without the stage: the block of `platform/targets/c62/CMakeLists.txt`
that adds it is inside `if(CONFIG_C62_NOISE_SUPPRESS)`, so nothing of the library
is compiled, and the two images differ only in the `GIT_VERSION` string that
every commit bakes into `.rodata`.

## The HiFi4 route

The DSP is where this belongs, and the numbers below say why it is a project and
not a build flag.

### What was measured on the DSP

The `venus_hifi4` core in the CSK6011B declares a single‑precision FPU
(`XCHAL_HAVE_FP=1`) but the Cadence GCC overlay for it `#undef`s FP codegen, and
the vendor `libm`/`libgcc` were built *with* FP while the QEMU model does not
implement it — an FP instruction faults with `EXCCAUSE 0` (illegal instruction),
so a float build cannot even be run in the model. The route that is both
measurable and realistic is integer: RNNoise's dense and GRU layers are already
int8 weights × Q15 activations, and its features are fixed‑point friendly.

Measured on QEMU `-M sim -cpu venus_hifi4`, `work/nr-probe/build_hifi4k.sh`: an
int8 × Q15 dot product over 32 768 taps (the size of `denoise_gru_weights`),
integer accumulator, no library calls:

| build | instructions per dot product | instructions per MAC |
|---|---|---|
| `-O2` | 262 166 | 8.00 |
| `-O3 -funroll-loops` | 213 018 | 6.50 |

The inner loop GCC produces is `l8ui / l16ui / sext / mul16s / add / 2×addi` —
i.e. one MAC per ~6.5 instructions: the LX scalar pipeline has no packed
multiply‑accumulate, so plain C buys nothing over the M33 (86 952 MAC × 6.5 ≈
565 k instructions ≈ **19 % of a 300 MHz HiFi4** for the network alone, before
the FFT and pitch analysis).

The DSP only becomes attractive with the HiFi4 `ae_` engine: `AE_MUL16X16` and
friends accumulate 4–8 MAC per instruction, which takes the same 86 952 MAC to
~1.2–1.6 instructions per MAC (~110–140 k instructions, **4–5 %** of 300 MHz).
That is the "4–8 %" figure that circulates for RNNoise on this class of DSP, and
it is reachable **only with hand‑written intrinsics**, which the GPL GCC in this
tree does not provide — it needs `xt-clang` (a licence) plus a Q15 rewrite of
`denoise.c`, `pitch.c` and `kiss_fft.c`, verified against this C implementation.

### Service contract (designed, not built)

The AP already talks to the CP over LSF/URPC and shares sample buffers through
`ICStream` objects in the window at `0x200C0000`; `sampleCountPerFrame`,
`cellSize` and `channelCount` are fixed when the stream is created, so 480
samples is a creation parameter, not a protocol change. A `ns_service` beside
`lsf_audio` (`services/service_audio.c` is the pattern, `LSF_SERVICE_DEFINE`)
would:

| item | design |
|---|---|
| Command | `CMD_NS_OPEN {480, 1 ch, Q15}` → returns the stream id; `CMD_NS_CLOSE`; audio never travels in the command (URPC frames are 16 B) |
| Data plane | one capture‑side `ICStream`, 480 samples (960 B) per frame, 16‑bit mono, 4‑frame ring in each direction (7.7 KiB of shared SRAM) |
| Direction | AP producer → CP consumer (audio in), CP producer → AP consumer (audio out), committed with `ICStream_Consumer_commitRemote()` as the codec already does |
| Block | 480 samples in, 480 out, in place; the CP does not change the frame size, so the AP's 10 ms pacing is untouched |
| Latency | 10 ms algorithm + 0–10 ms of ring buffering each way ⇒ **20–30 ms** worst case, against the 10 ms of the M33 path |
| Arming | the switch writes `CMD_NS_OPEN`/`CLOSE`; with it closed the AP does not route the capture stream through the CP stage at all, so the DSP cost is zero |
| Fallback | if the CP image has no `ns_service`, `CMD_NS_OPEN` returns `-ENOSYS` and the switch stays off (the UI mirrors the result, as it already does for the M33 path) |

Until that exists, the M33 stage in this branch is the reference: same library,
same block size, same 10 ms delay, so its output is the test vector a DSP port
must reproduce.

## Repository notes

* `lib/rnnoise/` — vendored v0.1.1 with `COPYING` (BSD‑3), `PATCHES.md` and
  two local patches in `src/denoise.c`, `src/pitch.c` and `src/rnn.c`: the
  per-frame scratch moved out of the stack (output bit‑identical on 7 146
  frames) and the `double` arithmetic of the frame path turned into `float`
  (stats in § "No `double` in the frame path"). Not a submodule: the build runs
  offline in a container.
* `scripts/rnnoise/` — the host driver and the evaluation that produce the
  quality table, plus the `quality.json` quoted here.
* `openrtx/src/core/noise_suppress.c`, `openrtx/include/core/noise_suppress.h` —
  the stage, the switch, and the enable/disable plumbing.
* `platform/drivers/audio/csk6_codec.c` — the hook in `captureFrame()`.
* `openrtx/include/core/settings.h` — `noiseSuppression` (appended at the end of
  `settings_t`; an old NVM record fails the CRC and defaults come back, so one
  settings reset is expected after flashing this build for the first time).
* `openrtx/src/ui/default/{ui,ui_menu}.c`, `openrtx/include/ui/ui_default.h` —
  the *RX Noise* row in Settings ▸ Audio.
* `platform/targets/c62/{Kconfig.board,CMakeLists.txt}` —
  `CONFIG_C62_NOISE_SUPPRESS`.

### Expected conflict

`settings_t`, `ui.c` and `ui_menu.c` are the files every audio feature touches;
the parallel UNet noise‑reduction branch adds its own field and its own row in
the same places. The conflict is **additive**: keep both fields, both enum
entries and both menu rows.
