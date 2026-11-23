# DMR receive mode

DMR (ETSI TS 102 361-1) receiver for the Retevis C62: 4FSK demodulation of the
radio's discriminator output, slot synchronisation, CACH, colour code and
timeslot, and voice through the AMBE+2 decoder. **Receive only** — there is no
DMR transmitter in this firmware.

The mode is a port of the host prototype in `work/dmr-rx`
(`reports/dmr_rx_proto.md`), which validated the chain against a real repeater
capture before any of this was written. This document says what the firmware
does differently, what it costs and what is still missing.

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y
```

`CONFIG_C62_DMR_RX` (`platform/targets/c62/Kconfig.board`) selects
`CONFIG_C62_DMR_VOCODER` and adds the protocol sources, `OpMode_DMR` and
`CONFIG_DMR` for the rest of the firmware. Key 5 cycles FM → M17 → TETRA → DMR.

## Where the code lives

```
openrtx/include/protocols/DMR/
    DmrDefs.h     constants, sync words, interleaver tables
    DmrFec.h      CACH (7,4), Golay(20,8), BPTC(196,96), CRC-16, de-interleaver
    DmrPhy.h      streaming demodulator interface
    DmrSpeech.h   voice queue and playback thread
    DmrStatus.h   what the UI shows
openrtx/src/protocols/DMR/
    DmrDefs.c  DmrFec.c  DmrPhy.c  DmrSpeech.c  DmrStatus.c
openrtx/src/rtx/OpMode_DMR.cpp
openrtx/include/rtx/OpMode_DMR.hpp
tests/platform/c62_dmr_host.c        host harness (PHY + vocoder + PCM out)
```

The protocol layer is C, like `protocols/TETRA`: the prototype was C++17 for
the convenience of `std::vector` on the host, and none of it survives the port.
`OpMode_DMR` is C++ because every mode handler is, and it does nothing but move
buffers.

## The chain, as it runs on the radio

```
48 kHz discriminator (BK4819 audio bypass, 25 kHz flat RX path)
  RRC α=0.2 matched filter, 81 taps, evaluated at 2 samples/symbol (24 kHz)
  sync search on the 4 slot words, 7 positions per 30 ms slot once locked
  per-burst timing: eye-opening search in 0.25-sample steps
  levels and carrier offset from the 24 sync symbols of that burst
  CACH → AT/TC/LS        slot type → colour code and data type
  voice: 216 payload bits → 3 × 72-bit AMBE+2 frames (no extra FEC)
  data:  BPTC(196,96) → 96-bit block, CRC-16 per mask
  voice → DmrSpeech queue → playback thread → dmr_vocoder_decode() → 8 kHz
