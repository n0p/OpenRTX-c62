<!--
SPDX-FileCopyrightText: 2026 OpenRTX Contributors

SPDX-License-Identifier: GPL-3.0-or-later
-->

# IMBE on the C62: P25 Phase 1 voice, own decoder

The C62 now decodes P25 Phase 1 voice itself: `lib/imbe_vocoder` turns the 88
IMBE parameters of a 20 ms frame into 160 samples of 8 kHz PCM, on the M33, in
**195 k instructions per frame (3.9 – 5.2 % of the core)**, against **9.36 M
(180 – 240 %)** for mbelib, which cannot therefore keep up with the 6 M a 20 ms
frame has. Nothing from mbelib is compiled in the default image; mbelib-neo
stays in the tree as the reference this decoder is verified against, and as a
selectable backend for A/B on the radio.

The decoder is a C port of mbelib-neo's IMBE path with the same discipline as
the AMBE+2 (`lib/dmr_vocoder`) and D-STAR (`lib/dstar_vocoder`) ports: one
caller-owned instance, no allocation, no TLS, no libm, every table `const` in
flash and generated from the reference by a script, and the model's phase
accumulators wrapped before any trigonometry touches them.

There is **no IMBE encoder in this firmware**, so P25 transmit still goes out
without voice. That is now a decision with evidence behind it rather than a
claim that no encoder exists: one does, and §8 says where, what it costs and
why porting it is a separate work package.

## 1. The frame, and where the 88 bits come from

P25 Phase 1 voice is IMBE at 7200 bit/s: 144 bits per 20 ms frame, of which 88
are vocoder parameters and 56 are channel code.

```
LDU1/LDU2 payload, one of the nine voice words (144 bits)
    4 x Golay(23,12)      92 bits   words 1-4, 12 data bits each
    3 x Hamming(15,11)    45 bits   words 5-7, 11 data bits each
    7 x unprotected        7 bits   word 8
   -------------------------------------------------------------------
                         144 bits   -> 88 parameter bits after the PN
                                      descrambler (seeded from the first 12
                                      bits of word 1), TIA-102.BABA 6.4-6.5
```

The 88 parameter bits are numbered b0…bL+1 and their layout depends on the
harmonic count L (9…56) through three tables: `bo` (which parameter bit sits in
which position of the bit map), `ba` (bit allocation of the gain residual
blocks) and `hoba` (bit allocation of the higher-order DCT coefficients). The
fixed part is:

| field | bits | meaning |
|---|---|---|
| b0 | 8 | fundamental: `w0 = 4π/(b0+39.5)`, b0 0…207 voice, 208…255 not voice |
| b1 | K | band voicing, one bit per band, K = ⌊(L+2)/3⌋ (L<37) or 12 |
| b2 | 6 | gain of the first spectral block, `B2[]` codebook |
| b3…b7 | 46…52 | gain residuals of blocks 2…6, from `ba[L-9]` |
| b8… | 24… | higher-order DCT coefficients, from `hoba[L-9]` |
| bit 87 | 1 | alternating synchronisation bit (§6.5), the receiver checks it |

For L = 9 the split is 8 + 4 + 6 + (10+9+9+9+9) + (9+8+7) = 88. The per-L
numbers are in `lib/imbe_vocoder/imbe_tables.h`, generated — not retyped — from
the reference's own tables by `lib/imbe_vocoder/tools/gen_imbe_tables.py`.

Out of the 88 bits the decoder produces, per frame: a fundamental and L, a
voiced/unvoiced decision per harmonic, L spectral amplitudes `Ml`, and from
them a regenerated phase. Between frames it carries the prediction state
(`log2Ml` of the previous frame), the adaptive-smoothing state and the unvoiced
noise state, which is why the decoder is stateful and why `imbe_vocoder_reset()`
must be called at the start of each transmission.

## 2. The decoder

