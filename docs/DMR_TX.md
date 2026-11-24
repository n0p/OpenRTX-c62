# DMR transmit mode

DMR (ETSI TS 102 361-1) transmitter for the Retevis C62: link control, CACH,
FEC, burst framing, the 4FSK modulator and the PTT state machine, on the same
flat discriminator path the receiver of `docs/DMR_RX.md` uses.

The chain is complete and verified against the firmware's own receiver, symbol
for symbol, and it now has its AMBE+2 encoder: a pure C port of the OpenDMR
encoder in `lib/dmr_encoder`, bit-exact with the reference and 1.43 M
instructions per 20 ms frame (24 – 38 % of the M33).

## Status at a glance

| piece | state |
|---|---|
| voice LC header, voice bursts, terminator, CACH, BPTC(196,96), RS(12,9), Golay(20,8) | done |
| polyphase RRC 4FSK, TDMA slot grid, PA ramps, pulse tail carried between slots | done |
| PTT state machine in `OpMode_DMR` (lead → LC → voice → terminator → tail) | done; pure function, host-tested, two safety timeouts |
| `CONFIG_DMR_TX`, off by default | done |
| AMBE+2 encoder (`lib/dmr_encoder`, C, `CONFIG_DMR_TX_ENC`) | done, bit-exact with the reference |
| deviation calibration (`REG_40 = 0x476`) | **pending**: a starting point, not a measurement |
| adjacent channel power, real air test | **pending**: needs a bench |
| audio cadence and key-up latency on hardware | **pending**: the loopback checks the frames, not the clock |

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_DMR_TX=y
```

`CONFIG_DMR_TX` (`platform/targets/c62/Kconfig.board`, `depends on
C62_DMR_RX`) is **off by default**. With it off the transmitter is not
compiled, the PTT branch of `OpMode_DMR::update()` is not compiled either, and
PTT in DMR mode does nothing. Without it the image is
`c62/full-aprs` plus 32 bytes (the vocoder hook).

`CONFIG_DMR_TX_ENC` (default **y**, depends on `DMR_TX`) builds the encoder and
overrides `dmrVocoder_encode()`, which is what makes PTT transmit. With it off
the chain is built but the encoder is not: PTT refuses to key, which is the
configuration a build that takes its frames from the DSP wants.

Key 5 cycles FM → M17 → TETRA → DMR, as before.

## Where the code lives

```
openrtx/include/protocols/DMR/
    DmrTx.h        the transmit chain interface, layouts, slot geometry
    DmrVocoder.h   the encoder hook (what a platform has to provide)
    DmrFec.h       + dmr_fec_bptc_encode(), dmr_fec_rs129_encode()
openrtx/src/protocols/DMR/
    DmrTx.c        link control, CACH, burst build, polyphase RRC, ramps
    DmrTxTables.c  RRC polyphase and PA ramp tables, GENERATED
    DmrVocoder.c   weak default of the hook
    DmrVocoderEncoder.c  the override: dmrVocoder_encode() -> dmr_encoder_encode()
    DmrFec.c       + the encode halves of BPTC and RS(12,9)
openrtx/src/rtx/OpMode_DMR.cpp        the state machine
lib/dmr_encoder/                      the AMBE+2 encoder (C, see below)
    dmr_encoder.{c,h}     firmware-facing contract, one static instance
    dmr_enc_core.c        fixed-point analysis (pitch, FFT, V/UV, spectrum)
    dmr_enc_frame.c       quantiser, predictor model, Golay, frame packing
    dmr_enc_basic.h       the ETSI fixed-point operators, static inline
    dmr_enc_int.h         parameters and the two halves' interface
    dmr_enc_ambe_tables.{c,h}  quantiser tables from mbelib, GENERATED
    dmr_enc_trig.{c,h}    cosines and logarithms, GENERATED
work/dmr-tx/                          host tools, harnesses and measurements
work/dmr-tx/enc-c/                    encoder build, bit-exactness and cost
```

`scripts/gen_dmr_tx_rrc.py` in `work/dmr-tx` regenerates `DmrTxTables.c`; the
encoder's tables come from `work/dmr-tx/enc-c/gen_ambe_tables.py` and
`gen_trig.c` (which prints the reference's own expressions as hex floats).
Nothing in the firmware generates tables at run time.

## The chain, bit by bit

```
3 AMBE+2 frames (9 B each, A|B|C order the decoder uses)
  interleave three 72-bit frames                     216 payload bits
  + 10-bit slot type (Golay 20,8 around the sync)     + 20 bits
  + 48-bit SYNC (MS voice / MS data)                  + 48 bits
  264 bits -> 132 symbols, dibit -> {-3,-1,+1,+3}
  + 12-symbol CACH (BS layout only)                   +-2,5 ms
  -> 144 symbols in a 30 ms slot
  polyphase RRC alpha=0.2, 10 samples/symbol (Q14)    1440 samples at 48 kHz
  PA envelope (MS layout, 1 ms raised cosine per edge)
