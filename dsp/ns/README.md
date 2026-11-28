# RNNoise on the CSK6011B HiFi4

The noise-reduction stage of the analog FM receive path, moved off the Cortex-M33
onto the DSP (`venus_hifi4`).  This directory is the measured base, not a
finished service: it builds the same 10 ms frames the firmware processes, runs
them on the DSP core, and checks them against the firmware's own output.

```
ns_dsp.h / ns_dsp.c     the service body: 480 int16 in, 480 int16 out, in place
rnn_dsp.c               optional HiFi4 matrix-vector kernel (see "SIMD" below)
ns_tables.h             the M33's window/DCT/twiddle tables, generated
bench/bench_main.c      frame harness: 2 warm-up frames + N measured, marked
bench/tables_main.c     dumps the init-time tables of a build
bench/pcm_frames.h      8 frames of 48 kHz speech + engine noise, 10 dB SNR
tests/test_ns_dsp.c     the 8 frames through ns_dsp_process(), int16 out
third_party/newlib_fdlibm/  newlib's log10f (Sun fdlibm), see below
common/                 M33 startup + linker script, QEMU instruction plugin
tools/                  literal typing, table generation, cost and diff tools
```

## What the port is

`lib/rnnoise` unchanged, compiled for the DSP.  What it needed to get there:

* **Float, not double.**  The HiFi4 has single-precision only (`XCHAL_HAVE_DFP`
  is 0), so every `double` in the frame path is a libgcc call.  The library was
  already float after the C62 patches; what was left was literal typing: the
  firmware builds with `-fsingle-precision-constant`, and **the Xtensa clang in
  the RI-2021.7 bundle accepts that flag and ignores it**, so `x*1.3` was still
  a double multiply.  `tools/fp_literals.py` writes the literals out with an `f`
  suffix for the DSP build (101 soft-double call sites per frame before, 0
  after).
* **One libm function.**  `log10f` is on the per-frame path (band energies).  The
  Xtensa libm's `log10f` is not correctly rounded — 1414/2000 values differ from
  the correctly rounded one — while the M33 runs newlib's Sun fdlibm version.
  `third_party/newlib_fdlibm/` compiles that implementation for the DSP under
  `-Dlog10f=ns_log10f`, and it reproduces the M33's values on 2000/2000 sampled
  inputs.  `sqrtf` and `floorf` are IEEE-exact and need nothing.
* **The M33's tables.**  `half_window`, `dct_table` and the FFT twiddles are
  `sin`/`cos` in double, rounded to float.  newlib and the Xtensa libm disagree
  in the last bit on a third of them (237/480, 388/484, 683/960), which is enough
  to change every output sample.  `ns_tables.h` is generated from the M33 build's
  own dump (`make ns_tables.h`) and used by `-DRNNOISE_FIXED_TABLES`; the
  allocation and metadata still come from `kiss_fft`.
* **The reductions in program order.**  `rnn.c` keeps its sums in one
  accumulator per neuron, ascending over inputs; `ns_matvec()` is that loop, and
  it is what a DSP kernel would replace.  The output gate's recurrent term is
  `w*state[j]*r[j]`, which C evaluates as `(w*state[j])*r[j]` — the port keeps
  that order too, because rounding it the other way moves the last bit.

## What bit-exact is measured against

`make m33-dump` builds `lib/rnnoise` for the Cortex-M33 with the firmware's flags
(`-O2 -fsingle-precision-constant -fno-math-errno`) plus **`-ffp-contract=off`**,
runs it on QEMU `mps2-an505`, and prints every output frame as raw float bits.
`make dsp-dump` does the same on the ISS, and `make compare` diffs them:

```
frames bit-exact: 8/8
samples bit-exact: 3840/3840 (100.0000 %)
```

Contraction is the one flag that matters here.  With GCC's default
(`-ffp-contract=fast`, which is what the shipped firmware gets), the M33 fuses
multiply-adds into VFMA in places the port cannot follow, and the same
comparison gives **max |error| 0.777 LSB, SNR ≈ 72 dB** — inaudible, but not
bit-exact.  Either the DSP build follows the M33's contraction choices (not
practical) or the firmware's `lib/rnnoise` target builds with
`-ffp-contract=off` when the stage runs on the DSP.  `-fno-tree-vectorize`
changes nothing (measured: bit-exact either way).

`make test` covers the service body as the LSF audio path will use it: the 8
frames go through `ns_dsp_process()` as int16, and the output is compared with
the M33 float dump converted with the firmware's rounding rule — also 3840/3840.

## Cost

`make cost` runs two builds of the same harness on the Xtensa ISS (0 and 8
measured frames) and subtracts, so the number is one frame with nothing else in
it:

| | instructions/frame | cycles/frame | % of 300 MHz |
|---|---:|---:|---:|
| Cortex-M33, same harness, `-ffp-contract=off` | 1 556 938 | — | 51.9 % (instr.) |
| Cortex-M33, shipped build (published) | 1 360 819 | 1 620 000 | 45 / 54 % |
| **HiFi4, this port** | **671 327** | **934 801** | **31.2 %** |

CPI on the ISS model is 1.39; the ISS has no caches, so treat the cycle column as
an ideal-pipeline estimate and the instruction column as the hard number.
Per frame the cost splits (sampled profile, ±2 %):

```
41.5 %  compute_gru            the three GRU layers, int8 weights x float input
14.5 %  rnnoise_process_frame  biquad, band energies, gain interpolation
13.8 %  rnn_fft_impl           960-point complex FFT, twice per frame
13.4 %  pitch                  xcorr, search, downsample, doubling removal
 2.2 %  rnn_fft_c
 1.8 %  compute_dense
```

## SIMD: measured, and not worth it yet

`make cost SIMD=1` replaces `ns_matvec()` with `rnn_dsp.c`, two neurons per
VFPU instruction (`MUL.S`/`ADD.S` on `xtfloatx2`, never fused).  It is
**bit-exact** (8/8 frames) and **slower**: **1 030 213 instructions/frame**, 1.53×
the scalar code.  The reason is the weights: they are `signed char`, so every
multiply needs an int8→float conversion, and the conversion costs more than the
two-lane multiply saves; clang also scalarises the union-based splat/pack, so
the generated code is single-lane `mul.s`/`add.s` with extra memory traffic.
The productive direction is a 16-bit path (`AE_MUL16X4`, four neurons per
instruction, weights widened once at build time to int16 in the DSP image),
which cannot be bit-exact and would have to spend the tolerance budget above.

## Running it

```
source ~/xtensa/env.sh          # XTENSA_CORE=venus_hifi4, core registry, licence stub
make m33-dump dsp-dump compare  # the bit-exactness check
make cost                       # instructions and cycles per 10 ms frame
make m33-cost                   # the same measurement on the M33 (QEMU plugin)
make test                       # the service body, int16 in and out
make tables                     # diff the init-time tables of both builds
make trace                      # stage-by-stage trace of one frame, if they ever diverge
make host                       # x86 build of the harness, sanity only
```
