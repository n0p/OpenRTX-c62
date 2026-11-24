# NXDN receive mode

NXDN (NXDN Forum TS 1-A/1-B, JVC Kenwood + Icom) receiver for the Retevis C62:
4FSK demodulation of the radio's discriminator output, frame sync, LICH, the
SACCH superframe with its layer 3 messages and voice through the AMBE+2
decoder. Receive only: there is no NXDN transmitter in this firmware.

The chain was validated on two real off-air captures before anything was
integrated, against `dsd-neo` as the reference decoder. This document is the
specification, the evidence, the cost and the hooks the UI needs.

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_NXDN_RX=y
```

`CONFIG_C62_NXDN_RX` (`platform/targets/c62/Kconfig.board`) selects
`CONFIG_C62_DMR_VOCODER` and adds the protocol sources, `OpMode_NXDN` and
`CONFIG_NXDN`. **The UI has not been changed by this work**: the mode list in
`openrtx/src/ui/default/ui.c` (the array around line 1106 that already holds
`OPMODE_TETRA` and `OPMODE_DMR`) has to gain `OPMODE_NXDN`, and the mode label
and the two values the mode screen shows have to be added there. Until then
the mode is compiled but unreachable from the keypad.

`OPMODE_NXDN = 5` is the only enum value this work adds (`rtx.h`); `rtx.cpp`
has the instance, the selector case and `opmode_canTransmit()` returning false
for it. `radio_C62.cpp` configures the BK4819 for it.

## 1. Air interface

### Symbol rate, modulation, pulse shape

| | NXDN48 | NXDN96 |
|---|---|---|
| bits/s | 4800 | 9600 |
| symbols/s | **2400** | 4800 |
| bits/symbol | 2 (4FSK) | 2 |
| frame | 192 symbols = 80 ms | 192 symbols = 40 ms |
| channel | 6.25 kHz (2 per 12.5 kHz) or 12.5 kHz | 12.5 kHz |
| deviation, outer/inner | ±350 / ±116.7 Hz (6.25 kHz), ±1050 / ±350 Hz (12.5 kHz) | ±1050 / ±350 Hz |

4FSK with a root-raised-cosine of roll-off 0.2 (TS 1-A 3.4/3.5; ETSI TS 102 658
clause 14.2.2.2 for the dPMR twin). The transmit pulse is that RRC **times the
sinc of a symbol held for its period**, so a receiver that is the exact inverse
sees a clean four-level eye, and any other receiver sees the inner symbols
pulled towards the outer ones. `dsd-neo`'s filter note (`src/dsp/dsd_filters.c`)
is the clearest public statement of this and is what this receiver's sync
detector was written around.

**This work implements NXDN48 (2400 symbols/s).** That is the mode used on
6.25 kHz channels and on most 12.5 kHz Kenwood NEXEDGE traffic. NXDN96 is the
same frame at twice the symbol rate: `NXDN_SPS` in `NxdnPhy.c` is the only
constant that would change (10 → 5), plus the frame timing, and it is not
verified here because no NXDN96 capture was decoded fully enough to prove it
(see §5).

### Frame

192 symbols, 384 bits. The LICH and everything after it is scrambled; the FSW is
not.

| symbols | field | bits |
|---|---|---|
| 0–9 | frame sync word (FSW), `CDF59` | 20 |
| 10–17 | LICH | 16 |
| 18–47 | SACCH | 60 |
| 48–191 | VCH 1 … VCH 4 (four voice channels of 20 ms) | 4 × 72 |

* **FSW** (TS 1-A 4.4.4, Table 4.4-2): 10 symbols `-3 +1 -3 +3 -3 -3 +3 +3 -1
  +3`, hex `CDF59` read two bits per symbol. Sum zero, so a carrier offset does
  not bias the correlation. There is one FSW for every channel type — no
  separate voice and data words as in DMR.
* **Preamble** (4.4.3): `5775FD`, a repeat of `+3 +3 -3 -3`; only the first
  frame of a transmission carries it.
* **Scrambler** (4.6): a 9-bit LFSR, X⁹ + X⁴ + 1, default seed `0b011100100`,
  one output bit per symbol, inverting the *sign* of the symbol. It runs over
  the 182 payload dibits of every frame, LICH included. Only the default seed is
  implemented; a non-default one comes from a layer 3 message and is not
  supported (it would show as a LICH parity failure).
* **LICH** (5.2.1, Table 5.2-1; 4.5.3, Figure 4.5-7): 7 control bits plus one
  even parity bit over the 4 MSBs, each coding bit sent as one dibit whose
  **sign** is the data bit (so an inverted discriminator inverts the LICH).
  Bits [6:5] RF channel type, [4:3] functional channel, [2:1] option (steal
  flag), [0] direction.

| RF type | meaning |
|---|---|
| `00` | RCCH (control channel) |
| `01` | RTCH (traffic, trunked) |
| `10` | RDCH (traffic, conventional / direct) |
| `11` | RTCH_C (composite control) |

| USC | meaning, on a traffic channel |
|---|---|
| `00` | SACCH, non-superframe (a call's first and last frame) |
| `01` | UDCH (user data) |
| `10` | SACCH, superframe (voice frames) |
| `11` | SACCH superframe with an idle FACCH1 |

Steal flag `11` = no steal (all four VCH are voice), `01` = VCH1/VCH2 are
FACCH1, `10` = VCH3/VCH4, `00` = both halves are FACCH1. On the capture the
LICH decodes to `0x57` = RDCH outbound, SACCH superframe, no steal, which is
exactly the four-VCH voice frame this receiver expects.

### Superframe

A voice call's first and last frames are `SACCH non-superframe`; everything in
between is a superframe of **four frames** (TS 1-A 4.1.1, Figure 4.1-1; TS 1-B
4.1.1). The SACCH is the only channel with the superframe structure: each frame
carries one 18-bit segment of a 72-bit layer 3 message, and the segment's
position is announced in the 2-bit SF field at the head of the SR byte.

At 9600 bit/s with the half-rate vocoder the frame alternates voice and FACCH1
(TS 1-B 4.1.2.1), which is why the steal flag exists.

## 2. Channel coding

### Voice channel: none of ours

**A VCH is 72 bits of AMBE+2 at 3600 bit/s, and those 72 bits already are one
complete AMBE+2 3600x2450 frame** — 12 + 12 + 25 information bits protected by
Golay(24,12) and Golay(23,12) inside the vocoder. TS 1-A 4.5.2 says it
outright: *"Channel coding on VCH is described in another section"*, and that
section is the vocoder specification, not the air interface. So:

* there is **no outer FEC, no interleaving and no CRC on NXDN voice**, unlike
  DMR where the 72 bits are spread over the burst;
* the 36 dibits of a VCH sit contiguously, in order, one channel per 20 ms;
* the 72 bits go to `ambe_fr[4][24]` through **the same schedule DMR uses**
  (`dmr_rw/dmr_rx/dmr_ry/dmr_rz` in `protocols/DMR/DmrDefs.c`, which is
  `dsd_ambe_2450_dibit_map` in `dsd-neo`).

### Does the C62's AMBE+2 decoder serve NXDN as it is?

**Yes, unchanged — and that is the point, not a shortcut.** `lib/dmr_vocoder`
takes nine bytes in the canonical `A(24) | B(23) | C(25)` order and decodes
them. Those nine bytes are the DMR voice frame *after* its burst de-interleave,
and they are produced from the on-air 72 bits by one fixed permutation. NXDN
transmits the same 72 bits with the same permutation and no burst interleave,
so the only difference is *where the 36 dibits come from*:

| | DMR | NXDN |
|---|---|---|
| 72-bit frame | 1/3 of the burst payload, interleaved over the 30 ms slot | one VCH, contiguous |
| bit order into the vocoder | `dmr_fec_voice_deinterleave()` | `nxdnFecVchToAmbe()`, same tables |
| vocoder | `dmr_vocoder_decode()` | `dmr_vocoder_decode()`, same code |

There is no second copy of the vocoder in the image: `CONFIG_C62_NXDN_RX`
selects `CONFIG_C62_DMR_VOCODER` and both modes call the same function.

This is *not* the 3600 bit/s "EFR" vocoder of TS 1-B 4.1.2: that one is 144
bits per 20 ms, used at 9600 bit/s, and it is a different AMBE+2 rate that the
C62 does not have.

### LICH

8 coding bits + 1 even parity bit over the 4 MSBs, each bit sent as one dibit
with the data in the sign (half of the dibit is wasted). No FEC, no
interleaving. The parity is the receiver's first bit-level check on a frame and
it is what settles the *polarity* of the discriminator: the two inner symbols
of the FSW are the ones a real channel distorts, so the negated sync word can
match as well as the true one, and the parity picks the reading (`decode_frame`
tries both).

### SACCH

60 bits per frame, one punctured convolutional block (TS 1-A 4.5.2.1,
Figure 4.5-4):

```
18 bits layer 3 + 8 bits SR                       26
   + 6-bit CRC  (X^6 + X^5 + X^2 + X + 1)          32
   + 4 zero tail bits                              36
   -> rate 1/2, K=5, G1 = 1+D^3+D^4, G2 = 1+D+D^2+D^4   72
   -> punctured 111111 / 110110                    60
   -> interleaved, five rows of 12 read column-wise
