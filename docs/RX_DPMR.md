<!--
SPDX-FileCopyrightText: 2026 OpenRTX Contributors

SPDX-License-Identifier: GPL-3.0-or-later
-->

# dPMR receive mode

dPMR (ETSI TS 102 490 V1.9.1) receiver for the Retevis C62: 4FSK at 2400
symbols/s on a 6,25 kHz channel, 80 ms frames of 384 bits, header and END
frames, the control channel of every frame and voice through the AMBE+2
3600x2450 decoder the DMR mode already links. Transmit with `CONFIG_DPMR_TX`:
`protocols/DPMR/DpmrTx.c` builds the same frames this receiver decodes
(`docs/TX_DPMR.md`).

The mode is on by default only when it is asked for (`CONFIG_C62_DPMR_RX`), and
it is a Kconfig symbol in `platform/targets/c62/Kconfig.board`, because that is
what the C62 build turns into a Kconfig fragment:

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DPMR_RX=y
```

## What dPMR is, and what it is not

The brief for this work said "6,25 kHz, 2 slots of 40 ms". The first half is
right and the second one is not: dPMR is **FDMA**, and clause 4.2.1 of
TS 102 490 says so in the first line of the transmission structure section
("All transmissions are asynchronous, since there is no entity to provide frame
or slot timing"). There are no timeslots, no second slot to listen to and no
40 ms anything: a carrier holds one call at a time.

What the standard does define (clause 4.2.2, "Transmission format"):

```
payload frame   384 bits, 80 ms:  FS2 or CC (24) | CCH (72) | TCH (72) x 4
superframe      4 payload frames, 320 ms
header frame    384 bits:         P (>= 72) | FS1 (48) | HI0 (120) | CC (24) | HI1 (120)
end frame        96 bits:         FS3 (24) | END (72)
```

The 2 slots of 40 ms are DMR's (TS 102 361-1) and NXDN's. Nothing in the dPMR
receiver is structured around them.

The physical layer is clause 12.2: 4FSK, 2400 symbols/s, deviation +-1050 Hz for
the outer symbols and +-350 Hz for the inner ones (h = 0,29), square root raised
cosine with alpha = 0,2 in transmission and, for the receiver, the same filter
plus the inverse sinc of the FM differentiator (clause 12.2.2.2).

## Where the code lives

```
openrtx/include/protocols/DPMR/
    DpmrDefs.h    framing constants, sync words, CCH/HI/END/TCH decoders
    DpmrData.h    the 18 bit slow data field of the CCH (clauses 5.9.1/5.9.2)
    DpmrPhy.h     streaming demodulator interface
    DpmrSpeech.h  voice queue and playback thread
    DpmrPort.h    where the big buffers live (PSRAM on the C62)
openrtx/src/protocols/DPMR/
    DpmrDefs.c  DpmrData.c  DpmrPhy.c  DpmrSpeech.c
openrtx/src/rtx/OpMode_DPMR.cpp
openrtx/include/rtx/OpMode_DPMR.hpp
tests/unit/dpmr_rx.cpp                    coding layer, AMBE mapping, capture
tests/unit/assets/dpmr_air_frame.dis      7 s of the DSDcc capture, the fixture
tests/platform/c62_dpmr_host.c            host harness: capture in, frames out
tests/platform/c62_dpmr_phy_cost.c        bare-metal cost harness (QEMU mps2-an505)
tests/platform/run_dpmr_phy_cost.sh       builds and runs it, as `meson test` does
tests/platform/qemu_mps2/                 its startup and linker script
```

`meson test` runs three of them: the unit test (coding layer and, with
`DPMR_CAPTURE` set, the whole 29,7 s capture), the host harness on the fixture,
and the cost harness under QEMU, which skips itself where `arm-none-eabi-gcc`
or `qemu-system-arm` are missing. The fixture is the second transmission of
the DSDcc sample, header to END: 40 frames, 37 with a valid CCH, 37 voice, the
two END frames, EVM 0.14.

The protocol layer is C and pure: no allocation, no globals in the coding
layer, so the host tests and the firmware run the same code. `OpMode_DPMR` is
C++ because every mode handler is, and does nothing but move buffers.

## The chain, as it runs on the radio

```
48 kHz discriminator (BK4819 audio bypass, 12,5 kHz RX path, 4,5 kHz IF)
  RRC alpha = 0.2 matched filter, 161 taps at 48 kHz, decimated by 5 -> 9,6 kHz
      1,55 M MAC/s: the one large cost of the mode
  4 samples per symbol, 4-point Lagrange interpolation for the timing
  sync search over FS2, FS3 and FS1 on the four phases of the sample grid
  peak hold of 64 samples, then the candidate becomes the frame grid
  levels from the sync run (every sync symbol is an outer one), per frame
  CCH: de-interleave 12x6, descramble, 6 x Hamming(12,8), CRC-7 -> 41 bits
  frame number (0..3) gates the grid: a frame must carry the number expected
  voice: 4 x 72 bit TCH -> 4 x AMBE+2 -> playback thread -> dmr_vocoder_decode()
  header: FS1 -> HI0 (or HI1) -> HT, called ID, own ID, mode, CI
  END: the field is 72 bits = two copies of one 36 bit scrambled block
