# System Fusion (C4FM) transmit mode

> Companion to `docs/RX_FUSION.md`. The receiver is the reference: the
> transmitter's FEC, interleavers and scrambling are the exact inverse of
> `YsfFrame.c`, and `tests/host/ysf_tx` closes the loop through the firmware's
> own PHY and frame layer.

Yaesu System Fusion transmitter for the Retevis C62: the header, V/D mode 1
voice frames, the terminator and the four-level waveform that carries them.
Enabled by `CONFIG_YSF_TX` (default **y** with `CONFIG_C62_YSF`).

## Status at a glance

| piece | state |
|---|---|
| frame layer: sync, FICH (Golay(24,12) + K=5 convolutional), DCH (PN(9,5) whitened), 20 × n interleavers | done, 14/14 frames through the receiver |
| V/D mode 1 voice: five slots of 36 CSD + 36 voice dibits | done, 60/60 AMBE+2 slots bit-exact |
| slow data text: 3 × 20 characters | done at frame level; the firmware sends none (no UI for it yet) |
| header and terminator (DG-ID, callsigns, nodes) | done, decoded by dsd-neo |
| C4FM shaper (RC α=0.2, 10 samples/symbol) | done, shared with P25 (`protocols/C4FM`) |
| end-to-end PHY loop | **14/14 frames, 0/60 bad voice words** (after the receiver fix below) |
| dsd-neo | decodes all 22 frames of a 20-frame call: V/D1, `EA5TEST`, `EA5RPT`, `CQCQCQ`, text |
| deviation calibration | **pending**: the level is a measured ratio, not a calibrated deviation |
| on-air test | **pending**: needs a bench and a Fusion radio |
| cost | 756 k instructions per 100 ms frame, 3.0 – 4.0 % of the M33 |

## What goes on the air