| file | what it is | licence |
|---|---|---|
| `imbe_vocoder.c/.h` | firmware-facing contract: one static instance, reset, decode | GPL-3.0-or-later |
| `imbe_decoder.c/.h` | the decoder: parameters, spectral enhancement, adaptive smoothing, synthesis | GPL-2.0-or-later |
| `imbe_math.h` | float-only `sincos`, `exp2`, `log2`, `sqrt`, `atan2` (no libm) | GPL-2.0-or-later |
| `imbe_fft.c/.h` | fixed 256-point real FFT (the AMBE+2 decoder's, shared) | GPL-2.0-or-later |
| `imbe_tables.h` | every table, `const`, in flash, generated | GPL-2.0-or-later |
| `tools/gen_imbe_tables.py` | the generator | GPL-3.0-or-later |

Pieces of the reference that are **not** in the port, deliberately: the soft-bit
decoders, the SIMD kernels, the tone frames (IMBE has none), and the DVSI 10 ms
subframe renderer — see §4.

Design points that cost real time to get right, in the order they bit:

* **`libm` is not usable.** newlib's `cosf()` reduces its argument in double;
  on an M33 with no double FPU that is a libgcc call per harmonic. `imbe_math.h`
  does the Cody-Waite reduction and the polynomials in float. Measured against
  glibc: `sincos` ≤ 2 ulp for |x| ≤ 2^22, `exp2` ≤ 1.2e-7 relative, `log2`
  ≤ 7.5e-7 absolute, `atan2` ≤ 2.8e-7 rad.
* **`atan2` needs the full reduction, not a polynomial.** The first version
  applied the fdlibm kernel polynomial directly over |x| ≤ 1; it is only valid
  to |x| ≤ 0.4375, and at the crossover of `atan2(√2·x, 1-x²)` (the output
  high-pass's phase, near 190 Hz) it was off by **1.6e-2 rad**, which the
  synthesis turned into 9 LSB on loud frames. The reduction in four pieces fixed
  it: 2.8e-7 rad.
* **The DCT cosine tables must be indexed like the reference's.** Two off-by-one
  errors — a missing zero row in `Ri` and a missing zero column plus a missing
  zero entry in the block offsets — made every amplitude wrong while the pitch,
  the harmonic count and the voicing decisions came out right, which is the most
  confusing possible failure. `gen_imbe_tables.py` now emits the unused element
  0 explicitly, and the decoder's tables are compared field by field against the
  reference's in the trace used during the port.
* **Phase accumulators are wrapped into [0, 2π) before use** (`wrap_two_pi`), as
  the reference does with `fmodf`. `PSIl` grows by up to ~500 rad per frame; a
  float cosine of an unwrapped 1e3-rad argument loses a bit per octave, and the
  synthesis is a sum of 56 of them.
* **The bit-map tables are extracted, never retyped.** `bo` is 48 × 79 pairs and
  a single transposed pair silently swaps two parameter bits.

## 3. Cost on the Cortex-M33

Measured by decoding the corpora in §4 on QEMU `mps2-an505` (Cortex-M33,
FPv5-SP, no cache model) and counting executed instructions with a TCG plugin,
`lib/imbe_vocoder/tests/qemu/run_qemu.sh`:

| | instructions |
|---|---|
| start-up, no frame | 15 720 |
| first frame (init + decode) | 100 584 |
| steady state, per frame (firmware objects) | **195 482** |
| the same sources rebuilt by the script, `-O2` | 193 933 |
| 300 MHz, CPI 1.2 | 0.78 ms per 20 ms frame = **3.9 %** |
| 300 MHz, CPI 1.6 | 1.04 ms per 20 ms frame = **5.2 %** |

The target was under 5 %: met at CPI ≤ 1.5, which is where the C62 runs code
from SRAM. `-O3` buys 2 % (189 797). For scale: mbelib needs 9.36 M per frame,
mbelib-neo's own IMBE path is of that order too, so this is a **48× reduction**
and the receiver now has 19 of its 20 ms back for the demodulator.

## 4. Verification

```sh
cd lib/imbe_vocoder/tests
./run_host.sh                                                   # corpora + reference
cd qemu && ./run_qemu.sh                                        # Cortex-M33 + count
FIRMWARE_BUILD=../../../build_c62 ./run_qemu.sh                 # firmware objects
```

Two corpora, 11 packed parameter bytes per frame (the air format):

* `tests/data/p25_vc_frames.imbe` — **261 frames from a real off-air P25 voice
  channel** (`work/p25-rx/samples/P25-C4FM-VC_IF.wav`, dumped with the host
  prototype's `--imbe-out`, converted by `tests/frames2bin.py`). Every frame
  arrived with 0 corrected FEC bits. This is the corpus that matters: it is what
  a receiver actually hands the vocoder.
* `tests/data/speech_frames.imbe` — **1000 frames from mbelib-neo's own IMBE
  encoder** over the codec2 test speech (`ve9qrp.wav`, 8 kHz), so the bench also
  covers full-scale audio and every L from 9 to 56.

The reference (`tests/ref_pcm.c`) is built from mbelib-neo twice: `mbe_processImbe4400Data()`
(its default P25 path, DVSI's 10 ms subframe rendering) and the same pipeline
with its **TIA-102.BABA 7.5 spec rendering** (`MBE_VOICED_SYNTHESIS_SPEC`),
which is the rendering this port implements.

**Parameters: bit-exact.** Decoding both corpora through the reference and
through the port in lockstep and diffing `log2Ml` per harmonic per frame gives
no difference at the 1e-3 level over all 1261 frames, and the enhanced amplitudes
agree to ≤ 2.4e-6 relative. Every field checked while porting (`Gm`, `Ri`, `Tl`,
`Cik`, `Vl`, `L`, `w0`, `K`) matched the reference's own `IMBE_DEBUG` trace digit
for digit, which is how the three table-indexing bugs of §2 were found.

**PCM, against the reference rendered the same way:**

| corpus | bit-identical frames | worst |Δ| | SNR |
|---|---|---|---|
| real capture (261, peaks ≈ ±6 k) | 138 / 261 | 9 LSB | 61.8 dB |
| encoder speech (1000, full scale) | 348 / 1000 | 127 LSB | 50.7 dB |

That residue is *not* noise in the sense of a bug and *not* compiler
sensitivity: mbelib-neo against itself built `-O0` and `-O2` is 1000/1000 and
261/261 bit-identical, and the float-domain agreement (before the 16-bit
conversion) is **75 dB**. It is the last ulp of each stage summed over up to 56
harmonics — per-harmonic agreement measured by projection is 0.006° in phase and
a few hundred ppm in amplitude, and 56 of those add up to 0.45 % of full scale
on the loudest frames. The AMBE+2 port got 1 LSB because AMBE+2's synthesis has fewer harmonics
and no low-band filter phase. If a future change needs the last LSB, the three
places to look are, in order: the reference's vector voiced kernels (which
re-seed the oscillator every four samples from `sincos(4ω)` where the scalar
path chains one-sample rotations), `log2f`/`exp2f` in the phase and amplitude
chain, and the shared FFT's rounding against pffft's.

**Two renderings, one model.** The reference's DVSI subframe rendering and its
spec rendering are the *same* signal: cross-correlation 0.998 at a 58-sample
offset. Sample-by-sample they read as −3 dB SNR against each other, and the port
reads the same −3 dB against the DVSI one, because that is what a 7 ms time
offset does to a waveform comparison. What a listener hears from the two is the
same voice; this port is the spec one, which is also what the AMBE+2 and D-STAR
ports implement.

**Host against target**: 997/1000 frames bit-identical, the rest 1 LSB, 113 dB
(the FFT contracts to VFMA on the M33 and not on x86-64; the model path is built
`-ffp-contract=off` on both).

**Robustness**: the decoder was run over the corpora with 1, 2, 4 and 8 random
bit errors per frame and over 20 000 random parameter vectors, under ASan/UBSan,
with no finding and no NaN: a frame it cannot use is repeated or muted
(TIA-102.BABA 7.7–7.8), never propagated.

## 5. In the firmware

* `platform/targets/c62/Kconfig.board`: `C62_P25_VOCODER` (default **y** when
  `C62_P25` is on) builds `lib/imbe_vocoder`; `C62_P25_MBELIB` (default **n**)
  links mbelib-neo instead, for A/B on the radio.
* `platform/targets/c62/CMakeLists.txt` adds the library with the Zephyr CPU and
  FPU flags (without them the archive links with VFP register-argument errors)
  and sets `-O2 -ffp-contract=off` per source, since the firmware-wide `-Os`
  would otherwise win.
* `openrtx/src/protocols/P25/P25Vocoder.c` is the adapter; `P25Speech.c` hands a
  frame the FEC could not fix over with an error count over the repeat
  threshold, so the decoder repeats or mutes it instead of the gap being filled
  with digital silence.
* The 11.6 KiB of decoder state live in `.psram_section`, the section the DMR and
  D-STAR vocoders use, so the mode's SRAM footprint does not grow. It is NOLOAD:
  `imbe_decoder_init()` clears the whole instance. Moving it to SRAM is one
  attribute and costs no flash.

Sizes (same branch, `-DCONFIG_C62_P25=y`, `arm-none-eabi-size` on the map):

| build | FLASH | RAM | PSRAM |
|---|---|---|---|
| no P25 at all | 471 168 | 152 403 | 411 492 |
| P25 without vocoder (PHY, FEC, speech queue) | 489 472 | 153 954 | 459 396 |
| P25 with our vocoder | **517 760** | **153 958** | **471 028** |
| the vocoder alone (difference of the two) | **+28 288** | **+4** | **+11 632** |
| **P25 RX complete (vs no P25)** | **+46 592** | **+1 555** | **+59 536** |

All seven modes on one image (`c62/mega` options: DMR RX, APRS, P25, TETRA,
scan, fusion, WFM): **604 544 B flash (57.7 % of 1 MiB), 165 751 B RAM (63.3 %
of 256 KiB), 471 892 B PSRAM (25.7 %)**, of which the IMBE decoder is the 28 KB
of flash and 11.6 KiB of PSRAM above. The three bit-map tables are `unsigned
char` in flash (`bo`, `hoba`, `ImbeJi`: 48 × 79 × 2, 48 × 50, 48 × 6 entries);
as `int` they would cost 22.7 KiB more, which is most of the difference between
this decoder and the AMBE+2 one in flash.

## 6. Licence

| part | licence |
|---|---|
| `lib/imbe_vocoder/imbe_vocoder.[ch]`, `tools/` | **GPL-3.0-or-later** |
| `lib/imbe_vocoder/imbe_decoder.*`, `imbe_math.h`, `imbe_tables.h`, `imbe_fft.*` | **GPL-2.0-or-later** (derived from mbelib-neo, which is GPL-2.0-or-later and itself carries mbelib's ISC portions) |
| the rest of this firmware | GPL-3.0-or-later |

The combination is distributed as GPL-3.0-or-later, which the derived decoder
allows. `imbe_tables.h` is generated from the reference's tables by
`tools/gen_imbe_tables.py`; the copyright notices of mbelib (Pavel Yazev) and of
mbelib-neo (arancormonk) are kept in the generated header.

**Patents are a separate question.** IMBE/AMBE is a DVSI vocoder family and the
software licence grants no patent rights over it (US 5,701,390, US 8,359,197 —
the latter said to run to about 2028). That is a statement of fact for the
reader, not legal advice, and it is unchanged by this work: the same
consideration already applies to the AMBE+2 decoder that ships in DMR mode.

## 7. What this does not prove

* The decoder is verified against mbelib-neo on **real off-air frames** and on
  encoder frames, but **not against a hardware P25 receiver**. The failure mode
  that would stay hidden is a systematic one in the *channel* layer (an entry of
  the PN descrambler or the bit map used consistently by both sides because both
  come from mbelib). A bench comparison with a real radio is the missing step.
* The spec rendering is not what DVSI's own decoders play (they render 10 ms
  subframes; §4 measures how the two relate). If the subframe rendering is
  ever wanted, the reference's `mbe_subframe.c` is the code to port and its extra
  cost is a pulse-train renderer: not measured here.
* Voice quality is mbelib-neo's model, not a P25 radio's. Nothing in this work
  touches the model, only how it is computed.

## 8. The encoder: verdict

**An open-source IMBE encoder does exist, and this file previously said it did
not.** Two, in fact:

1. **mbelib-neo ≥ 2.6** has `src/imbe/imbe7200x4400_enc.c` plus a shared speech
   analysis (`mbe_speech_analysis.c`), GPL-2.0-or-later: TIA-102.BABA chapter 6
   quantisation including the six block DCTs, the gain vector and the
   synchronisation bit, with `mbe_encodeImbe7200x4400Frame()` adding the
   Golay/Hamming codes and the PN interleave. Its README quantifies it against
   DVSI's P25 test vectors: level within 0.2 dB of DVSI's own encoding, log
   spectral distance 4.5 dB against DVSI's 4.7, the same voicing decision on
   82–89 % of each 1 kHz band — and states plainly that *on-air interoperability
   has not been verified*.
2. **Pavel Yazev's fixed-point IMBE encoder**, GPL-3.0, is **already in this
   tree**: `lib/opendmr/encoder/` (encode-only, `encode_4400()` emits the same 88
   bits). It came in with the OpenDMR package and was never wired up.

So the honest picture for the three options the task set out:

* **(a) port an analysis-by-synthesis encoder — viable, and a work package.**
  Neither candidate is integer C for an M33. mbelib-neo's encoder is float, host
  oriented, allocates its FFT plan and is ~1 500 lines of analysis plus quantiser
  search; measured on x86-64 with its SIMD kernels it costs **221 k
  instructions per frame**, which on the M33 in scalar C lands in the few-million
  range — the same order as the AMBE+2 encoder this firmware already ships in
  DMR TX (**1.43 M, 28–38 % of the core**). That is affordable in a transmit-only
  mode and not affordable in the receive path. It is the same size of job as
  `lib/dmr_encoder` was: port, prove bit-exactness against the reference on a
  corpus, measure, and then — the part that cannot be done without hardware —
  verify that a real P25 radio decodes it.
* **(b) train a model that inverts the decoder — rejected.** The output must be
  a *standard* bit stream; a learned inverse can only be as correct as the
  quantiser it imitates, and there is no corpus of paired speech and IMBE bits
  from real transmitters here to train or validate against. It would be research
  with no path to a verifiable claim, which is exactly what this task forbids.
* **(c) leave TX without voice — what this branch does.** P25 transmit stays
  HDU + TSBK + TDU: a valid, decodable call with the talkgroup and source
  address and no voice, as documented in `docs/TX_P25.md`.

The recommendation for the next work package: port mbelib-neo's encoder (it is
the one with published DVSI parity numbers and it is under active maintenance),
target the TX-only budget, and treat "a real P25 radio decodes it" as the
acceptance test — not "our decoder decodes it", which only proves the pair is
self-consistent.
