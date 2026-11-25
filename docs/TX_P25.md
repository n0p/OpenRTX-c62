# P25 Phase 1 transmit mode

> Companion to `docs/RX_P25.md`. The receiver is the reference: every encoder
> here is the inverse of a decoder that was verified against off-air captures,
> and `tests/host/p25_tx` closes the loop through the firmware's own PHY.

P25 Phase 1 (C4FM) transmitter for the Retevis C62: the HDU, the LDU1/LDU2
voice frames, TSBK signalling, the terminator, and the four-level waveform that
carries them. Enabled by `CONFIG_P25_TX` (default **y** with `CONFIG_C62_P25`).

## Status at a glance

| piece | state |
|---|---|
| HDU (RS(36,20) over GF(64), 36 Golay(18,6) words) | done, decoded by dsd-neo |
| LDU1/LDU2 (9 IMBE words, link control, encryption sync) | done, bit-exact through the receiver |
| TSBK (three trellis-coded 196-bit blocks, CRC-16) | done; transmits the Null Information block |
| TDU | done |
| NID (BCH(63,16) NAC + DUID), status symbols, interleaving | done |
| C4FM shaper (RRC α=0.2, 10 samples/symbol, 48 kHz) | done, shared with System Fusion (`protocols/C4FM`) |
| voice (IMBE encoder) | **absent on purpose**, see below: the call goes out as HDU + TSBK + TDU |
| deviation calibration | **pending**: `P25_TX_LEVEL` is a starting point, not a measurement |
| on-air test | **pending**: needs a bench and a real P25 receiver |
| cost | 1.36 M instructions per 180 ms frame, 3.0 – 4.1 % of the M33 |

## What goes on the air

One frame is 180 ms, 864 symbols, 1728 bits at 4800 symbols/s. Four levels,
dibits `01 00 10 11` = `+3 +1 -1 -3`, so the outer deviation is ±1800 Hz
(TIA-102.BAAA 6.3).

```
48 bits   frame sync      0x5575F5FF77FF
64 bits   NID             NAC (12) + DUID (4) + BCH(63,16) parity + parity bit
24 sym    status symbols   fixed dibit 01, one in every 36, never payload
1568 bits payload         laid out by the DUID
```

The 24 status symbols are transmitted as the constant `01` and discarded by
every receiver; the position tables skip them, so nothing else has to know
where they are.

| DUID | payload | coding |
|---|---|---|
| HDU `0x0` | 120 bits: MI (72), MFID (8), ALGID (8), KID (16), TGID (16) | RS(36,20) over GF(64), then each of the 36 symbols as a Golay(18,6) word: 6 information bits + 12 parity |
| LDU1 `0x5` | 9 IMBE words (1296 bits) + link control (240 bits) + 32 zero bits | IMBE: 4 × Golay(23,12) + 3 × Hamming(15,11) + 7 raw bits, PN-scrambled from the first word; link control: RS(24,12) over GF(64) then 24 × Hamming(10,6) |
| LDU2 `0xA` | the same voice, and 240 bits of encryption sync | same; the encryption sync is zeros (the call is clear, ALGID 0x80 says so) |
| TSBK `0xC` | three 196-bit blocks, 12 bytes of data + CRC-16 each | 4-state, rate-½ trellis (TIA-102.BAAA 7.2) |
| TDU `0x3` | none | NID only |

The link control is `LCF 0x00` (group voice user), `MFID 0x00`, service
options, a reserved octet, the talkgroup and the 24-bit source address. The
talkgroup is read back by dsd-neo as "Group 1 Source 1193046".

## Voice, and why there is none

P25 Phase 1 voice is IMBE, and there is no free encoder for it:

* `mbelib` / `mbelib-neo` and the `op25` vocoder are **decoders**. Both are
  built from the same analysis of the DVSI AMBE/IMBE bit stream; neither has an
  encoder half, and the encoder is what the patents and the DVSI licence cover.
* Writing one would mean reimplementing the IMBE analysis (pitch, V/UV
  decisions, the 88-parameter quantiser) from scratch against a codec whose
  specification is not public — months of work, and still encumbered.

So the transmitter sends what it can send legally and correctly: **HDU, then
one TSBK frame every 180 ms, then TDU on the PTT release**. That is a valid,
decodable P25 transmission with no voice in it — a receiver locks the NAC, shows
the talkgroup and the source, and plays silence. It is what the firmware keys
when `CONFIG_C62_P25_VOCODER` is off on the receive side too.

A platform that has an encoder installs it with `p25Tx_setVoiceSource()`: one
call per LDU asking for 11 bytes of the 88 IMBE parameters, packed MSB first —
the layout the receiver's vocoder takes. With it, LDU1/LDU2 go out instead of
the TSBKs and the call is a normal voice call.

Note the difference from DMR (`docs/DMR_TX.md`), which does speak: AMBE+2 has a
public C encoder (the OpenDMR one, `lib/dmr_encoder`), IMBE does not.

## Deviation: a starting point, not a measurement

`OpMode_P25.cpp` hands the shaper

```
P25_TX_LEVEL = 23000 * 0.3165 * 1800 / 2400
```

i.e. the DMR transmitter's scale (`1944 Hz` for the same symbol level) rescaled
to the P25 outer level of 1800 Hz. What that number actually means is the
transceiver's `REG_40` deviation setting, and it has **not** been measured:

1. put the radio in P25 TX with an idle call (no voice source) and a deviation
   meter or a service monitor on the antenna port;
2. measure the outer symbols (`+3`), which are 3× the inner ones;
3. scale `P25_TX_LEVEL` by (measured / 1800) and repeat until it lands;
4. then check the eye on a receiver: an over-deviated C4FM signal is mis-sliced
   by both the firmware's own PHY and dsd-neo.

The host harness uses half that level for the same reason: the RRC pulse peaks
at about 1.8× the outer symbol level, so a signal whose peak sits at full scale
clips in the modulator and is sliced wrongly downstream.

## Verification

`tests/host/p25_tx` (build: `make`, run `./build/p25_tx_test fec|loop|idle|sweep|wave`):

| check | result |
|---|---|
| FEC round-trips (BCH, Golay(23,12)/Hamming(15,11), RS(64), Golay(18,6), trellis, CRC-16) | `fec: ok (0 failures)` |
| `loop` at 60 dB SNR, 20 frames | `frames=20/20 imbe=0/162 bad hdu=1(0 bad) tdu=1 sync_err=0 bch=0`: every IMBE word of the call bit-exact, no NAC/DUID/LC errors |
| `sweep`, BER against SNR | clean from 16 dB up (20/20 frames, 0/162 words); at 12 dB 19/20 frames and 5 words flagged by the receiver's own Golay validity bit |
| `wave` into dsd-neo 2.x (`-f1`) | NAC 293, HDU, LDU1/LDU2 and TDU; "Group Voice Channel User - Group 1 Source 1193046"; **Total audio errors: 0** |

The 60 dB loop is the closed-loop check: the transmitter's bits go through
`P25Phy` (the firmware's own demodulator and FEC) and are compared word by word
against what was sent, so a mismatch on either side shows up as an IMBE word
that differs.

Writing the transmitter is what exposed a real defect in the **receiver**: the
HDU path was decoding its 18-bit words as a truncated Golay(23,12) instead of
the (18,6) code of TIA-102.BAAC, so the header always came out as garbage
(ALGID and KID wrong). It now reports ALGID `0x80` and KID `0` from real
captures as well as from this transmitter.

## Cost

`tests/platform/c62_c4fm_tx_cost.sh p25 [frames]`, QEMU `mps2-an505` (Cortex-M33
as the CSK6011B), instructions counted by the TCG plugin in
`tests/host/dmr_enc_qemu/insn_count.c`. `VOICE=1` installs a voice source, so
the frame timed carries nine IMBE words.

| | instructions per 180 ms frame | per second of audio | 300 MHz, CPI 1.2 | 300 MHz, CPI 1.6 |
|---|---|---|---|---|
| LDU, null voice | 1 358 782 | 7.55 M | 3.0 % of a core | 4.0 % |
| LDU, voice packed | 1 391 089 | 7.73 M | 3.1 % | 4.1 % |

One 180 ms frame takes 5.4 – 7.4 ms of a 300 MHz core, and the call is pumped
in 30 ms slots (`TX_SLOT_SAMPLES`), so even the worst figure leaves 4× headroom
on the slot it has to fill. The IMBE packing is 0.03 % of it; what costs is the
FEC and the shaper (the RRC convolution is 81 taps × 8640 samples).

## Image size

The numbers below are `zephyr.bin` and the Zephyr memory report for the same
worktree, only the Kconfig options changing (build with the `openrtx-c62`
container, `c62-build`):

| configuration | FLASH | RAM | PSRAM |
|---|---|---|---|
| `C62_DMR_RX` + `C62_APRS` (the branch reference) | 566 968 B (54.1 %) | 179 504 B (68.5 %) | 435 980 B (23.8 %) |
| + `C62_P25` + `C62_YSF`, both `*_TX` off | 588 552 B (56.1 %) | 181 184 B (69.1 %) | 527 622 B (28.8 %) |
| + `P25_TX` + `YSF_TX` (default) | 594 780 B (56.7 %) | 182 240 B (69.5 %) | 542 986 B (29.6 %) |
| + `DMR_TX` (the AMBE+2 encoder, for a speaking YSF TX) | 656 012 B (62.6 %) | 195 312 B (74.5 %) | 542 986 B (29.6 %) |

Both transmitters together cost **6.2 KB of flash, 1 KB of RAM and 15 KB of
PSRAM** over the receiving modes; most of the PSRAM is the per-frame bit buffer
(1728 bytes here, one byte per bit) and the 8640-sample output block. The last
row is not a transmitter: it is the AMBE+2 encoder that System Fusion needs in
order to speak (`-DCONFIG_DMR_TX=y`), 61 KB of flash and 13 KB of RAM.

## Limits

* **No voice**: see above. The call is header + Null Information TSBKs +
  terminator.
* **No trunking**: the mode takes one frequency; nothing follows a control
  channel. The TSBK the transmitter sends is a Null Information block, not a
  grant.
* **No encryption**: ALGID `0x80`, KID 0, MI all zeros.
* **No in-call signalling**: the TSBK block is fixed by `p25Tx_setTsbk()`
  before the call; there is no API to change it while transmitting.
* **NAC is the compiled default** (`0x293`) until the UI carries a per-channel
  one.
* **Deviation uncalibrated**, see above; there has been no on-air test.
* The waveform peaks at half of full scale on purpose. That is 6 dB of
  transmit level given away to keep the shaper inside its linear range; the
  real fix is the deviation calibration.