One frame is 100 ms, 480 symbols, 960 bits at 4800 symbols/s, four levels at
±1 and ±3 (`10 11` = `+1 +3`, `00 01` = `-1 -3` in Yaesu's mapping).

```
40 bits   sync       3,1,1,0,1,3,0,1,3,0,2,1,1,2,0,3,1,0,3,1
200 bits  FICH       32 information bits + CRC-16, four Golay(24,12) codewords,
                     rate-½ K=5 convolutional code with four tail bits,
                     20 × 5 dibit interleave
720 bits  payload    V/D mode 1: five slots of 72 bits
                     36 CSD dibits (data channel) + 36 voice dibits each
```

The FICH carries the frame indicator (header / communication / terminator), the
call mode, block and frame number, data type, and the DG-ID in place of the
"communication mode", with a CRC-16 over the lot.

The data channel rotates with the frame number, one block per frame:

| frame | block number | content |
|---|---|---|
| header | 0 and 1 | destination + source, then downlink + uplink |
| voice, even `fn` | 0 | destination + source |
| voice, odd `fn` | 1 | uplink + downlink |
| voice, `fn ≥ 2` | 2 | one 20-character fragment of the text message |

The frame number runs 0…4 and repeats, as the mode's superframe does; the text
is therefore repeated every five frames, which is what a receiver needs to pick
it up late.

## Voice

System Fusion has two voice modes. V/D mode 1 carries the **72-bit AMBE+2
channel frame** — the same canonical `A|B|C` order `lib/dmr_encoder` produces
and `lib/dmr_vocoder` consumes — unprotected, five per frame, 360 bits of
speech per 100 ms. It is the mode a Fusion radio uses by default, and it is the
one this transmitter sends.

**Voice FR** (data type 3, full-rate IMBE, 7200 bit/s) is **not** transmitted:
it needs an IMBE encoder, and there is none in this tree — the same licence
story as `docs/TX_P25.md`.

The firmware's voice source is the AMBE+2 encoder the DMR side already carries
(`lib/dmr_encoder`, GPL-3.0+, 1.43 M instructions per 20 ms frame — see
`docs/DMR_TX.md`), read from the microphone at 8 kHz. It comes in with
`CONFIG_DMR_TX` (the platform builds the encoder whenever the DMR transmitter is
enabled; there is no separate switch); without it
`CONFIG_YSF_TX` still builds and keys, and the voice slots go out as zeros — a
valid frame with no speech in it, exactly like the P25 transmitter without an
IMBE encoder. That path is built and linked (`-DCONFIG_C62_YSF=y` alone,
528 888 B of flash), it just does not speak.

## Deviation: measured ratio, not a calibrated deviation

`OpMode_YSF.cpp` hands the shaper

```
YSF_TX_LEVEL = 23000 * 0.3165 * 900 / 2400
```

Half of the level the P25 transmitter uses for its 1800 Hz outer symbols. The
raised-cosine pulse of a four-level signal peaks at about 1.8× the outer symbol
level, so a signal whose peak sits at full scale is mis-sliced by both the
firmware's own level estimator and dsd-neo: with the full level, the peak hits
-1 dBFS and neither receiver slices it cleanly (measured on the host loop).
The halving is what makes the transmitter usable today.

The real number is the transceiver's `REG_40` deviation setting, and it has not
been measured. The calibration recipe is the one in `docs/TX_P25.md`; the
starting point here is 900 Hz of outer deviation, i.e. **half of what a Fusion
radio puts on the air**, and raising it needs the bench measurement, not a
guess.

## The receiver defect this uncovered

The end-to-end loop first decoded only 8 of 14 frames, and every voice word
compared against the wrong frame. The transmitter was fine; the receiver's
sync verification was not:

* `sync_verify()` (the 40-bit check that stops the hunt from locking onto
  noise) used the **median of the 20 sync symbol values as the DC reference**.
  The YSF sync is 13 outer symbols to 7 inner ones, so that median lands on the
  inner level. Every inner *positive* symbol (dibit 0) then reads as an inner
  *negative* one (dibit 2): **exactly 8 of the 40 bits wrong on a perfect
  signal**.
* Consequence: `hunt()` rejected every real sync. The receiver only acquired on
  a chance candidate that happened to score 5-of-40, six frames into the call
  (sample 33 640 instead of 4 840), so frames 0–5 were lost, `fich_bad=1`, the
  text arrived in one block of four, and the voice comparison was offset by
  seven frames — which is why "the voice bits diverge a hundred milliseconds
  into the frame". It was never the transmitter, and never the ring.
* The fix: the DC reference is the **midpoint of the sync's extremes** (the
  pattern carries both outer levels, so they are a symmetric pair), and the
  inner/outer split is the **largest gap of the sorted magnitudes** instead of
  their median, for the same 13/7 reason. Both are in `YsfPhy.c`, with the
  reasoning in the comment.

A second, smaller defect came out of the same loop: after the call ends the
receiver kept coasting on the frame cadence and emitted one garbage frame out
of the silence (FICH failing, 20 of 20 sync symbols wrong). `track()` now
checks the sync score before emitting: a score below a quarter of the symbols
is a carrier that has gone away, not a faded frame, so the receiver drops the
lock and hunts again instead of handing a certain-garbage 100 ms upwards.

## Verification

`tests/host/ysf_tx` (build: `make`):

| check | result |
|---|---|
| `loop 60 12` — a 14-frame call at 60 dB | `frames=14/14 fich_bad=0 voice=0/60 bad text_bad=0`: PASS |
| `loop 60 60` — a 62-frame call | `frames=62/62 voice=0/300 bad`: PASS |
| `loop` at 30 dB and above | 0 bad voice words in 300 |
| `loop` at 24 dB | 8/300 words corrupted (voice is unprotected by design) |
| `loop` at 22 / 20 / 18 dB | 263/295, 202/300 and 276/290 words, and 1–2 frames lost |
| `loop` at 16 / 14 / 12 dB | 41, 15 and 0 of 62 frames: the cliff of an unprotected 9600 bit/s voice mode |
| `wave ysf.wav 20` into dsd-neo (`-fy`) | 22/22 frames: `V/D1`, `SRC: EA5TEST`, `U/L: EA5RPT D/L: EA5RPT`, `DST: CQCQCQ`, and the text tail `BLOQUES DE VEINTE!!` in the slow data |