```

Data bursts (voice LC header, terminator) go through the full chain instead:
9-byte LC PDU → RS(12,9) parity → BPTC(196,96) → the two 98-bit halves around
the sync, as clause 6.3 lays them out.

### Voice link control

`dmrTx_linkControl()` builds the 9-byte full LC of a group voice call
(FLCO 0x00, FID 0x00, service options 0x00, destination, source), packs it MSB
first and appends the three parity bytes of the (12,9) Reed-Solomon code over
GF(2⁸) with x⁸+x⁴+x³+x²+1 and generator (x+a)(x+a²)(x+a³). The parity is
transmitted XORed with the data-type mask the standard puts on the air
(0x96 96 96 for a voice LC header, 0x99 99 99 for a terminator with LC), and
the byte order is the one the register produces, not the one the polynomial
reads.

That parity is the piece that cannot be checked in a loopback: a receiver that
is fed by its own transmitter agrees with itself whatever the mask or the byte
order. It was checked against a real radio instead — the recomputed parity of a
captured voice LC header reproduces the two bytes on the air
(`55 7D FE` for one LC, `5A 72 F1` for another) and both have zero syndrome
with the decoder's GF(256).

### CACH

Outbound (repeater) slots start with 24 bits: AT, TC, LS1, LS0 from the
configuration, 17 bits of the empty short LC, three parity bits from the
Hamming(7,4) of `dmr_fec_cach_encode()`. The MS layout has no CACH: on the
inbound channel the CACH belongs to the base station.

### Slot layouts

| layout | what it is | the slot |
|---|---|---|
| `DMR_TX_LAYOUT_MS` | handheld, simplex, what the PTT uses | 6 symbols of guard, 132 symbols of burst, 6 symbols of guard, ramps inside the guard |
| `DMR_TX_LAYOUT_BS` | repeater / base station | 12-symbol CACH, then the burst, continuous carrier, no ramping |

The modulator carries the last 8 symbols of the pulse tail from one slot to the
next, so a continuous carrier has no discontinuity at the slot boundary and a
gated one ramps down inside the guard.

The modulator is integer end to end: Q14 tables, no floating point, no
`exp()`/`cos()`, 180 bytes of RRC table and 96 bytes of ramp.

## The PTT state machine

The stages live in a pure function, `openrtx/include/protocols/DMR/DmrTxFsm.h`
(`dmrTxFsm_next()`), one call per 30 ms slot from `OpMode_DMR::txSlot()`. It
needs no audio stream, no radio and no vocoder, so `tests/unit/dmr_tx_fsm.cpp`
walks every state and every PTT/queue condition on the host.

```
        PTT, encoder available
RX ─────────────────────────────► TX_LEAD ──► TX_LC_HEADER ──► TX_VOICE
        stopSampling()              n slots      1 slot          n slots
        radio_disableRtx()            │                            │ PTT up
        radio_enableTx()              │ PTT up, nothing sent       ▼
                                      ▼               OFF ◄── TX_TAIL ◄── TX_TERMINATOR
                                  (done)             unkey      1 slot       1 slot