```

The decoder de-interleaves, un-punctures (the two erased G2 bits of each group
of six become erasures), runs a hard-decision Viterbi over the 16-state trellis,
and checks the CRC-6 in a register that starts all ones — the reference
decoders' convention, and the one a transmitter has to match.

**There is no Golay code in the SACCH**, and no Reed-Solomon anywhere in NXDN
voice. Golay appears in NXDN only inside the AMBE+2 frame, which is the
vocoder's business.

### FACCH1 / FACCH2 / UDCH

Not implemented. They carry the layer 3 messages that do not fit in the SACCH
(call setup on a control channel, short data, paging), and each has its own
12-bit CRC, puncturing matrix and inter-frame interleaving. See §4.

## 3. Messages

The SACCH carries a 72-bit layer 3 message per superframe. What this firmware
decodes:

| type | name | fields decoded |
|---|---|---|
| `0x01` | VCALL | call type (3 bits), source unit ID (16), destination / talkgroup (16) |
| `0x07` | TX_REL_EX | source, destination |
| `0x08` | TX_REL | source, destination |
| `0x11` | DISC | source, destination |
| `0x35` | ALIAS | recognised, not interpreted (a text alias in 4 segments) |

The RAN is not a field of the message: it rides in bits 2..7 of the 8-bit SR
byte of every SACCH block, and is shown from there.

Layout of a VCALL, from the first bit of the 72 (bit indices in the message):
`0–7` message type, `8–15` call control option, `16–18` call type, `19–23`
voice call option, `24–39` source ID, `40–55` destination, `56–57` cipher type,
`58–63` key ID.

### Are there short data / text messages?

**Yes, NXDN has them, and this receiver does not decode them.** NXDN defines a
Short Data Call (TS 1-C §12, `SDCALL`, "Broadcast" and "Unit to Unit") and a
status call, both carried in **FACCH1** (144 bits, 12-bit CRC, its own
puncturing matrix, interleaved across two frames), and a data call in
**UDCH/FACCH2** (348 bits, 16-bit CRC). The text itself is up to 255 characters
in the data-call PDU. Nothing in the two captures used here carried one — their
SACCH messages are VCALL, TX_REL and ALIAS — so implementing the FACCH1 path
would have been code without a bit-level reference to check it against, and
this is a receive path for voice. It is the next useful piece of work, and the
first thing it needs is a capture of a real short message.

## 4. The chain, as it runs on the radio

```
48 kHz discriminator (BK4819 audio bypass, 12.5 kHz channel, REG_43 = 0x7e48)
  RRC a=0.2 matched filter, 81 taps, evaluated at 24 kHz (10 samples/symbol)
  pre-gate over 11 instants across one whole symbol period
  sync search on the 10-symbol FSW, signs only, both polarities
  per-frame fractional timing: 16 phases of a 4-point Lagrange interpolation
       over +-2.5 samples, metric = sync correlation + 0.35 * payload EVM
  levels: median DC, then 4-means (outer, inner = outer/3) with a drift term
  slice to dibits, descramble (XOR with the mask, plus the polarity)
  LICH -> parity -> polarity and RF/functional/steal fields
  SACCH -> de-interleave, de-puncture, Viterbi, CRC-6 -> 18 bits of L3
  VCH 1..4 -> nxdnFecVchToAmbe() -> 9-byte AMBE+2 frames
  voice -> NxdnSpeech queue -> playback thread -> dmr_vocoder_decode() -> 8 kHz