The 22/22 count is the whole transmission, header to terminator. dsd-neo's
"Total audio errors" counter is high in that run because the harness feeds it
**synthetic random** voice frames, not speech, so its AMBE+2 decoder rejects
them; the frame layer is what the comparison is about.

The loop is the real check: the transmitter's 480 dibits go through
`YsfPhy` + `YsfFrame`, and every field is compared with what was sent — frame
indicator, block and frame number, data type, DG-ID, destination, source, both
nodes, the text reassembled across frames, and all five 72-bit voice slots of
every frame, bit for bit.

## Cost

`tests/platform/c62_c4fm_tx_cost.sh ysf [frames]`, QEMU `mps2-an505`
(Cortex-M33), same counter as `docs/DMR_TX.md`. `VOICE=1` installs a voice
source.

| | instructions per 100 ms frame | per second of audio | 300 MHz, CPI 1.2 | 300 MHz, CPI 1.6 |
|---|---|---|---|---|
| V/D mode 1, null voice | 755 373 | 7.55 M | 3.0 % of a core | 4.0 % |
| V/D mode 1, voice packed | 755 623 | 7.56 M | 3.0 % | 4.0 % |

3.0 – 4.0 ms of a 300 MHz core per 100 ms frame, i.e. 4 % of the time budget
with the voice source in. Almost all of it is the RC convolution (81 taps ×
4800 samples) and the FEC; the AMBE+2 packing is noise in the measurement.
The **encoder** is the expensive part and it lives in the DMR figures: 1.43 M
instructions per 20 ms frame, 4.8 % of the core on its own (`docs/DMR_TX.md`).

## Image size

Same worktree, only Kconfig changing (built with the `openrtx-c62` container):

| configuration | FLASH | RAM | PSRAM |
|---|---|---|---|
| `C62_DMR_RX` + `C62_APRS` (the branch reference) | 566 968 B | 179 504 B | 435 980 B |
| `C62_YSF` alone (RX + TX, no DMR, no AMBE encoder) | 528 888 B | 168 032 B | 446 258 B |
| `C62_P25` + `C62_YSF`, both `*_TX` off | 588 552 B | 181 184 B | 527 622 B |
| + `P25_TX` + `YSF_TX` | 594 780 B | 182 240 B | 542 986 B |
| + `DMR_TX` (the AMBE+2 encoder → the YSF voice path) | 656 012 B | 195 312 B | 542 986 B |

The two transmitters together are 6.2 KB of flash, 1 KB of RAM and 15 KB of
PSRAM over the same image with both off; System Fusion's share of that is the
frame builder plus the shared shaper (the per-mode split has not been measured
separately). The 480-dibit frame buffer, the 4800-sample output block and the
microphone queue all live in PSRAM.

## Enabling it

```
west build -b c62 -d build_c62 . -- \
    -DCONFIG_C62_YSF=y -DCONFIG_YSF_TX=y -DCONFIG_DMR_TX=y
```

`CONFIG_YSF_TX` is off only if the build passes `-DCONFIG_YSF_TX=n`; with it off
the transmitter is not compiled and PTT in System Fusion does nothing. The last
flag is what makes it speak: without it the call goes out with zero voice.

## Limits

* **No Voice FR**: no IMBE encoder, so only V/D mode 1 goes out.
* **The firmware sends no text**: `cfg.text` is `NULL` in `OpMode_YSF`; the
  chain carries 3 × 20 characters and the tests use them, the UI does not.
* **Fixed call fields**: source is `settings.callsign`, destination `CQCQCQ`,
  nodes `DIRECT`, DG-ID 0. There is no UI for any of them yet.
* **No repeater/WIRES-X control**: the frames are what a simplex or a
  transparent repeater carries; there is no DG-ID routing or node control.
* **Deviation is half scale and uncalibrated**, see above.
* **No gating of the PA between frames**: the carrier is continuous from key-up
  to the terminator, which is what the mode wants, but the PA ramp is the
  platform's, not this chain's.
