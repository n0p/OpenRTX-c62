# lib/imbe_vocoder tests

Host and Cortex-M33 verification of the P25 IMBE decoder. The results, the raw
numbers and the discussion are in `docs/IMBE.md`; this file is just how to run
them.

```sh
./run_host.sh                    # corpora, reference comparison, ASan/UBSan pass
cd qemu && ./run_qemu.sh         # Cortex-M33: host-vs-target PCM + instruction count
FIRMWARE_BUILD=../../../build_c62 ./run_qemu.sh     # count the shipped objects
```

`MBELIB=/path/to/mbelib-neo ./run_host.sh` points the reference build somewhere
else; the default is `/home/n0p/src/c62/ref/mbelib-neo`, already built (the
script needs `build/libmbe-neo.a` there).

## Corpora

Both are 11-byte packed parameter records (88 bits, b0 first), the format the
P25 LDU carries.

| file | frames | where it comes from |
|---|---|---|
| `data/p25_vc_frames.imbe` | 261 | real off-air P25 Phase 1 voice: `work/p25-rx/samples/P25-C4FM-VC_IF.wav` through that prototype's `--imbe-out`, then `frames2bin.py` |
| `data/speech_frames.imbe` | 1000 | mbelib-neo's own IMBE encoder over the codec2 test speech `wav/ve9qrp.wav` (8 kHz), through `encode_imbe.c` |

To rebuild the first from a new capture:

```sh
cd ~/src/c62/work/p25-rx && ./build/p25_rx capture.wav --format wav-iq --imbe-out dump.txt
./frames2bin.py dump.txt data/p25_vc_frames.imbe
```

## Programs

| file | what it does |
|---|---|
| `pcm_dump.c` | decodes ITERS frames with lib/imbe_vocoder; runs on the host and on the emulated M33 |
| `ref_pcm.c` | the same frames through mbelib-neo, in `spec` (TIA-102.BABA 7.5 rendering) or `dvsi` (its default subframe rendering) mode |
| `encode_imbe.c` | builds the encoder corpus with mbelib-neo's IMBE encoder |
| `frames2bin.py` | p25-rx dump to packed records |
| `compare_pcm.py` | bit-exact frames, worst difference, SNR; exits non-zero below `--min-snr` |
| `fuzz.c` | bit-error sweep and random parameter vectors, built with ASan/UBSan |
| `qemu/` | mps2-an505 startup, linker script and the TCG instruction-counting plugin |