```

Everything is on the AP. `-DCONFIG_C62_NXDN_RX=y` costs 6 828 B of flash and
15 000 B of RAM over the DMR+APRS build (§6).

The demodulator is the DMR one, and that is not a coincidence: NXDN48 is 2400
symbols/s with RRC 0.2, which is the same symbol rate and the same pulse shape
DMR's chain was built and measured for (81 taps at 24 kHz, ten per symbol,
eight symbols of span). What is new is the frame: one sync word instead of four
slot words, no CACH, and 192 symbols per frame instead of 144 per slot.

Two things were learned the hard way while bringing the capture up and are worth
keeping:

* **The scan's guard has to be in the ring's units.** The ring counts 24 kHz
  samples and the scan counts symbols; comparing them directly lets the scan run
  off the end of the history, and `mf_at()` then returns zeros. The symptom is a
  receiver that finds nothing while every correlation on the same data, taken
  offline, scores 1.0.
* **Match the FSW on signs, not on levels.** Requiring the exact `±3/±1`
  pattern of the sync field rejects real signals: the channel distorts exactly
  the two inner symbols of the field. With signs only the detector finds every
  frame of both captures.

## 5. Captures used, and what they prove

Both are the sigidwiki NXDN examples (`sigidwiki.com/wiki/NXDN`, "I/Q Raw
Recording", `NXDN_IQ.zip`): 48 kHz complex int16, 11.8 s and 15.3 s. They were
converted to the 48 kHz real discriminator the radio hands the mode by
`angle(z[n]·conj(z[n-1]))`, which is what an FM demodulator produces — so the
demodulator works on the same kind of signal in both cases.

The reference is `dsd-neo` v2.13.0 (nightly AppImage) on the same converted
files, with `-Z` for its per-frame trace.

| | NXDN48 capture | NXDN96 capture |
|---|---|---|
| dsd-neo reference | 452 AMBE frames, VCALL Src=901 Dst=0, RAN 01, alias text | 712 AMBE frames, NXDN96 |
| **this chain** | **123 frames, 0 LICH errors, 118 with voice, 466 AMBE frames** | syncs but no voice decoded |
| SACCH | 54 / 123 blocks pass CRC-6, 4 VCALL messages assembled | — |
| polarity | 121 of 123 frames decode inverted (the capture's I/Q convention) | — |

What that pins down:

* **Frame layout and scrambling.** The LICH decodes with **zero parity failures
  on all 123 frames**, to `0x57` = RDCH / SACCH superframe / no steal, the same
  value `dsd-neo` prints. A wrong FSW offset, a wrong scrambler mask or a wrong
  LICH bit order cannot produce that.
* **SACCH, bit for bit.** The assembled VCALL is
  `[01][00][20][03][85][00][00][00][00]` with **Src=901, Dst=0** — identical to
  what `dsd-neo` decodes from the same capture. That covers the de-interleave,
  the un-puncturing, the Viterbi, the CRC-6, the SF position and the layer 3
  layout. Getting any of them wrong leaves the CRC at noise level (0/123); the
  first working combination was found by brute force over the four plausible
  conventions, and only one of them lifts the CRC from 2/123 to 54/123.
* **Voice bit mapping.** Of the 62 frames this chain reports with **zero Golay
  corrections**, 27 are bit-identical, as 49-bit AMBE payloads, to a frame
  `dsd-neo` printed. The remaining 35 differ in the 25 unprotected C bits — no
  FEC protects them, so a single slicer error changes the word while the Golay
  blocks still report zero corrections.
* **Continuity.** 466 AMBE frames decoded over 10.35 s of PCM (the capture
  contains silences; the harness pads the timeline with them).

### What is *not* proven

* **The capture is noisy, and this front end is behind the reference's.** Golay
  corrections per frame: 62 frames at 0, 73 at 1, 113 at 2, 131 at 3, and 87
  above. `dsd-neo` shows `err = [0] [0]` for most frames on the same file. Its
  receiver has the exact inverse-sinc matched filter of §1 and a *soft-decision*
  Viterbi; this one has a plain RRC and hard decisions. Measured consequence:
  44 % of SACCH blocks fail their CRC here where the reference decodes almost
  all of them, and 13 % of voice frames are error-free against most of them
  there. The decoded audio is intelligible but has audible artefacts that the
  number above explains. Closing that gap is the single highest-value
  improvement and it is two changes: build the matched filter as the inverse of
  the transmit pulse, and carry per-dibit reliability into the Viterbi (the
  `match_window`/`energy` numbers are already computed and thrown away).
* **NXDN96 does not decode.** The 15.3 s capture syncs the frame grid but no
  voice: `NXDN_SPS` is 10 in this build. Wrong symbol rate, so this is expected,
  not a defect — but it means the 12.5 kHz / 9600 bit/s combination is
  unverified.
* **6.25 vs 12.5 kHz channel spacing.** Both captures are 12.5 kHz-class signals
  (outer deviation ≈ ±1 kHz). NXDN48 in a 6.25 kHz channel has a third of that
  deviation; the level estimation is automatic, so it should work, but no such
  capture was available and it has not been tried.
* **Nothing has been received on the radio itself.** Every number above comes
  from files, through the same C code the firmware runs. What the BK4819 of a
  real C62 hands over — level, bandwidth, continuity — is what
  `scripts/c62_bbdump.py` captures (`docs/DMR_RX.md`, *Capturing what the radio
  hears*); the NXDN mode takes the same path, so the same tool works unchanged.

Reproduce it:

```sh
cmake -S lib/dmr_vocoder -B build_dmr_vocoder && cmake --build build_dmr_vocoder
cc -O2 -I openrtx/include -I lib/dmr_vocoder tests/platform/c62_nxdn_host.c \
   openrtx/src/protocols/NXDN/{NxdnDefs,NxdnFec,NxdnPhy}.c \
   openrtx/src/protocols/DMR/DmrDefs.c build_dmr_vocoder/libdmr_vocoder.a \
   -lm -o c62_nxdn_host