```

Four things differ from the prototype, all of them because the prototype sees
a whole file and the firmware sees 30 ms at a time:

1. **Matched filter at 2 samples/symbol.** The prototype filters at 48 kHz
   (3.9 MMAC/s); the firmware decimates by two while filtering (1.94 MMAC/s)
   and interpolates the 24 kHz stream with the same Catmull-Rom kernel. The
   coarse phase search covers the even 48 kHz samples, the quarter-sample grid
   covers the odd ones.
2. **Slot grid.** After a burst, the next sync can only be a whole number of
   144-symbol slots away. The firmware searches 7 positions around each
   predicted slot instead of all 4800 symbols/s (20 times less CPU) and, more
   important, a spurious correlation in the middle of a burst can no longer
   shadow the real sync of the next slot — that was the one false positive the
   prototype produced in the synthetic capture, and it costs three frames of
   audio each time.
3. **Running symbol timing.** The fractional offset is carried from burst to
   burst and the coarse phase steps by two samples when it reaches ±4. The
   prototype re-estimates ±1 sample around a fixed phase, so after about four
   seconds of a capture with a few ppm of clock error the eye closes: its EVM
   climbs 0.02 → 0.39 and the decoded frames degrade. On the sigidwiki capture
   the firmware keeps **0.02 EVM from the first burst to the last**.
4. **EVM gate.** A burst whose eye is closed past 0.50 is dropped instead of
   emitted: at that point the AMBE+2 Golay protection is exhausted and the
   frames are noise.

## Cost

Per second of audio on the Cortex-M33 at 300 MHz (single-precision FPU, no
DSP extension):

| stage | cost | CPU |
|---|---|---|
| RRC, 81 taps at 24 kHz | 1.94 MMAC/s | ~1.3 % |
| sync search, 7 positions/slot | ~0.2 MMAC/s | ~0.2 % |
| per-burst timing, levels, EVM | ~40 kFLOP per burst × 16.7 | ~0.3 % |
| CACH, Golay, BPTC, CRC | < 1 kFLOP per burst | negligible |
| **PHY total** | | **~2 %** |
| **vocoder** (measured, `docs/DMR_VOCODER.md`) | 751 k instructions per 20 ms frame | **15–20 %** |

The vocoder is three times the cost of everything else. It runs on the playback
thread, never in the rtx thread: a burst hands over three 9-byte frames in
microseconds and the demodulator goes back to sampling.

### AP or HiFi4

Everything on the AP. The PHY is 2 % and the vocoder 15–20 % of one core; the
HiFi4 would only be worth waking for a fixed-point vocoder or the stock
denoiser, and both are separate projects. What must stay on the AP is the
control logic (slot grid, FEC, state machine) — small, stateful, branchy. If
anything ever moves to the DSP it is the per-sample RRC, which SIMD likes.

## Memory

Measured on `c62/dmr-rx` with `-DCONFIG_C62_DMR_RX=y`, against the same tree
with the option off (394 968 B flash / 143 144 B RAM) and against the vocoder
alone (436 444 / 195 248, the measurement of `docs/DMR_VOCODER.md`):

| build | FLASH | RAM |
|---|---|---|
| no DMR | 394 968 B (37.67 %) | 143 144 B (54.61 %) |
| vocoder only | 436 444 B | 195 248 B |
| **DMR RX** | **443 440 B (42.29 %)** | **198 600 B (75.76 %)** |

zephyr.bin 443 440 B, sha256 `aac8d120e0a6754505797e3f7e62f06294d683eb215d3c51bcc91fdae043bb45`.

The mode costs **+6 996 B of flash** (PHY, FEC, speech, status, handler) and
**+3 352 B of RAM** over the vocoder alone, **+48 472 B of flash** and
**+55 456 B of RAM** over a firmware without DMR. The RAM is:

| object | bytes | note |
|---|---|---|
| vocoder DCT cache | 23 656 | `.bss`, belongs in flash (see below) |
| vocoder arena | 12 288 | was 20 480; sized to the measured 11 200 B high-water |
| vocoder state | 7 824 | three AMBE parameter sets |
| baseband double buffer | 5 760 | 2 × 1440 samples of 48 kHz, the audio stream |
| matched filter ring | 4 096 | 1024 samples at 24 kHz, 205 symbols |
| everything else in the mode | ~1 200 | input history, speech queue, status |

The vocoder arena was trimmed as part of this work: 20 480 → 12 288 B, 8 KiB
back, on the strength of the measured high-water mark. The next 23 KiB is the
AMBE DCT cosine cache, a constant table that is built at run time and only
1 785 of whose 5 832 floats are read; turning it into a build-time table in
flash is a change to the OpenDMR sources (the vocoder submodule), so it is not
done here.

## What the screen shows

Line 2: `CC13 TS2` once a burst has decoded, `DMR searching` before that.
Line 1: `Voice e2%` or the burst data type with the EVM of the last burst.
The green LED follows synchronisation, and the RSSI/S-meter is the ordinary
one. `dmrStatus` (`DmrStatus.h`) is what the UI reads, the same split the TETRA
mode uses.

Audio is muted unless the last burst arrived less than 300 ms ago
(`dmrPhy_synced()` is also what opens the squelch for the standby timer), and
the playback thread releases the speaker after a second of silence.

## Verification

* **Bit-exact against a known transmitter.** The synthetic capture of the
  prototype (real speech → AMBE+2 → DMR modulator) decodes to **300/300 frames
  identical** to the frames the modulator started from. That covers the
  matched filter, sync, timing, CACH, de-interleaving and the frame layout.
* **Real signal.** On the sigidwiki repeater capture (34 s, 12 kHz IQ through
  the prototype's own discriminator dump): **43 voice bursts, 43 decoded**, EVM
  0.020–0.027 throughout, colour code 13 from the slot types, a VoiceLC header
  and a Terminator at the end of the transmission, and the four data bursts at
  the start of the capture. The prototype found the same 43 voice bursts but
  its last 20 decode with EVM 0.19–0.39.
* **Vocoder.** `dmr_vocoder_decode()` and OpenDMR's `opendmr_decode()` produce
  byte-identical PCM for the same frames (checked frame by frame).
* **Audio.** `tests/platform/c62_dmr_host.c` writes 28.6 s of 8 kHz PCM from
  that capture; 89 % of the energy of the loudest 200 ms is below 1 kHz, the
  spectral signature of speech the prototype report used.
* **Firmware.** Builds with the Zephyr flow, no warnings from the new files;
  the TETRA and M17 modes are untouched apart from sharing the flat RX path.

Reproduce the host part:

```sh
cmake -S lib/dmr_vocoder -B build_dmr_vocoder && cmake --build build_dmr_vocoder
cc -O2 -I openrtx/include -I lib/dmr_vocoder tests/platform/c62_dmr_host.c \
   openrtx/src/protocols/DMR/{DmrDefs,DmrFec,DmrPhy}.c \
   build_dmr_vocoder/libdmr_vocoder.a -lm -o c62_dmr_host