```

Design points worth their own paragraph, because each one cost a wrong answer
first:

1. **Levels from the sync run, not from the payload.** Every symbol of FS1 to
   FS4 and of the channel code is an outer one (clause 7.1 expands a channel
   code bit c into the di-bit (c, 1), so the second bit of every pair is 1), so
   the first 12 or 24 symbols of a frame measure the +-1050 Hz full scale
   exactly. The slicer then only has to split at +-1/3 of that. This is what
   makes the receiver independent of the deviation the C62 delivers, which is
   about 1,6 kHz where ETSI says 1,05 kHz.
2. **The frame grid is checked by the frame number.** Inside a superframe FN
   runs 0,1,2,3 (table 5.6). A CRC-7 closes on noise one time in 128 per
   alignment and the sweep tries 51, so the CRC alone is not evidence; a frame
   that does not carry the expected FN is dropped.
3. **The END frame is looked for where the next payload frame would be.**
   Clause 4.2.2 puts it there, and without this the END frame is only decoded
   if the acquisition happens to lock on its FS3 first, which on a real capture
   it almost never does: the grid is still expecting payload frames when the
   transmission stops. On the reference capture this one change is the
   difference between none and the six END frames of the capture.
4. **The END scrambler restarts for each 36 bit copy.** Clause 9.6 is explicit:
   "These 36 bits are now repeated and each block of 36 bits is scrambled". A
   receiver that runs one LFSR over the whole 72 bits decodes exactly one half
   of every END frame and, worse, keeps decoding it every second attempt, which
   looks like a working decoder with a curious 50 % rate. With the reset both
   copies close and agree (`dpmrEnd.same`), which is also the gate that keeps
   the 51-alignments sweep from inventing END frames on noise.
5. **Header types and field offsets come from the tables, not from memory.**
   Table 5.11 defines HT 0000..1000 (communication start, connect, disconnect,
   ACK, system request, ACK reply, system delivery, status response, status
   request) and table 5.4 puts two reserved bits between the format field and
   the 11 bit CI. Both were wrong in the first draft; the unit test now builds a
   header with a known CI and checks it comes back.

## The vocoder: it is DMR's AMBE+2 3600x2450, not another one

dPMR voice is 4 x 72 bits of TCH per 80 ms frame, i.e. 3600 bit/s, and each 72
bit group is one AMBE+2 3600x2450 frame, the same frame DMR carries. The
vocoder is therefore `lib/dmr_vocoder`, unchanged, and the only thing dPMR does
differently is the order of the bits on air: the 72 bits travel as four
interleaved streams of 18, canonical bit 18j + k leaving as transmitted bit
4k + j.

That claim is not taken on trust:

* `dpmrFec_voiceFrame()` and the DMR path's
  reference de-interleaver of the test (the DMR four-block rule) produce **the
  same 9 bytes for 2000 random
  72 bit payloads** (unit test). The DMR path is validated against the
  modulator that generated its synthetic capture (300/300 frames identical,
  `docs/DMR_RX.md`), so this is a bit-level identity with a reference, not a
  re-reading of the same table.
* DSDcc, the reference decoder for this capture, applies its DMR AMBE
  de-interleaver tables to the dPMR TCH unchanged (`dpmr.cpp`,
  `processVoiceFrame()`), which is the same statement from the other side.
* On the real capture **408 of 408 AMBE+2 frames decode**, 401 of them to
  non-silent audio, in 8,16 s of 8 kHz PCM: RMS 6602, peak 31128, median
  dominant frequency 500 Hz. The Golay FEC inside the frame corrects 391 bits
  over those 408 frames, 0,96 per frame, which is what a 4FSK signal at
  EVM 0.19 should cost.

A control experiment worth recording as a *weak* one: decoding the same frames
with the interleave transposed gives a higher spectral flatness (0.111 against
0.069) but still recognisable audio, because AMBE+2's parameter layout tolerates
some permutation. The argument rests on the bit-level identity above, not on
this.

## Short data and status

Two different things travel in the 18 bit SLD field of the CCH, and dPMR has no
data frame of its own inside a voice call:

* **Mode 001, voice + slow data (clause 5.9.1).** Two user bytes per frame, each
  behind a continuation flag that marks the last byte of the message.
  `DpmrData.c` decodes the pair and `dpmrSldMessage` reassembles a message
  across frames (32 bytes, truncation flagged). The mode keeps the last complete
  message for the UI.
* **Mode 101, voice + appended type 2 data (clause 5.9.2).** The same 18 bits
  are a header: 5 reserved bits, data position (table 5.9a), format (table
  5.9b), continuation, 6 bit length. The mode decodes the header and keeps
  format and length.
* **Status message (clause 5.15).** Five bits in the END frame, but only when
  the end type is 01 (table 5.12); with ET = 00 they are dummy data and the mode
  reports "no status" rather than a zero.

What is *not* implemented, and would need real work: the **payload** of a type 2
data message lives in the TCH of data superframes (clauses 9.2), and type 3
(packet, ARQ) has its own FS4 header and framing (clauses 5.4 and 9.4). Neither
has a capture here, and the reference capture has no SLD traffic at all (mode
000, SLD all zero), so both decoders are covered by synthetic round-trip tests
only. The status message *was* exercised over the air: the six END frames of
the capture all carry ET = 00, so the gate that ignores the dummy status is what
that evidence exercises.

The 18 bit field does not fit a `uint16_t`. It did, in the first draft, which
silently dropped the top two bits -- the continuation flag of the first byte --
and the synthetic reassembly test is what found it.

## Cost

Measured, not estimated, with `tests/platform/c62_dpmr_phy_cost.c` on a QEMU
`mps2-an505` (Cortex M33, single-precision FPU, no DSP extension), pushing the
reference capture through `dpmrPhy_push()` with a frame marker every second of
audio (`qemu-mcu-instruction-profiling`). The input is the real capture: 20 s,
with the lock/unlock pattern of a real recording, which is what makes the spread
between the two regimes meaningful.

| build | instructions per second of audio | % of 300 MHz |
|---|---|---|
| `-Os`, median of 19 one-second windows | 81,5 M | 27,2 % |
| `-Os`, slowest second (searching) | 108,6 M | 36,2 % |
| `-Os`, fastest second (locked, decoding) | 19,7 M | 6,6 % |
| `-O2`, median | 73,9 M | 24,6 % |

The mode spends most of its budget in the **search**, not in the frames: the
matched filter is 1,55 M MAC/s (about 7,7 M instructions, the same order as the
DMR PHY's whole 20 M) and every unlocked sample runs the sync correlation on
four phases, two patterns and both polarities through the interpolator. When the
grid is locked and frames decode, the cost is 6,6 % -- the DMR PHY costs 6,7 %
in the same conditions, so the frame path is comparable and the acquisition is
where the money goes. Halving it means searching one phase in four and refining
afterwards, which is a change with a measured risk of losing weak frames on
first acquisition; it is not done here.

### Memory

`west build` with the Docker image, `-DCMAKE_BUILD_TYPE` default, image of this
commit:

| build | FLASH | RAM | PSRAM |
|---|---|---|---|
| nothing of DMR/dPMR (`build_none`) | 464 168 B | 174 288 B | 379 064 B |
| DMR RX only, the reference of this branch | 518 176 B | 196 688 B | 379 064 B |
| DMR RX + **dPMR RX** | **525 120 B** | **197 688 B** | **389 848 B** |
| dPMR RX alone | 509 840 B | 183 824 B | 389 848 B |

So the dPMR mode costs **+6 944 B of flash and +1 000 B of RAM** on top of a
DMR build. A build with dPMR and without DMR pays 8 536 B more than that,
because it is the one that brings `lib/dmr_vocoder` in for the voice: `dPMR RX
alone` is 183 824 B, against 174 288 B for an image with neither mode. The
flash is the demodulator and coding layer, the mode handler and the pieces of
`OpMode` glue; the vocoder is already in the DMR image.

The 1 000 B are what is left in SRAM after the dPMR buffers went to PSRAM
(`DpmrPort.h`): the demodulator's counters and framer state, the 161 tap
matched filter history, the small patterns and the mode's own fields. The
buffers that moved, 10 784 B in total:

| buffer | bytes | touched |
|---|---|---|
| baseband block, 2 x 1440 samples (30 ms) | 5 760 | once per rtx update |
| ring of demodulated samples, 1024 floats | 4 096 | written once per symbol, read a frame at a time |
| playback block, 2 x 160 samples (20 ms) | 640 | once per 20 ms |
| AMBE+2 queue, 32 x 9 bytes | 288 | once per 80 ms frame |

The matched filter history (644 B) deliberately stays in SRAM: it is read 161
times per output symbol, 2 400 times a second, and PSRAM wait states there
would land in the innermost loop of the receiver. `.psram_section` is NOLOAD,
so the loader does not zero it and every buffer above is cleared by its own
reset path before it is read (`dpmrPhy_reset()`, `dpmrSpeech_start()`,
`OpMode_DPMR::startSampling()`).

Moving them costs no instructions. The same 20 s of capture through the cost
harness, before and after, built `-O2 -ffp-contract=off` as `meson test` builds
it (the `-Os` figures above are the firmware's own image):

| | before | after |
|---|---|---|
| 20 s of audio, total | 1 265,6 M instructions | 1 265,1 M (-0,03 %) |
| 1 s window, median | 62,6 M/s | 62,6 M/s |
| searching, median (14 windows) | 75,5 M/s | 75,5 M/s |
| locked and decoding, median (5 windows) | 34,0 M/s | 34,0 M/s |

QEMU has no PSRAM and the harness does not define `PLATFORM_C62`, so what that
measures is the restructuring -- the ring left the state struct and became a
global, which is if anything one register cheaper. The cost of the move is
memory traffic: the ring writes 9,6 kB/s and is read about 40 kB/s while
frames are sliced, next to the 384 kB/s the baseband block already moves, on
an AP that runs with its D-cache on (`CONFIG_CSK6_DCACHE=y`, 32 byte lines).
That is why the figure has to be confirmed on the radio: the cache is what
makes the demodulator's sequential ring access cheap, and QEMU models neither
the cache nor the bus.

`-ffp-contract=off` is set for `DpmrPhy.c` in both the firmware and the unit
test on purpose: the slicer and the sync search decide on margins (EVM,
correlation score), and the same file built with FMA contraction decodes 111
frames of the capture instead of 121. A receiver whose frame count
depends on the compiler is not reproducible, and the flag is free here.

## Verification

Everything below is measured on `samples/dpmr.dis` from DSDcc (S16LE 48 kS/s
discriminator output, 29,7 s, French professional system), decoded end to end by
`tests/unit/dpmr_rx.cpp`. One payload frame of the capture sits on the edge of
the timing sweep and moves with the compiler flags (120 frames here, 121 with
the same chain built by hand at `-O2` without `-ffp-contract=off`); the table is
what the test prints. The 29,7 s capture is not in the repository: seven
seconds of it, the second transmission from its header to its END frame, are
(`tests/unit/assets/dpmr_air_frame.dis`), enough for `meson test` to run the
host and the cost harness on real audio; the unit test decodes the whole file
when `DPMR_CAPTURE` points at it and the coding checks run either way.

| metric | value |
|---|---|
| payload frames with a valid CCH | **112** of 112 that reached a CCH (CRC-7 100 %), 120 frames emitted |
| CCH CRC failures | **0** |
| voice frames / AMBE+2 frames | **102** / **408**, all non-silent |
| header frames | **2** (HT 0000 = communication start) |
| called / own address | **0x00393D / 0x000135**, from the header and from the four CCH address slices |
| communication mode | **000** (voice, no user data) |
| channel code | **0x0DD** on every frame that carries one (64 frames) |
| END frames | **6**, each with both copies decoded and in agreement |
| mean EVM | **0.19** |
| sync | 1168 of 1485 20 ms blocks had the squelch open |

* **Coding layer, against vectors.** Hamming (12,8) corrects every single-bit
  error of all 256 codewords and is transparent to the 12 positions
  (unit test); CRC-7 and CRC-8 round-trip through the CCH, HI and END encoders;
  the scrambler reproduces the LFSR DSDcc uses (X^9+X^5+1, preset all ones);
  the channel codes are checked against the 24 bit strings printed in tables
  6.1/6.2 for both groups, which is also what showed the first table to be
  wrong (it read the second bit of each di-bit instead of the first, and assumed
  groups A and B share codes, which they do not).
* **Addresses, twice.** The header frame announces 0x00393D and 0x000135, and
  the CCH of the four frames of the superframe carries those two addresses as
  four 12 bit slices (FN 0/1 the called station, FN 2/3 the own station, clause
  9.1). Both agree, on every superframe: a bit error anywhere in the CCH, the
  de-interleaver or the Hamming decoder would break one of the two.
* **Voice, through the real vocoder.** All 408 frames are decoded by
  `dmr_vocoder_decode()` inside the unit test and required to produce
  non-silent PCM; the test also counts the Golay corrections (391 over 408).
* **Not verified.** The 6,25 kHz RF path of the C62 (the radio runs the
  12,5 kHz BK4819 channel with 4,5 kHz IF filters and lets the RRC filter do the
  selectivity): there is no 6,25 kHz dPMR signal generator here and no second
  radio, so the channel filter, the deviation and the adjacent channel
  behaviour are unmeasured. The capture is 48 kHz discriminator audio, so
  everything from the slicer backwards is exercised and nothing in front of it.
  A transmitter does not exist, so nothing of the transmit side (LBT, header,
  CCH, END) is tested against the standard.
* The 6,25 kHz channel *selection* in the UI is not dPMR-specific: the operator
  stores a channel with mode dPMR on a frequency, and `radio_setOpmode()` gives
  it the 12,5 kHz flat path. Channels with 6,25 kHz spacing work because the
  receiver ignores what the chip cannot filter.

## What the screen can show

`DpmrStatus.c` is what the operator reads: it publishes what each decoded frame
gives and the main screen draws it (ui_mode_state.c), the same shape as NXDN's
`NxdnStatus.c`.

| field | content |
|---|---|
| `sync` | a frame decoded since the mode was entered or retuned |
| `receiving` | the frame grid is locked right now (`dpmrPhy_synced()`) |
| `channelCode` | 12 bit channel code (clause 6.1.5), -1 until a frame that carries it closes its CCH CRC |
| `callType` | `DPMR_CALLTYPE_VCALL` (start header), `TX_REL` (END frame), `DISC` (disconnect request) |
| `source` / `dest` | 24 bit own and called addresses of the header frame, `idsValid` |
| `mode` / `ht` | communication mode (clause 5.7) and header type (clause 5.11) of the last header |
| `endSeen` / `endHasStatus` / `endStatus` / `endWait` | the END frame, its 5 bit status when `ET = 01` (clause 5.15) and the Tx wait (clause 5.14) |
| `evm`, `frames`, `cchErrors`, `voiceFrames`, `ambeFrames`, `headers`, `ends` | what the demodulator measured and how much it decoded |

Line 2 carries the channel code (`CC0DD`, `dPMR SYNC` while it has not been
decoded yet, `RX` while the grid is locked), line 1 the call
(`000135>00393D` in the 24 bit hex dPMR CPSs use) with the EVM and the frame
count -- these take the width only while no header has decoded, because the
call does not give it up -- and the note on the right of line 2 the call event
(`VCALL`, `TX_REL` with its status as `S21`, `DISC`, and the type alone when the
status does not fit). The identifiers stay on screen after the
transmission ends, which is when the END status is worth reading. Nothing
decoded reads `dPMR SRCH`, never a zero.

The `OpMode_DPMR` accessors (`communicationMode()`, `headerType()`, `calledId()`,
`ownId()`, `channelCode()`, `lastStatusMessage()`, `txWait()`,
`slowDataText()`/`slowDataLength()`/`slowDataMessages()`,
`appendedFormat()`/`appendedLength()`, `framesDecoded()`/`headersDecoded()`/
`endsDecoded()`) stay as the diagnostics of the class; the screen no longer
reads them, `DpmrStatus.c` is written from the same frames in `onFrame()`.
`tests/unit/dpmr_status.cpp` builds a call with this tree's transmitter and
affirms both the struct and the texts `ui_mode_state_read()` returns.

Audio is muted unless the last frame arrived less than 400 ms ago
(`dpmrPhy_synced()`), and the playback thread releases the speaker after a
second of silence, the same shape as the TETRA and DMR speech paths.

## What is missing

* The data payload of type 2 (clause 9.2) and the whole of type 3 packet data
  (clauses 5.4, 9.4, ARQ): no capture, no decoder. The SLD header of type 2 is
  decoded so the mode knows a message is coming.
* Short data is reassembled but not delivered to `core/messaging`: there is no
  transport id for dPMR in the message store, and inventing one before the
  format is exercised over the air would be guessing.
* The acquisition cost (27 % of a core at the median) has an obvious next step,
  searching one phase in four.
* A transmitter, if it is ever wanted: LBT (clause 10), the header and the END
  frame are all specified, and none of it exists.