```

* `startTx()` refuses if `dmrVocoder_available()` is false: a mode that keys the
  PA and sends three zero frames gets a mouthful of silence on the air and no
  clue on the screen. The PTT simply does nothing instead.
* The receiver is stopped and the radio taken out of RX **before** the PA is
  keyed: the BK4819 is half duplex and both directions share the audio path.
* One empty slot of lead lets the PA settle before the LC header, one empty slot
  of tail carries the last pulse; the PA ramp is a 1 ms raised cosine inside the
  guard of the terminator, so it lands before the tail slot.
* The lead stage waits for a queued frame before opening the call: the LC header
  announces a call and the first voice burst follows it, so keying the header
  with an empty queue would put a header and then silence on the air.
* **One burst every other slot.** A call occupies one of the two timeslots of
  the carrier, and a voice burst carries 60 ms of speech — exactly what the
  vocoder produces in the 60 ms between two bursts of the same timeslot. The
  slot in between is silence (`DMR_TXB_EMPTY`, which drains the pulse tail), not
  a second burst: transmitting in every slot would play the speech at twice its
  speed and trample the station that owns the other slot.
* Voice frames come from a 12-slot ring buffer (`DMR_TX_QUEUE_FRAMES`, four
  bursts of margin) fed by up to two frames per slot from the microphone stream
  (1.5 is the average rate), so the queue absorbs the jitter of an encoder that
  is slower than 20 ms per frame instead of slipping the slot clock. A burst
  always carries three frames; missing ones are zero frames.
* The slot cadence is the blocking write to the 48 kHz output stream: 1440
  samples are exactly 30 ms and the stream is double buffered.

### Leaving transmit: one exit, two timeouts

Leaving is decided by the slot counter first, **not** by the stage: a stage that
only leaves when some condition becomes true can be trapped by a condition that
never does. Both timeouts are in the header and covered by the host test:

| Timeout | Slots | What it protects |
|---|---|---|
| `DMR_TX_MAX_SLOTS_AFTER_PTT` | 4 (120 ms) | PTT released: always ends, whatever the stage, even with an empty queue (the lead stage used to hold the carrier for ever here) |
| `DMR_TX_MAX_LEAD_SLOTS` | 33 (1 s) | PTT held with a dead vocoder/microphone: no infinite unmodulated carrier |

`stopTx()` is the only way out and it is unconditional: it stops the output
stream, terminates the input stream, releases both audio paths and calls
`radio_disableRtx()` itself, so the PA, the TX LED and the BK4819 go back to
receive even if the caller's logic misbehaves. (It used to return early when
`txRunning` was false, leaving half-open streams behind after a failed start.)

### Buffers and the stream API

An audio stream started with `BUF_CIRC_DOUBLE` is given the **whole two-half
buffer** and hands out one half at a time (`blockSamples()` = `bufSize / 2`).
The transmitter has to size its buffers accordingly:

| Stream | Buffer | Half the driver hands out | Needed |
|---|---|---|---|
| MIC → MCU, 8 kHz | `micPcm[2 * 160]` | 160 samples = 20 ms | one AMBE+2 frame |
| MCU → RTX, 48 kHz | `txSamples[2 * 1440]` | 1440 samples = 30 ms | one timeslot |

Getting this wrong is not a small bug: with `txSamples[1440]` the idle half was
720 samples while the renderer copied 1440, so every second slot wrote 1440
bytes past the member — over `frameQueue`, `frameCount` and `txStage`, and past
the end of `dmrMode` into `tetraMode` and `m17Mode` in `.bss` (the linker puts
them next to each other). That was the stuck PA of the first bench test: the
stage became an audio sample, so the machine never reached the terminator, and
the PTT release was never seen. It is also why M17 crashed afterwards: the
write landed on `M17::Modulator::baseband_buffer`, a `std::unique_ptr`, which
`Modulator::init()` then freed.


## The encoder: lib/dmr_encoder

`DmrVocoder.h` is the contract: 160 samples of 8 kHz PCM in, one 72-bit AMBE+2
frame out in the canonical A|B|C order. `dmrVocoder_encode()` is weak and
aliased to a function that returns −1; `dmrVocoder_available()` compares the
addresses to tell "nobody overrode me" from "an encoder is linked in".
`DmrVocoderEncoder.c` overrides it with `dmr_encoder_encode()` when
`CONFIG_DMR_TX_ENC` is set.

### What was wrong with the reference

The only complete encoder available is OpenDMR's (OP25's MBE encoder on top of
Pavel Yazev's fixed-point IMBE analysis). Built as it comes, it does not fit:

| build | instructions per 20 ms frame | share of the M33 (300 MHz, CPI 1.2–1.6) | flash |
|---|---|---|---|
| `-O2` | 4.19 M | 84 – 112 % | — |
| `-O3 -flto` | 2.08 M | 42 – 55 % | 253 KiB |

That measurement also said where the time went: **51 % of it was calls to the
ETSI basic operators of `basicop2.cc`**, each with its own stack frame and its
own copy of the sticky `Overflow`/`Carry` flags, and only 108 k real
multiplications per frame. `-flto` was buying part of the inlining back. And
the Xtensa toolchain available for the HiFi4 is **C only, there is no g++**, so
the port had to be C anyway.

### The port

`lib/dmr_encoder`, five translation units, target-agnostic C99:

| file | what | lines |
|---|---|---|
| `dmr_encoder.c` | firmware-facing contract (one static instance) | 50 |
| `dmr_enc_core.c` | fixed-point analysis: pitch, windows, FFT, V/UV, spectrum | 2 300 |
| `dmr_enc_frame.c` | quantiser, predictor model, Golay, frame packing | 700 |
| `dmr_enc_ambe_tables.c` | the mbelib quantiser tables (generated) | 700 |
| `dmr_enc_trig.c` | cosines and logarithms (generated) | 700 |

What changed with respect to the reference, and why each change is safe:

* **The whole analysis is one translation unit and the ETSI operators are
  `static inline`** (`dmr_enc_basic.h`) instead of 36 external functions. This
  is what `-flto` was doing, without needing `-flto`: 4.19 M → 2.34 M
  instructions before touching the algorithm.
* **The autocorrelation is a 64-bit accumulator** instead of a chain of four
  saturating operators per sample. It is bit-exact: every term is a square, so
  the reference's saturating `L_add` can only clamp at `MAX_32` and stay there
  (the comparison at the end reproduces it); `L_mult` saturates only on
  (−32768)·(−32768) and the signal is the input times a positive Q15 window, so
  it cannot reach −32768; and for a positive value `(2p)>>s == p>>(s-1)`, which
  removes the doubling. 2.34 M → **1.43 M**. The pitch search is three
  `e_p()` calls per frame over ~130 lags each: that loop is 36 % of the frame.
* **No C++**: class members became file scope again (they were file `static`
  before the class existed), `<cstdio>`/`<cstdlib>`/`<cmath>`/WMOPS/`printf`/
  `assert` are gone, `div_s()` clamps instead of calling `abort()`.
* **The Golay codewords are computed** (`golay_code23()`, twelve XOR steps)
  instead of looked up in `ENCODING_TABLE_23127/24128`: 32 KiB of flash fewer,
  checked against both tables for all 4096 codewords.
* **The cosines and the per-`b0` logarithms are const tables**
  (`dmr_enc_trig.c`) generated by evaluating the reference's own expressions
  with the host libm and printing them as hex floats. mbelib builds its cosine
  cache at run time in a thread-local struct; here it is flash. The prediction
  model (`mbe_decodeAmbe2450Parms`) is ported without the amplitudes, the
  phases and the synthesis state, which the encoder never reads.
* **The only libm call left is `log2f`**, once per harmonic per frame. Newlib's
  `log2f` does not return the same bits as glibc's for every input (their
  fingerprints differ), which is why the frame comparison is also run *on the
  target* and not only on the host.
* `-ffp-contract=off` is set for the library: the quantiser is single precision
  and the reference was measured without FMAs.

### Verified

* **Bit-exact against the reference, 1000 frames**, on the host
  (`work/dmr-tx/enc-c/enc_cmp`, the same PCM through `libopendmr` and through
  `lib/dmr_encoder`): 0 frames differ, 0 bit errors. The corpus is 8 kHz speech
  (codec2's `hts1a`, Spanish TTS, Mandarin speech), a 1 kHz tone, digital
  silence and white noise, which exercises the silence path, the unvoiced path
  and the pitch-quantiser clamp.
* **The same 1000 frames on the target** (`enc_arm_cmp`, QEMU mps2-an505,
  arm-none-eabi-gcc `-O2 -ffp-contract=off`, newlib): 0 frames differ, 0 bit
  errors. This is the test that covers newlib's `log2f`.
* **End to end through the transmit chain and the receiver of this same
  firmware** (`dmr_loop` with `-DENCODER_C`): 150/150 voice frames, 52/52 CACH,
  and the decoded PCM is **byte-identical** to the loop that used the reference
  encoder, in both the MS and the BS layouts. SNR and PESQ against the previous
  loop are therefore ∞ / identical by construction: the frames are the same
  frames.

## Deviation calibration (pending)

Two constants set the deviation and both are provisional:

```
DMR_TX_INNER_LEVEL   2731   Q15 amplitude of a +-1 symbol  (DmrTx.h)
DMR_TX_DEVIATION     0x476  REG_40 <11:0>, BK4819 FM sensitivity (radio_C62.cpp)
```

`0x476` comes from M17's `0x582`, which puts its outer symbols at ±2.4 kHz:
`0x582 · 1944/2400 = 0x476`. That is arithmetic, not calibration. The BK4819
drives a varactor whose sensitivity changes per band and per PA, and the C62
stores per-band TX calibration words (`calib_C62`, `txDevNarrow`/`txDevWide`)
with no Hz attached to them.

To calibrate: transmit a +3/−3 alternating pattern (or a DMR test pattern) and
measure the deviation with an analyser or a deviation meter, then scale
`REG_40` until the outer symbol reads 1944 Hz. Do **not** scale
`DMR_TX_INNER_LEVEL` for this: the modulator writes the symbol shape, the chip
sets the FM sensitivity.

One thing to keep in mind while doing it: the RRC peak is
h(0) = 1 − α + 4α/π = 1.0546, not 1 (`scripts/gen_dmr_tx_rrc.py` explains
why), so with the nominal 1944 Hz at the symbol instant the instantaneous
deviation of the waveform peaks at **3306 Hz** — the crest factor of a
four-level signal through a ±4-symbol RRC. TS 102 361-1 caps the peak at
2.5 kHz. Either the calibration is done against the peak (outer symbol at
~1470 Hz, which narrows the spectrum) or the pulse span is widened and the
crest factor drops; that is a bench decision, and it is part of why no
adjacent-channel figure appears below.

## Cost

Measured on QEMU `mps2-an505` (Cortex M33, no DSP extension) with
`work/dmr-tx/arm/build.sh`, which drops a frame marker every slot
(`qemu-mcu-instruction-profiling`):

| slot content | instructions per 30 ms slot | time at 300 MHz, CPI 1.2–1.6 |
|---|---|---|
| voice burst | **117 669** | 0.47 – 0.63 ms |
| LC header / terminator | 122 214 | 0.49 – 0.65 ms |
| empty slot | 1 644 | 7 – 9 µs |
| voice burst, `-O3` | 99 813 | 0.40 – 0.53 ms |
| voice burst, `-Os` | 116 600 | 0.47 – 0.62 ms |

So the transmit chain is **1.6 – 2.1 % of one core**, and the empty-slot path
(half of what a handheld transmits) is 70 times cheaper than a voice slot
because it does not run the interpolator at all.

The encoder, same harness (`work/dmr-tx/enc-c/measure.sh`, Cortex-M33, 100
frames of speech, frame marker between frames):

| build | instructions per 20 ms frame | time at 300 MHz, CPI 1.2 – 1.6 | text of the library |
|---|---|---|---|
| reference `-O2` | 4.19 M | 16.8 – 22.4 ms (84 – 112 %) | 253 KiB |
| reference `-O3 -flto` | 2.08 M | 8.3 – 11.1 ms (42 – 55 %) | 253 KiB |
| **C port `-O2`** | **1 427 272** | **5.7 – 7.6 ms (28 – 38 %)** | **54 808 B** |
| C port `-O3` | 1 352 600 | 5.4 – 7.2 ms (27 – 36 %) | 68 450 B |
| C port `-O3 -flto` | 1 442 043 | 5.8 – 7.7 ms (29 – 38 %) | 68 450 B |
| C port `-Os` | 2 206 867 | 8.8 – 11.8 ms (44 – 59 %) | 45 252 B |

`-O3` gains 5 % of the instructions and costs 13 KiB of flash (it unrolls the
pitch and DCT loops); `-flto` gains nothing now that the analysis is one
translation unit, which is the point of the port. The library is built `-O2`
for that reason.

So: **1.43 M instructions per frame, 2.9× cheaper than the reference at the same
-O2 and 1.5× cheaper than the reference's best `-O3 -flto`**. It fits: a frame
is 5.7 – 7.6 ms of a 20 ms budget on the M33, 28 – 38 % of the core while the
radio also receives (the DMR receiver is 113 k instructions per frame, 0.6 ms).
It is not free — half of one core while talking — but it is a mode and not a
prototype.

Where the remaining 1.43 M goes (per-frame profile, `-O2`, attributed with
addr2line): 36 % the autocorrelation of the pitch search, 11 % the rest of
`pitch_est`, 5 % `div_s` in the 203-candidate normalised-error loop, ~15 %
`cos_fxp` inside the DCTs/IDCTs of the spectrum and the V/UV detector, 3 %
`pitch_ref`, 2 % `memmove` of the 621-sample pitch buffers, 1.4 % the quantiser
(`enc_ambe`).

### And the HiFi4

The sources are target-agnostic C99 and **compile for the `venus_hifi4` core**
with the self-built GNU Xtensa overlay of the `xtensa-hifi4-csk6` skill
(`xtensa-elf-gcc -O2`, vendor newlib headers, no SIMD, no intrinsics): 59 447 B
of code and 5 780 B of state. They were not measured there: with the encoder
inside the M33 budget the DSP is not needed, and the only instruction counter
available for that core is a QEMU build that would have to be taught the
encoder's harness. The estimate is in the same ballpark as the M33 (same C, no
SIMD used), i.e. what the HiFi4 would buy is releasing 30 % of the M33, not
making the encoder possible.

## Memory

Zephyr build of this branch, `-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`:

| build | FLASH | RAM |
|---|---|---|
| `c62/full-aprs` | 488 056 B (46.54 %) | 182 912 B (69.78 %) |
| + DMR mode, `CONFIG_DMR_TX` **off** | 488 088 B (46.55 %) | 182 920 B (69.78 %) |
| + `CONFIG_DMR_TX=y`, `CONFIG_DMR_TX_ENC=n` | 491 236 B (46.85 %) | 186 440 B (71.12 %) |
| + `CONFIG_DMR_TX=y`, `CONFIG_DMR_TX_ENC=y` | **546 892 B (52.16 %)** | **192 224 B (73.33 %)** |
| + as above, after the transmit-buffer fix (`c62/fix-tx`) | **547 924 B (52.25 %)** | **203 696 B (77.70 %)** |
| plain build, no DMR, after the patch | 439 564 B (41.92 %) | 161 384 B (61.56 %) |
| plain build, no DMR, `c62/full-aprs` | 439 532 B (41.92 %) | 161 368 B (61.56 %) |

The transmit-buffer fix costs **+1 032 B of flash and +11 472 B of RAM** over
the numbers of the previous row. The RAM is two corrections and one budget: the
slot buffer must hold two halves (`txSamples[2 * 1440]`, +2880 B) and the
microphone buffer two frames (`micPcm[2 * 160]`, +320 B), the rtx thread stack
has to hold the AMBE+2 encoder (1 KiB → 9 KiB, +8192 B, from the kernel heap)
and the kernel heap itself grows by the same 8192 B so Codec2 keeps the
headroom M17 transmit needs. The flash is the stage machine and the guards of
the M17 modulator.

The transmitter costs **+3 148 B of flash and +3 520 B of RAM** on top of the
receive mode. The RAM is the slot buffer (1440 × 2 = 2880 B at the time — one
half of what it has to be, see "Buffers and the stream API" above), the
microphone buffer (160 × 2 B), the 12-frame queue
(108 B) and the modulator state including the 8-symbol pulse tail (~190 B).
The flash is the modulator and its tables (180 B of RRC, 96 B of ramp) and the
encode halves of the FEC.

The encoder adds **+55 656 B of flash and +5 784 B of RAM** over that: 54 808 B
of code and read-only tables (13 268 B the mbelib quantiser tables, 16 160 B the
cosines and logarithms, the rest the analysis and its windows/quantisers) and
5 770 B of state on the RAM side (the two 621-sample pitch buffers, the FFT
scratch, the predictor history and one frame of parameters). Against the
reference's 253 KiB, the 32 KiB of Golay tables that are now twelve XOR steps
and the C++ the linker no longer has to carry explain the difference.

`CONFIG_DMR_TX=n` — the default — leaves the image at the receive-mode size
plus the 32 bytes of the vocoder hook. Without the DMR mode at all the build
grows by **32 B of flash and 16 B of RAM**: the four `rtxStatus_t` fields
(`dmrColourCode`, `dmrTimeslot`, `dmrSourceId`, `dmrTalkgroup`) and their
initialisation, which the settings and the mode share, and which no build can
avoid. No DMR code, no TX chain, no slots buffer, no encoder.

The 38 warnings of the DMR build are the same set as the base image: the
encoder compiles clean (`-Wall -Wextra`) and none of the new files appears in
the warning list.

## Verification

### Loopback against the firmware's own receiver

`work/dmr-tx/build/dmr_loop` builds the *firmware* transmitter and the
*firmware* receiver into one process and pushes the baseband back in with the
10 ms blocks the rtx thread uses, after two seconds of silence:

```
$ make FW=../OpenRTX-integ build/dmr_loop
$ ./build/dmr_loop --pcm out/speech8k.wav --layout bs
transmitted      56 slots, 50 voice bursts, 150 AMBE frames, 2 LC bursts
received         52 bursts (50 voice, 2 data)
sync             52/52 bursts with a sync, 0 symbol errors total, min score 1.00
CACH             52 valid (0 of them corrected), 0 invalid
voice frames     150/150 identical, 0 bit errors in 150 frames (BER 0.00e+00)
mean EVM         0.030
colour code      1
data types       VoiceLC=1 Term=1
RESULT OK
```

The MS layout gives the same 150/150 with mean EVM 0.044.  The bursts carry
the sync word of the layout: MS voice/data in the handheld one, BS voice/data in
the repeater one (the receiver searches all four, so the loopback passes either
way — a real repeater only answers the MS word). 150 voice frames,
52 CACH decodes, no bit error anywhere: the transmit chain and the receive
chain of this tree agree bit for bit.

The same loop with **the C encoder in place of the reference**
(`make -f` `work/dmr-tx/Makefile` with `-DENCODER_C`, see
`enc-c/`): the statistics are identical and the two 16-bit WAV files it writes
(`out/loop_bs.wav`, `out/loop_ms.wav`) are **byte-identical** to the ones the
reference encoder produces. Bit-exact AMBE frames in, bit-identical PCM out.

### The encoder against the reference

`work/dmr-tx/enc-c/enc_cmp` encodes the same 1000 frames (20 s: speech, a tone,
silence and noise) with `libopendmr` and with `lib/dmr_encoder` and compares
the 72 bits frame by frame:

```
$ ./enc_cmp --ref speech1000.pcm r.ref
$ ./enc_cmp --c   speech1000.pcm r.c
$ ./enc_cmp --cmp r.ref r.c
frames 1000, differing 0, bit errors 0
```

and the same test **on the Cortex-M33** (`enc_arm_cmp`, newlib, VFP, `-O2`,
`-ffp-contract=off`), which is the one that also covers the target's `log2f`:

```
arm: frames 1000, differing 0, bit errors 0
```

The three bugs that the port had and the comparison found, in case anybody
touches it again: the gain quantiser subtracts a constant 1.0 (the reference's
`d_gain_adjust`), the B block is `(golay23 << 1) ^ prng` and not `golay23 ^
prng`, and mbelib's PRBA cosine table is indexed 1-based while the encoder's DCT
one is 0-based. All three change the bits and none of them changes the sound,
which is why only a frame comparison catches them.

### BER against noise

`make ber` (`build/dmr_ber`), **450 frames per point** (150 bursts × 3), Eb/N0
added to the 48 kHz baseband:

| Eb/N0 | BS frames in error | BS BER | MS frames in error | MS BER |
|---|---|---|---|---|
| 20 dB | 0/450 | 0 | 0/450 | 0 |
| 15 dB | 0/450 | 0 | 0/450 | 0 |
| 12 dB | 0/450 | 0 | 0/450 | 0 |
| 10 dB | 0/450 | 0 | 2/450 | 0.6 × 10⁻⁴ |
| 9 dB | 0/450 | 0 | 5/450 | 1.5 × 10⁻⁴ |
| 8 dB | 5/450 | 1.5 × 10⁻⁴ | 8/450 | 2.5 × 10⁻⁴ |
| 7 dB | 6/450 | 1.9 × 10⁻⁴ | 8/450 | 2.8 × 10⁻⁴ |
| 6 dB | 22/450 | 7.7 × 10⁻⁴ | 20/450 | 6.8 × 10⁻⁴ |
| 5 dB | 32/450 | 1.0 × 10⁻³ | 40/450 | 1.6 × 10⁻³ |
| 4 dB | 47/450 | 1.9 × 10⁻³ | 60/450 | 2.4 × 10⁻³ |

Error free down to 10 dB (BS) and 12 dB (MS). The MS layout is 1–2 dB worse
because half of every slot is guard: the receiver sees no signal there and its
level/timing estimates start over.

### Bugs the loopback could not have caught

1. **Sync symbols written as dibit 0 instead of 1.** The symbol-to-dibit map
   put −3 on dibit 0, so bytes 0,0 went out for the −3 symbols of the sync
   word. The receiver correlates the sync by sign, so it still locked: only the
   sync field, which is the level and timing reference of the whole burst, sat
   two levels away from where it belonged — the level estimate then came out
   wrong and the EVM of every burst was poor.
2. **RS(12,9) parity order and mask.** The parity bytes leave the shift
   register as `par[2], par[1], par[0]` and the standard transmits them XORed
   with a data-type mask (0x96 for a voice LC header, 0x99 for a terminator
   with LC). Sending them in polynomial order and unmasked passes every
   loopback and fails every real radio, because a receiver fed by its own
   transmitter agrees with itself. This one was settled against captured
   traffic: the recomputed parity of a real voice LC header is byte-identical
   to what was on the air, syndrome zero.
3. **Sync word picked regardless of the layout.** The modulator always put the
   MS voice/data sync on the air, including in the repeater layout where a base
   station sends its own. The firmware receiver searches all four words, so the
   loopback passed either way; over the air a repeater ignores the wrong one.
   `syncWord()` now follows `cfg->layout`.

### And one the loopback has no opinion on

The state machine as first written sent a voice burst in **every** 30 ms slot and
took one frame per slot from the vocoder. A burst carries 60 ms of speech, so
that plays the speech at twice its speed, and the queue drains three frames per
burst while the microphone fills it with one — after six slots the bursts are
half empty. The loopback cannot see it: it renders a list of slots, it does not
run the PTT. The fix is the scheduling described above, a burst every other
slot and up to two frames taken per slot, and the check that goes with it: a
burst only dequeues frames when it is actually a voice burst, not on the empty
slot in between.

## Spectrum: what is measured and what is not

`work/dmr-tx/scripts/acp.py` measures the baseband file DmrTx writes, which is
the instantaneous frequency deviation of the carrier:

| quantity | MS | BS |
|---|---|---|
| modulating 99 % bandwidth | 2.67 kHz | 2.64 kHz |
| power in the ±12.5 kHz band | −41.7 dBc | −39.1 dBc |
| power in the ±25 kHz band | −49.9 dBc | −47.2 dBc |
| implied deviation (±3 symbol / peak) | 1944 / 3306 Hz | 1944 / 3399 Hz |

The 2.6-2.7 kHz matches the design (2400 Bd through an α=0.2 RRC is 2.88 kHz
wide). The band ratios are the *modulating waveform's*, not an adjacent channel
power measurement: an earlier version of the script built the ideal FM signal
`exp(j2π∫f dt)` and integrated its spectrum in the same bands, and it reported
about −40 dBc at 12.5 kHz — 20 dB from the −60 dBc of clause 10.2.2 — even when
fed a perfectly clean RRC pulse, because the linear FM model has no PA, no
varactor, no chip filter and no analyser in it. That number has been removed
rather than quoted: it reads as a verdict and it is not one.

Two open items come out of the same measurements and belong to the bench:

* **There is no TX splatter filter in the path.** `radio_C62.cpp` puts the DMR
  mode on `BK4819_AUDIO_BYPASS`, which clears the AF TX filters (REG_2B/31/47/
  4B/7E) because a 4FSK baseband that starts at DC cannot go through them, and
  only the 3 kHz LPF2 field of REG_43 is programmed. Whatever the chip's own
  TX filter would have done to the skirt is therefore not being done.
* **The pulse is short on purpose and it shows.** ±4 symbols of RRC gives the
  waveform a far-out skirt: against a ±16-symbol RRC of the same data the
  baseband power above 12.5 kHz drops by 28 dB. Widening the span costs
  nothing but table space (10 phases × 2·span+1 taps) and no instructions.

## Limitations

* **No deviation calibration.** `REG_40 = 0x476` is arithmetic from the M17
  value, and the resulting peak deviation (3306 Hz) exceeds the 2.5 kHz the
  standard allows.
* **No adjacent channel power measurement.** See above; the bench decides.
* **TX only, no talkaround.** The mode transmits in the selected timeslot and
  does not listen to the other one: a handheld cannot hear itself or another
  station while it is keyed (the BK4819 is half duplex anyway).
* **The encoder is half of the core while it runs.** 1.43 M instructions per
  20 ms frame is 28 – 38 % of the M33; it fits, and the receiver keeps up, but
  it is the largest single load in the firmware. Moving it to the HiFi4 is now
  an optimisation, not a prerequisite.
* **No call setup beyond "group call to a talkgroup"**: FLCO 0x00 only, no
  unit-to-unit (0x03) selection in the UI, no late entry, no emergency.
* **No hang time.** The terminator goes out on the slot after the PTT opens;
  a real radio would wait a few hundred ms to bridge the pauses in speech.
* **No power control or PA protection tied to the mode**: `radio_enableTx()`
  does what the FM path does.
* **Colour code, source ID, talkgroup and timeslot are `rtxStatus_t` fields
  with fixed defaults** (CC 1, TS 1, ID 1234567, TG 9) — there is no DMR
  settings screen yet.

## What comes next, in order

1. **Bench: deviation and spectrum.** Calibrate `REG_40` against 1944 Hz, look
   at the skirt with the analyser, decide whether the RRC span and/or the
   BK4819 TX filter has to change. `work/dmr-tx/build/dmr_tx` writes the exact
   baseband the firmware writes and can be played into the radio's audio path.
2. **Bench: the encoder in the loop.** With the encoder linked, a PTT in DMR
   mode now really transmits. The deviation measurement is the same one, done
   with speech instead of a test pattern.
3. **On-air test** against a repeater, and the receive-side RS(12,9) decode
   that `docs/DMR_RX.md` still lists as missing — with the encoder in, the two
   halves can be tested against each other and against real traffic.
4. **UI and settings**: DMR screen with CC/TG/ID/timeslot, call state, and hang
   time on the PTT.
5. **Optional: the encoder in the HiFi4**, to give the M33 back its 30 % while
   transmitting. The C sources already compile for `venus_hifi4`; what is
   missing is the LSF service and the packaging, not the codec.