./c62_dmr_host -o out.wav -c bursts.csv work/dmr-rx/out/discr_sigidwiki.wav
```

## Limitations

* **No TX.** The AMBE+2 encoder does not fit in the M33 (4.2 M instructions per
  frame, 84–112 % of the core, `docs/DMR_VOCODER.md`), and the TDMA transmit
  switch (2.5 ms guard, PA keying inside a 30 ms slot) has not been designed.
* **No LC RS(12,9).** The 96-bit link control block is decoded by BPTC but its
  Reed-Solomon layer is not, so **talkgroup and source/destination IDs are not
  shown**; colour code and data type come from the slot type, which needs no
  RS. The next useful piece of FEC, about 80 lines of GF(256).
* **Scalar RRC and interpolation.** No SIMD, no Farrow interpolator: the
  implementation sits about 1.5 dB above the theoretical 4-PAM curve, the same
  gap the prototype measured. Halving the filter cost with the 2× decimation
  was preferred to closing that gap, which no measurement on this hardware
  would notice.
* **One slot at a time by accident, not by design.** Both timeslots are
  demodulated and the voice of both would be queued if both carried speech;
  there is no "follow this timeslot" policy like the TETRA mode's.
* **No equaliser.** DMR indoors does not need one; with two strong paths the
  sync detection suffers and that has not been measured.
* **No AFC.** The discriminator turns carrier offset into DC, which the
  per-burst level estimate removes; the BK4819 AFC stays where the FM mode
  leaves it.
* **Audio quality is mbelib-neo's**, a reimplementation of AMBE+2. It is
  intelligible on the real capture, but nothing here has been compared against
  a DVSI reference decode.

## Next step: transmit

In this order, because each one is testable on its own:

1. **RS(12,9) decode** for the LC: source and destination IDs on the screen,
   and a real call-state machine (header → voice → terminator) instead of the
   300 ms sync timeout.
2. **AMBE+2 encode somewhere else.** Options: the HiFi4 DSP (it already runs
   the stock denoiser and has SIMD), or a fixed-point port of the OP25 encoder
   with the pitch search pruned. The 9-byte frames, the interleaver
   (`dmr_fec_voice_interleave()`) and the BPTC encoder (already in the
   prototype) are all in place; what is missing is the encoder and the slot
   timing.
3. **TDMA transmit timing.** 30 ms slots, 2.5 ms guard, PA keying and the
   BK4819 deviation set to 1944 Hz (the C62 transmits 1600 Hz in its own
   firmware, which the receiver tolerates because it estimates levels per
   burst — a DMR transmitter must not).
4. **Colour code and talkgroup policy**, which is a UI/state question, not a
   PHY one.
