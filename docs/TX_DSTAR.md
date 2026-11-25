# D-STAR DV transmit mode

> Companion to `docs/RX_DSTAR.md`. The transmitter is the mirror of a receiver
> that was verified against off-air captures; `tests/host/dstar` closes the loop
> through `DStarPhy.c` + `DStarSlowData.c`, and the resulting signal was fed to
> dsd-neo as well.

D-STAR DV transmitter for the Retevis C62: bit sync, frame sync, the FEC'd
header, DV voice frames with their slow-data channel, and the GMSK waveform the
FM path wants. Enabled by `CONFIG_C62_DSTAR` (default **n**).

## Status at a glance

| piece | state |
|---|---|
| bit sync (64 × `10`), frame sync `0x7650`, end pattern | done |
| header: 41 bytes → 660 coded bits (FEC + interleave + scrambler) | done, decoded by the receiver and by dsd-neo |
| DV frames: 72 voice bits + 24 slow-data bits, 21-frame superframe | done |
| slow data: 20-character message, NMEA / D-PRS position, resync word every 21st frame | done |
| GMSK BT 0.5, 4800 bit/s, 10 samples/symbol, 48 kHz out | done |
| voice: AMBE-2020 3600x2400, 72-bit frame | chain done; the **null frame** goes out (no encoder in this tree) |
| 32-bit bit counter | done: a 16-bit one wrapped after 13.6 s and broke the stream |
| deviation calibration | **pending**: the amplitude is a bench item |
| on-air test | **pending**: needs a bench and an Icom radio |
| cost | 6.28 M instructions per second of waveform, 2.5 – 3.3 % of the M33 |

## What goes on the air

```
64 bits   bit sync      0xAAAAAAAA...      (STD-7 4.1.1 a)
15 bits   frame sync    0x7650             (STD-7 4.1.1 b)
660 bits  header        FEC + interleave + scrambler over 41 bytes
N × 96    DV frames     72 voice + 24 slow data
                        the 24-bit resync word replaces the slow field of
                        every 21st frame (0xAAB468)
48 bits   end pattern   32 × "10" + 0x135F (JARL STD-7 4.1.1 d)
```

The header carries the three flag bytes, RPT1, RPT2, UR, MY and the four-byte
suffix, with the CRC the standard puts on it; the slow data carries one 20-byte
message and the GPS/NMEA stream that the receiver already documents.

One second of DV is 50 frames of 20 ms: 4800 bits/s of which 3600 are voice and
1200 slow data.

## Voice

D-STAR voice is AMBE-2020 at 3600 bit/s (the "3600×2400" the Icom radios use),
72 bits per 20 ms frame. `lib/dstar_vocoder` has the **decoder** (a port of
mbelib-neo's D-STAR path, `docs/RX_DSTAR.md`); there is no encoder in this tree
and none is available under a free licence, the same story as IMBE in
`docs/TX_P25.md`.

Without an encoder the chain sends the **D-STAR null frame**, which is exactly
what an Icom radio transmits when the operator is silent. That is not a
placeholder invented here: it is a defined AMBE-2020 frame, so a receiver plays
it as silence, keeps the call open and shows the header and the slow data. The
frame slot is real, the header is real, the on-air behaviour of everything but
the speech is real.

A platform with an encoder installs it with `dstarTx_setVoiceSource()`: 9 bytes
per frame, air order.

## The bit counter

The generator used to keep its position in a 16-bit counter, which wraps after
65 536 bits — **13.6 seconds** of DV. Past that point the frame boundaries moved
and the transmission fell apart; a call longer than a sentence was broken. It
is a 32-bit counter now (`dstar: 32-bit bit counter in the transmitter`), and
the verification below runs 808 frames (77 568 bits, well past the old wrap).

## Verification

`tests/host/dstar/dstar_host.c` (the build line is in its header comment):

| check | result |
|---|---|
| `selftest` — header FEC with 0, 6 and 20 bit errors, slow-data text, GPS | all `ok`, CRC recovers at 20 bit errors |
| `loop 30 400` — transmitter → AWGN 30 dB → `DStarPhy` | 404 frames, `ber=0.00e+00 (0/28800)`, header ok |
| `loop 30 804` — past the 16-bit wrap | 808 frames, `ber=0.00e+00 (0/57888)`, header ok, 39 resyncs |
| dsd-neo on a generated transmission | header and frames decode (`-fd`) |

Every payload bit the receiver compares is a voice bit it re-derived from the
waveform: 57 888 of them in the 808-frame run, none wrong. The header, the
resync words and the slow data are checked by the receiver's own parser, which
is the one verified against captures.

## Deviation

`dstarTx_samples()` takes the amplitude in Q15 full-scale units, like the other
transmitters; on the C62 that is the same `REG_40` deviation register, and the
value used by the tests (24000 of 32768) has **not** been checked with a meter.
D-STAR is GMSK in a 6.25 kHz channel, so the deviation is fixed by the channel
rather than by a symbol alphabet: the calibration is one measurement, see
`docs/TX_P25.md` for the recipe, and the FM path's own gain then applies to
both.

## Cost

`tests/platform/c62_dstar_tx_cost.c` on QEMU's `mps2-an505`, instructions
counted by the plugin in `tests/host/dmr_enc_qemu/insn_count.c`; five seconds of
waveform with the bit generator and the GMSK shaper in the loop:

| | instructions |
|---|---|
| 5 s of modulating waveform | 31 385 576 |
| per second of audio | 6.28 M |
| 300 MHz, CPI 1.2 | 25.1 ms per second = **2.5 % of a core** |
| 300 MHz, CPI 1.6 | 33.5 ms per second = **3.3 %** |

Cheaper than the C4FM transmitters per second of audio (6.3 M against 7.6 M),
because GMSK is one bit per symbol at 4800 baud instead of two, and the
Gaussian filter is 8 taps instead of 81.

## Image size

Same worktree, only Kconfig changing:

| configuration | FLASH | RAM | PSRAM |
|---|---|---|---|
| `C62_DMR_RX` + `C62_APRS` (the branch reference) | 566 968 B (54.1 %) | 179 504 B (68.5 %) | 435 980 B (23.8 %) |
| + `C62_DSTAR` (RX + TX + the AMBE 3600x2400 decoder) | 614 984 B (58.7 %) | 187 792 B (71.6 %) | 453 624 B (24.7 %) |

D-STAR costs **48 KB of flash, 8 KB of RAM and 18 KB of PSRAM**. That row is the
whole mode: `C62_DSTAR` also selects `C62_DSTAR_VOCODER` (the receive-side AMBE
decoder), so the figure covers the decoder as well as the transmitter.

In the firmware, `CONFIG_DSTAR_TX` (default **y** with the mode) is the PTT path
in `OpMode_DSTAR`: MY comes from `settings.callsign`, UR is `CQCQCQ`, and RPT1
and RPT2 are `DIRECT` on a simplex channel or the operator's own callsign when
the channel has a split (there is no repeater database to fill them in
properly). The slow-data text is the queued D-STAR message when
`CONFIG_DSTAR_MESSAGING` is on.

## Limits

* **No voice**: the null frame goes out, which is silence. Header, slow data,
  framing and the modulation are all real.
* **No repeater linking**: the header carries RPT1/RPT2 as configured, but
  nothing does the gateway handshake.
* **Slow data is set at key-up**: the text is taken from `dstarTx_begin()`;
  GPS streaming needs a platform that keeps feeding it.
* **Deviation uncalibrated**, and no on-air test.
* **No DV Fast Data**, no digital squelch (DSQL), no call-sign routing beyond
  what the header carries.