# whole capture: PCM out, one CSV row per frame, AMBE frames, SACCH messages
./c62_nxdn_host -o out.wav -c frames.csv -a frames.txt -m msgs.txt nxdn48_disc.wav
```

`NXDN_DUMP=1 ./c62_nxdn_host ...` also writes the 60 raw SACCH bits of every
block to `/tmp/sacch_bits.txt` and the Golay error count of every decoded frame
to `/tmp/frames_err.txt`; those are what the two brute-force checks in §5 were
run on.

## 6. Cost

### Instructions (QEMU `mps2-an505`, Cortex-M33, no DSP extension)

`tests/platform/c62_nxdn_phy_cost.c` pushes 12 s of the real NXDN48 capture
through `nxdnPhy_push()` and drops a frame marker every second
(`qemu-mcu-instruction-profiling`, plugin `work/dsp-probe/common/insnframe.c`).
The demodulator only: no vocoder, no audio interface.

| | instructions per second of audio | % of 300 MHz |
|---|---|---|
| this chain, steady state | **13.8 – 14.1 M** | **4.6 – 4.7 %** |
| this chain, mean over the file (silent lead-in included) | 13.38 M | 4.46 % |
| DMR PHY, for reference (`docs/DMR_RX.md`) | 20.2 M | 6.7 % |

It is cheaper than the DMR demodulator on the same kind of signal because the
frame is 192 symbols with one sync word and the scan visits each symbol position
once, where DMR searches four slot words and re-measures five instants per
symbol. The vocoder is the same code and the same cost as DMR's: 751 k
instructions per 20 ms frame, 15-20 % of the core (`docs/DMR_VOCODER.md`).

`-Os` instead of `-O2` (what the Zephyr image uses) was not measured separately;
the DMR equivalent differs by 2 %.

### Flash and RAM

Measured with the Zephyr build (`scripts/c62/build.sh`), same options on both
sides (`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`):

| build | FLASH | RAM |
|---|---|---|
| master, DMR RX + APRS | 518 144 B (49.41 %) | 196 688 B (75.03 %) |
| **+ NXDN RX** | **524 972 B (50.07 %)** | **211 688 B (80.75 %)** |
| difference | **+6 828 B** | **+15 000 B** |

`zephyr.bin` is the figure quoted; the ELF's FLASH region reads the same. The
118 B of slack against the 518 104 B quoted elsewhere is the `git describe`
string the image embeds, which depends on the commit it is built from.

Where the RAM goes:

| object | bytes | note |
|---|---|---|
| PHY ring | 8 192 | 2 048 floats of matched-filter output at 24 kHz; a frame is 1 920 samples, so this is the minimum that lets a whole frame be decoded in one go |
| baseband block | 3 840 | 2 × 960 samples of 48 kHz input, 20 ms |
| frame window | 768 | 192 floats of symbol values |
| speech queue and PCM | ~1 500 | 32 AMBE frames, 2 × 160 samples of PCM, thread stack 8 KiB (the DMR value) |
| everything else | ~700 | status, handler, audio path |

It is 80.75 % of the 256 KB region, i.e. **44 616 B still free**, which is the
number that matters (see the RAM rule in the fork's notes). The ring is the one
large item and it is the price of decoding a whole frame in one call: sampling
each symbol into the frame window as it arrives instead would let the ring drop
to 1 024 floats (4 KiB) and bring the total to ~76 %, at the cost of ~40 lines
in `scan()` and `decode_frame()`. It was left alone because the whole-frame
decode is what the captures above validated.

## 7. What the screen should show

Nothing in `openrtx/src/ui/**` was touched. `nxdn_getStatus()` fills this, and
the UI should read it the way it reads `dmr_getStatus()`:

```c
struct nxdnStatus st;
nxdn_getStatus(&st);
st.sync            // a frame decoded in the last 300 ms
st.voice           // a voice frame in the last second
st.ran             // -1 until a SACCH block decodes
st.rfType, st.usc, st.steal   // LICH fields, for a debug line
st.inverted        // the discriminator is inverted on this signal
st.evm             // EVM of the last frame, per mille
st.message.valid   // a layer 3 message was assembled
st.message.type    // 0x01 VCALL, ... (nxdnFecMsgName())
st.message.source, st.message.dest
st.frames, st.voiceFrames, st.ambeFrames, st.lichErrors
st.sacchChecks, st.sacchOk, st.messages
```

Suggested layout, matching the DMR and TETRA screens (line 2 is the identity
line they all use):

* line 2: `RAN 01` once a SACCH block decodes, `NXDN searching` before that,
  and `NXDN inv` when `inverted` is set (worth showing: it means the signal is
  being received on the wrong sideband of the discriminator and the audio is
  still correct, which is not obvious otherwise).
* line 1: `VCALL 901>0` while a call's message is current, or `Voice e12%` with
  the EVM, or the message type of the last assembled one.
* The green LED follows `nxdnPhy_synced()`, the same as DMR.
* The quick menu has nothing NXDN-specific to add: this is a receive-only mode
  with one frequency.

The squelch is `nxdnPhy_synced()`: audio opens while a frame has decoded in the
last 300 ms, and the playback thread releases the speaker after a second of
silence, so the tail of a transmission is not cut.

## 8. Files

```
openrtx/include/protocols/NXDN/
    NxdnDefs.h    constants, frame layout, FSW, scrambler mask, LICH fields
    NxdnFec.h     descrambler, LICH parity, SACCH block, VCH -> AMBE, L3 parse
    NxdnPhy.h     streaming demodulator interface
    NxdnSpeech.h  voice queue and playback thread
    NxdnStatus.h  what the UI shows
openrtx/src/protocols/NXDN/
    NxdnDefs.c  NxdnFec.c  NxdnPhy.c  NxdnSpeech.c  NxdnStatus.c
openrtx/include/rtx/OpMode_NXDN.hpp
openrtx/src/rtx/OpMode_NXDN.cpp
tests/platform/c62_nxdn_host.c        host harness (PHY + vocoder + PCM out)
tests/platform/c62_nxdn_phy_cost.c    bare-metal cost harness (QEMU mps2-an505)
```

`DmrDefs.c` is compiled into the NXDN build as well: it holds the AMBE+2 2450
dibit schedule that both modes share.

## 9. Sources

* NXDN Forum, **TS 1-A v1.3** (Common Air Interface), Nov 2011 — frame and
  superframe structure (4.1.2, 4.3, 4.4), FSW and preamble (4.4.3–4.4.4),
  channel coding (4.5), scrambler (4.6), LICH (5.2.1).
  <https://www.qsl.net/kb9mwr/projects/dv/nxdn/NXDN-TS-1-A_v0103.pdf>
* NXDN Forum, **TS 1-B v1.3** (Basic Operation) — superframe structure, VCH
  allocation at 4800/9600 bit/s and the EHR/EFR vocoder split (4.1.1, 4.1.2).
  <https://www.qsl.net/kb9mwr/projects/dv/nxdn/NXDN-TS-1-B_v0103.pdf>
* NXDN Forum, **TS 1-C v1.3** (Call Control) — Short Data Call (§12),
  VCALL/VCALL_ASSGN/TX_REL/DISC procedures.
  <https://www.qsl.net/kb9mwr/projects/dv/nxdn/NXDN-TS-1-C_v0103.pdf>
* NXDN Forum, **TS 1-E v1.1** (Common Air Interface, Type-D) — the Icom IDAS
  variant, RTCH2/SCCH framing.
* NXDN Forum white paper, *NXDN vs DMR*, for the deviation and channel figures.
* `dsd-neo` (GPL-3.0), <https://github.com/arancormonk/dsd-neo> — the public
  reference implementation this was checked against: `src/dsp/dsd_filters.c`
  (the inverse-sinc receive pulse), `src/protocol/nxdn/nxdn_{voice,frame,
  deperm,convolution,lfsr,descramble}.c` (VCH mapping, SACCH block, scrambler
  seed, layer 3 layout), `nxdn_const.h` (the interleaver permutations). No code
  was copied: the tables here were re-derived from the specification and the
  results compared bit for bit.
* `dsd`/DSD-FME, OP25 (`rx_sync.cc`, `nxdn` encoder) and SDRTrunk's
  `layer2/LICH.java`, which independently use the same VCH→AMBE+2 schedule,
  the same FSW and the same LICH tables.
* Sigidwiki, *NXDN*, for the captures and the published symbol/deviation
  table: <https://www.sigidwiki.com/wiki/NXDN>
