# dmr_vocoder tests

Two things are checked, without hardware:

1. **the C layer is faithful**: the PCM it produces on the host is identical to
   OpenDMR's own C++ decode path (`opendmr.cpp` + `cgolay24128.cpp`) built from
   the same sources and flags.  `dmr_golay_table.c` is additionally verified
   entry by entry against `CGolay24128`'s decoding table, and the generator is
   run from the generator polynomial alone (`tools/gen_golay_table.py`);
2. **it works on the target**: the same sources are built for Cortex-M33 and run
   in QEMU (`mps2-an505`), the PCM is compared byte for byte, and the cost per
   frame is measured with QEMU's instruction counter.

## Running the Cortex-M33 verification

Needs `gcc`, `arm-none-eabi-gcc` (newlib), `qemu-system-arm` with TCG plugin
support and glib's headers:

```sh
cd lib/dmr_vocoder/tests/qemu
./run_qemu.sh /path/to/frames.ambe 100
```

The input is a flat sequence of 9-byte frames (e.g. the output of
`dmr_codec encode`, 1000 frames of 20 s of speech); at least `frames + 1` frames
are needed, the extra one separates the one-off initialisation (DCT tables, FFT
plan) from the per-frame cost.  The script

- builds the host binary and dumps `host.pcm`;
- builds three Cortex-M33 ELFs: 0, 1 and N+1 frames, all with `-O2`, the
  firmware's FPU flags (`-mfpu=fpv5-sp-d16 -mfloat-abi=hard`), `-ffp-contract=off`
  and the same defines as the CMake build;
- runs each in `qemu-system-arm -M mps2-an505` with a plugin that counts every
  executed instruction, and the N+1 one also writes `arm.pcm` through
  semihosting;
- compares `host.pcm` and `arm.pcm` byte for byte and prints
  `instructions/frame` plus the resulting CPU load at 300 MHz.

`run_qemu.sh` needs `pkg-config --cflags glib-2.0` for the counter plugin; if it
is missing, only the PCM comparison is affected.

## Host-only checks

`tests/pcm_dump.c` builds on its own and prints the decoded PCM and the arena
high-water mark:

```sh
gcc -O2 -std=c99 -ffp-contract=off -fno-math-errno \
    -DNDEBUG -DMBE_THREAD_LOCAL= -DPFFFT_MALLOC=dmr_vocoder_alloc \
    -DPFFFT_FREE=dmr_vocoder_free -DM_PI=3.14159265358979323846 \
    -DM_SQRT1_2=0.70710678118654752440 -DM_SQRT2=1.41421356237309504880 \
    -include ../../dmr_alloc.h -I.. -I../../../opendmr/decoder \
    ../pcm_dump.c ../dmr_vocoder.c ../dmr_golay.c ../dmr_golay_table.c \
    ../dmr_math.c ../../../opendmr/decoder/*.c -lm -o pcm_dump
```

`dmr_math.c` is only needed when the deterministic math is enabled (the default,
see `DMR_VOCODER_DETERMINISTIC_MATH`); `../../../opendmr/decoder/*.c` here means
the eight decoder sources listed in `CMakeLists.txt`, without `fftpack.c`.

## Results

Recorded in `docs/DMR_VOCODER.md`: decoded PCM per frame, instructions per frame
on the emulated M33, and the flash/RAM delta in the firmware build.
