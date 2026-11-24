# System Fusion (C4FM) receive

Yaesu System Fusion transit at 4800 symbols/s, four levels, 9600 bit/s in a
12.5 kHz channel, framed every 100 ms. This document is the specification the
implementation follows, the evidence it was checked against, and the numbers of
the build that carries it.

Everything below was verified against one off-air recording and against the
encoders/decoders of four independent implementations (op25, MMDVM, dsd-fme,
RYSEN). Where a source disagrees with the recording, the recording wins and the
disagreement is written down.

## 1. Air interface

| Property | Value |
|---|---|
| Modulation | C4FM: 4-level FSK, Nyquist (RRC α ≈ 0.2) shaped |
| Symbol rate | 4800 symbols/s, 2 bits per symbol |
| Channel rate | 9600 bit/s in 12.5 kHz |
| Frame | 100 ms = 480 symbols = 960 bits = 120 bytes |
| Deviation | inner ±1.8 kHz, outer ±5.4 kHz (nominal, 3:1) |
| Sync | 20 symbols, `0xD471C9634D` |
| Voice | AMBE+2 2450 bit/s (49 bits / 20 ms) in V/D modes |

The deviation figure is nominal: the slicer derives the inner and outer level
from the frame itself (§5), so a transmitter that is off or a receiver whose
discriminator gain differs does not break the decode — only the absolute
deviation on our own transmitter would matter, and this mode has no
transmitter.

### 1.1 Symbol mapping

Two encodings of the same dibit appear in the literature and they are negations
of each other. System Fusion and P25 use the *inner-positive* one, which is what
the recording shows:

| Dibit | Deviation | Level |
|---|---|---|
| `00` | +1.8 kHz | +1 |
| `01` | +5.4 kHz | +3 |
| `10` | −1.8 kHz | −1 |
| `11` | −5.4 kHz | −3 |

Sources: op25 `ysf_tx_sb_impl.cc` (`ysf_fs[]` = `{3,1,1,0,1,3,0,1,3,0,2,1,1,2,0,3,1,0,3,1}`,
the dibits of `0xD471C9634D`), Rafe's YSF page (`_LEVEL = {1:3, 0:1, 2:-1, 3:-3}`),
dsd-fme `ysf.c`. MMDVM's `YSFDefines.h` publishes the same 20 symbol values but with **one of
them wrong**: `YSF_SYNC_SYMBOLS_VALUES[14]` is `+3` where the pattern (and its
own explanatory comment two lines above) gives `+1` — dibit `00` of the third
group. The array is only used for a correlation, which is why the typo has
survived; do not copy it.

The polarity of the received signal is *not* inverted on air: correlating the
recording against the pattern as written finds 26 syncs, correlating against its
negation finds none.

### 1.2 Frame layout

```
│◀──────────── 100 ms · 960 bits · 480 symbols ────────────▶│
┌──────────────┬────────────────────┬───────────────────────┐
│ 40 SYNC      │ 200 FICH           │ 720 payload           │
│ 0xD471C9634D │ 32 info + CRC-16   │ depends on DT         │
└──────────────┴────────────────────┴───────────────────────┘
```

### 1.3 FICH

200 transmitted bits carry 32 information bits:

```
32 info │ +CRC-16 → 48 │ 4 × Golay(24,12) → 96 │ +4 tail → conv 1/2, K=5
        → 200 → 20×5 dibit interleave
```

* CRC-16: polynomial `0x1021`, registers start at zero, result inverted, so a
  message carrying its own CRC checks to zero.
* Golay(24,12): the P25 extended Golay, generator `0xC75`, 12 information bits
  first, then 11 parity bits, then one overall parity bit.
* Convolutional code: rate 1/2, K = 5, generators `0o31`/`0o27` applied to
  `reg = (reg << 1) | bit` as masks `0x19`/`0x17`. (This is the same code as
  `0o23`/`0o35` in the other notation; the masks are the bit-reversed pair and
  the combination with the shift direction is what matters. op25's encoder and
  the decoder here round-trip.)
* Interleave: 100 dibits written as 20 rows of 5, read as 5 columns of 20.
  **It interleaves dibits, not bits.** Interleaving the 200 bits as 20×10 is a
  different permutation and decodes nothing: that was the first bug found here.

Field layout, most significant bit first:

| Field | Bits | Meaning |
|---|---|---|
| FI | 0-1 | 0 header, 1 communication, 2 terminator, 3 reserved |
| CS | 2-3 | callsigns assigned |
| CM | 4-5 | 0 group/CQ, 1 radio ID, 2 reserved, 3 private |
| BN | 6-7 | data channel block number |
| BT | 8-9 | data channel block type |
| FN | 10-12 | frame number inside the superframe |
| FT | 13-15 | last frame number |
| — | 16-17 | reserved |
| MR | 18-20 | message/repeater flag |
| VP | 21 | 0 simplex, 1 repeater/VoIP |
| DT | 22-23 | data type, §1.4 |
| ST | 24 | squelch code **disabled** when set |
| SC | 25-31 | squelch code = **DG-ID** |
| CRC | 32-47 | CRC-16 |

### 1.4 Data types (DT)

| DT | Name | Payload |
|---|---|---|
| 0 | V/D mode 1 | 5 × (36 dibits data + 36 dibits voice) |
| 1 | data FR | 10 × 36 dibits of data |
| 2 | V/D mode 2 ("DN") | 5 × (20 dibits data + 52 dibits voice) |
| 3 | voice FR ("wide", VW) | 5 × 72 dibits of full-rate voice |

Note on the names: the mode that carries **voice and data together** is *V/D*
(voice/data), DT 0 and DT 2. **VW / "voice wide" / voice FR** is DT 3 and is
full-rate voice with no data at all. The two are often mixed up; a receiver
that waits for text messages in DT 3 will never see one.

### 1.5 Data channel

Three shapes, all with the same coding chain (whiten, CRC-16, conv 1/2 K=5,
dibit interleave):

| Where | Air bits | Interleave | Info bits | Content |
|---|---|---|---|---|
| FICH | 200 | 20×5 dibits | 32 | frame header |
| V/D 1 DCH | 360 | 20×9 dibits | 160 | 20 characters |
| V/D 2 DCH | 200 | 20×5 dibits | 80 | 10 characters |
| header/terminator | 360 | 20×9 dibits | 160 | 20 characters |

The whitening is PN(9,5): a 9-bit LFSR loaded with `0x1C9`, output the LSB,
feedback `S0 ^ S4` into the top. It covers the **information bits before the
CRC** (op25 `generate_dch()` whitens the 160 bits and then computes the CRC over
them), so a receiver checks the CRC on the raw decoded bits and de-whitens
afterwards. Rafe's page describes whitening the transmitted bits after the
interleaver, which contradicts its own CRC; the recording settles it in favour
of op25/dsd-fme.

Content is selected by BN in V/D mode 1 (0 = destination + source, 1 = uplink +
downlink, 2 = **text message**) and by FN in V/D mode 2, where one transmission
cycles 8 fields:

| FN | Content (observed on air) |
|---|---|
| 0 | destination |
| 1 | source callsign |
| 2 | uplink node |
| 3 | downlink node |
| 4 | remark |
| 5 | station id |
| 6 | radio/model identification block |
| 7 | status block (vendor binary) |

### 1.6 DG-ID

The DG-ID is **not** in the data channel: it is the FICH squelch code, ST +
SC, 7 bits, 0-99 valid. `ST = 0` means "a squelch code is present". DG-ID 00 is
the "everything" setting, which is what the reference transmission carries.

### 1.7 Text messages

A text message rides in the data channel with BN = 2, split into fields: FN is
the index of the field and FT the index of the last one, so a message is
`(FT+1)` fields of 20 characters in V/D mode 1 and 10 characters in V/D mode 2
(80 characters is the longest this firmware keeps). Characters are what the
radio typed, space padded; anything outside 0x20-0x7E is not text.

This is the shape dsd-fme implements (`ysf_dch_decode_text()`, indexed by FN,
bounded by FT) and what the FICH block-number field is for. **It is not
verified against an off-air capture** — see §7.

## 2. Voice: does the AMBE+2 decoder we have serve?

Yes, for both V/D modes, with one small addition to `lib/dmr_vocoder`.

* **V/D mode 1**: each 36-dibit slot expands to 72 bits, the canonical AMBE+2
  channel frame `A(24) | B(23) | C(25)` — Golay(24,12) over the first 12
  parameters, PN-scrambled Golay(23,12) over the next 12 seeded by the first,
  25 unprotected — which is exactly what `dmr_vocoder_decode()` already takes
  and un-FECs for DMR. Nothing to change: the slot goes in as it arrives.
* **V/D mode 2**: 52 dibits → 104 bits, de-interleaved 26×4, de-whitened with
  the same PN(9,5), then 27 parameter bits sent three times each plus 22 sent
  once, plus one fixed zero bit. That yields **49 AMBE parameter bits
  directly**, the same vector `dmr_vocoder_decode()` computes internally. This
  needed a new entry point, `dmr_vocoder_decode_ambe()`, which is the existing
  frame body without the Golay layer: no new tables, no new state, the same
  bit-exact decoder.
* **V/D mode 3 (voice FR) does not serve**: 144 bits per 20 ms is P25
  Phase 1 IMBE at 7200 bit/s, not AMBE+2. Those frames are counted and dropped.

Consequence for the brief's premise: System Fusion's voice is **AMBE+2 at
2450 bit/s** in the voice+data modes, not 3600. 3600 bit/s is DVSI's rate for
other systems, and the only full-rate voice in Fusion is the 7200 bit/s IMBE of
voice FR.

The FEC bits of the 72-bit V/D 1 frame are inside the canonical frame, so no
extra work: `dmr_vocoder_decode()` corrects them and reports the count.

## 3. Reference capture and evidence

| | |
|---|---|
| File | `Yaesu_sys_fusion.wav`, sigidwiki "Yaesu System Fusion" |
| URL | https://www.sigidwiki.com/images/2/26/Yaesu_sys_fusion.wav |
| SHA-256 | `607b9f9789cecc9ab1c7c204f869d10879a0d69b7b4ae60a132694e88d5476e0` |
| Format | 48 kHz, 16 bit, stereo, 9.30 s; channel 0 is discriminator audio |
| Content | one Wires-X repeater QSO: header (`FI=0`), communication frames, terminator |

It is FM-discriminator audio, not IQ: nothing above 3 kHz, both channels equal.
That is what the radio's audio path delivers, so no IQ demodulator is needed.

What the chain decodes from it:

```
frames 83, FICH ok 80 (96 %), FICH bad 3
voice slots 370 of 400 at 49 bits (V/D mode 2), all with the fixed bit clean
DST "*****F0XHI"  SRC "N8KDR-TERY"  U/L "W8USA"  D/L "W8USA"  DG-ID 00
PCM 59 200 samples (7.4 s of speech)
```

* Every frame decodes as **DT = 2** (V/D mode 2), FN cycling 0-7 with FT = 7.
* The three bad frames are single isolated failures inside the transmission
  (sync 19-20 of 20, so the timing was right: real bit errors), plus the
  partial frame at the start of the lock.
* The callsigns are the real ones for that transmission, which is the strongest
  single piece of evidence: 20 characters of `N8KDR-TERY` and the two Wires-X
  node names come out of a 200-bit block protected by CRC + a convolutional
  code + whitening. Random decoding cannot produce them.
* The data channel CRC holds on 77 of 83 frames, and the fixed bit of the V/D 2
  voice slot (bit 103, transmitted as zero) is clean on 370 of the 400 slots,
  which is the structural check on the voice de-interleave and whitening: a wrong
  permutation or whitening fails it half the time by chance.

Comparison with a reference decoder: the frame, FICH and data channel results
were first produced by an independent Python model of the chain written from
op25's encoder (`tools/ysf/ysf_proto.py`), then reproduced by the C code the
radio runs. The two agree frame for frame, which validates the C port; neither
is an independent decoder of the voice payload, so:

**No human has listened to the decoded audio.** What was measured on it: RMS
3888, peak 31 128 (no clipping), spectral centroid 1 148 Hz, 32 % of frames
with a strong pitch period. That is speech-shaped, and it is consistent with
the fixed-bit and FEC evidence, but "intelligible" is not claimed here.

The text message path was exercised the other way round, because no public
capture carries one: `tests/host/ysf_host/txmodel.c` builds a V/D mode 2 frame
with the published encoder (CRC-16, Golay(24,12), rate-1/2 K=5, dibit
interleave, PN(9,5)) and the harness decodes it with the same
`ysfFrame_decode()` the radio runs. Two blocks, `bn = 2`, `fn` 0 and 1 of `ft`
1, reassemble into `"HELLO FROM THE C62"`. That proves the reassembler and the
whole block chain; it does not prove that a real Yaesu puts a text message in
`bn = 2`.

No capture with a text message, no V/D mode 1 and no voice FR transmission was
found in public sources (sigidwiki carries one YSF file). Those paths are
implemented to the specification and tested structurally, not on air — §7.

## 4. Implementation

```
openrtx/src/rtx/OpMode_YSF.cpp          the mode: RX only
openrtx/src/protocols/YSF/YsfPhy.c      4-level slicer, sync, frame clock
openrtx/src/protocols/YSF/YsfFrame.c    FICH, data channel, text, voice slots
openrtx/src/protocols/YSF/YsfSpeech.c   voice queue and playback thread
openrtx/include/protocols/YSF/YsfMem.h  where the buffers live (SRAM or PSRAM)
lib/dmr_vocoder                         + dmr_vocoder_decode_ambe() (49 bits)
                                        + dmr_golay24_correct() (shared table)
tests/host/ysf_host/                    host harness on a capture
tests/platform/c62_ysf_cost.c           QEMU instruction cost
tools/ysf/ysf_proto.py                  Python model of the chain
```

### 4.1 PHY

48 kHz in, 10 samples per symbol exactly. No matched filter: the radio's IF
filter already shapes the eye, and DmrPhy takes the same shortcut.

* Ring of 7360 raw samples (one frame, plus one block of the audio
  stream, plus a sync of margin), in PSRAM — §4.5.
* **Acquisition** correlates the signs of the 20 sync symbols (cheap, level
  independent), then verifies the full 40 bits against levels taken from the
  sync symbols themselves: median of the 20 values is the DC, median of the
  magnitudes splits the inner from the outer cluster. Sign-only acquisition
  false-alarms about once a second and cost 13 frames of the reference capture
  before the full check was added.
* **Tracking** re-correlates the sync at the expected position ±8 samples, and
  coasts on the frame cadence for up to 3 frames.
* **Slicing** is per frame: DC = mean, inner and outer = means of the two halves
  of the magnitude histogram around its median, thresholds at 0 and at
  (inner+outer)/2.
* The frame is emitted as soon as its last symbol is in the ring, from inside
  `ysfPhy_push()` — the ring is deliberately only one frame deep, so a frame
  that is late reads samples that have already wrapped.

### 4.2 Cost

Bare metal on QEMU (mps2-an505, Cortex-M33), TCG plugin, whole reference
capture, per-instruction counters:

| | instructions | per 100 ms frame | share |
|---|---|---|---|
| total (PHY + frame layer) | 43 569 155 / 9.3 s | 524 900 | 100 % |
| Viterbi (FICH + DCH, 2 per frame) | 15 939 351 | 192 000 | 36.6 % |
| symbol interpolation | 11 917 300 | 143 600 | 27.4 % |
| ring fill and frame clock | 8 366 684 | 100 800 | 19.2 % |
| level estimate and slicing | 3 774 903 | 45 500 | 8.7 % |
| FICH/DCH/DG-ID/text | 1 496 821 | 18 000 | 3.4 % |
| Golay(24,12) | 61 376 | 740 | 0.1 % |

4.68 M instructions per second of audio, i.e. **1.9-2.3 % of the M33** at
200-250 MHz, and that figure includes acquiring the carrier from cold (about
7 M of the total are the scan that finds the first sync; a receiver already
locked runs at 3.9 M/s).  The vocoder adds 113 370 instructions per 20 ms
frame (5.7 M/s, 2.3-3.0 %), so a full System Fusion receive costs **about
5 % of the core**, with the voice in its own thread.

The Viterbi is the one place with obvious headroom: it is written as a plain
16-state add-compare-select and the two predecessor branches of each state
share their input bit, so a butterfly structure would halve it for about 90 k
instructions per frame. It was left simple on purpose — 5 % total does not
justify the risk.

### 4.3 Flash and RAM

Measured on the C62 firmware, `west build -b c62`, same tree:

| Build | FLASH | SRAM | SRAM used | PSRAM |
|---|---|---|---|---|
| DMR RX + vocoder (baseline, `-DCONFIG_C62_DMR_RX=y`) | 518 104 B | 196 688 B | 75.0 % | 379 064 B |
| **System Fusion** (`-DCONFIG_C62_YSF=y`) | **509 096 B** | **183 424 B** | **70.0 %** | 418 870 B |
| DMR RX + System Fusion (`…-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_APRS=y`) | **524 380 B** | **197 280 B** | **75.3 %** | 418 870 B |

Against the DMR-RX baseline, System Fusion costs **+6.3 KB of flash and +592 B
of SRAM**: its 39.8 KB of working memory lives in PSRAM, which the combined
build leaves at 22.8 % of its 1.75 MB. The first version of the mode kept all
of it in SRAM — 60 % of it in two buffers — and the combined build then sat at
237 088 B, **90.4 %** of the 256 KB region with 25 KB free: inside, but not
something to add a mode or a table to. §4.5 is what moved.

### 4.4 Where the bytes go

* The Golay(24,12) syndrome table (4 KiB) is **not** duplicated: `YsfFrame.c`
  calls `dmr_golay24_correct()`, which lives next to the table in
  `lib/dmr_vocoder`. The FICH uses the P25 code the vocoder already carried, so
  the shared corrector costs 200 B of code and no table.
* Per object, `arm-zephyr-eabi-size` on the combined build:

| Object | FLASH | SRAM | PSRAM | What the PSRAM holds |
|---|---|---|---|---|
| `YsfPhy.c` | 2 248 B | 508 B | 14 720 B | the raw-sample ring, 7360 samples |
| `YsfFrame.c` | 2 246 B | 1 B | 3 786 B | Viterbi history + `out`/`call`/`text` |
| `YsfSpeech.c` | 878 B | 58 B | 2 104 B | voice queue (20 slots) + PCM scratch |
| `OpMode_YSF.cpp` | 550 B | — | 19 200 B | baseband stream buffer (2 × 4800) |
| **total** | **5 922 B** | **567 B** | **39 810 B** | |

  The rest of the +6.3 KB of flash is the extra `enum opmode` case, the Kconfig
  string and alignment.

### 4.5 What went to PSRAM, and how much it freed

`.psram_section` on the C62 (see `openrtx/include/protocols/YSF/YsfMem.h`) is
the section `lib/dmr_vocoder`'s port shim, the codec endpoints and the
display's shadow buffer already use. `west build` reports the effect directly:

| Moved out of `.bss` | Bytes | Why it can live there |
|---|---|---|
| `OpMode_YSF::baseband[2 × 4800]` | 19 200 | the codec worker fills it with a software copy from the DSP buffer, sample by sample (`captureToStream`), so it is not a DMA target; `ysfPhy_push` reads it once per block |
| PHY sample ring `ring[7360]` | 14 720 | written once per sample at 48 kHz and read at symbol rate (480 reads per frame, ×4 taps), never inside a per-sample DSP loop; the scalars of the demodulator (`ringIndex`, `pos`, `hunt`) stay in SRAM because they are touched per sample |
| Viterbi survivor history `trellisHist[200][16]` | 3 200 | written before it is read inside one call, no state across frames |
| `out`, `call`, `text` snapshots | 586 | what the UI reads, updated once per 100 ms frame |
| voice queue + PCM scratch (`YsfSpeech.c`) | 2 104 | written before read (the queue only pops what it pushed, `pcm` is cleared before the stream starts) |
| **total** | **39 810** | SRAM 237 088 → **197 280 B (75.3 %)** |

Two things make this safe rather than lucky:

* The section is **NOLOAD** — the linker neither zeroes it nor copies it at
  boot, so anything placed there must be cleared by its own init. `ysfPhy_init()`
  and `ysfPhy_reset()` zero the ring, `ysfSpeech_start()` resets the queue and
  `playCall()` clears `pcm`; the frame layer keeps a `stateReady` flag in SRAM
  and resets itself on the first `ysfFrame_call()`, `ysfFrame_text()` or
  `ysfFrame_decode()`, because the UI may draw the call info before the mode
  has ever been enabled.
* The D-cache is on (`CONFIG_CSK6_DCACHE=y`), so the ring and the stream buffer
  are read through the cache and the extra traffic is 96 KB/s of writes plus
  ~20 k reads/s, not worth measuring. The measured instruction cost of §4.2
  does not change: it is a count of executed instructions, and there are no new
  ones.

The ring was **reviewed and left at 7360 samples**. It must hold one frame
(4800) plus the largest block the audio stream hands out (4800 in this mode,
2400 in the general case) plus the two samples the 4-point interpolator reads
ahead, so the real floor is about 4800 + 2400 + 4; 7360 keeps 2.4 KB of margin.
Now that the ring is not in SRAM, shrinking it to 5120 would free 4.5 KB of
PSRAM — of which there are 1.3 MB free — and gain nothing, so the margin stays.
The remaining SRAM cost of the mode is 592 B over the DMR baseline.

## 5. UI hooks

`openrtx/src/ui/**` is not touched; this is what a screen needs and where it
comes from.

| What | Where |
|---|---|
| Squelch / sync indicator | `ysfPhy_synced()`, wired to `rxSquelchOpen()` |
| Calling station | `ysfFrame_call()->source` (10 characters, already trimmed) |
| Destination or room | `->dest`, `->uplink`, `->downlink` |
| DG-ID | `->dgidValid`, `->dgid` (0-99) |
| Mode and progress | `->dt` (see §1.4), `->fi`, `->fn`/`->ft` |
| Received text message | `ysfFrame_text()->text`, `->complete`, `->messages` |
| Counters, for a diagnostics line | `->frames`, `->badFich`, `->voices`, `->unsupported`, `->dataBlocks` |
| Voice FR notice | `->unsupported` growing is "this call is a mode we cannot decode" |

Mode selection: the mode is `OPMODE_YSF = 8` in `enum opmode`, a channel can
carry it (`chstore_modeName()` returns `"YSF"`, and `chstore_valid()` /
`zones_valid()` accept it), and a zone can hold such a channel. The value is
fixed: `enum opmode` lays out the whole fleet's numbering — `NONE 0, FM 1,
DMR 2, M17 3, TETRA 4, NXDN 5, dPMR 6, P25 7, YSF 8, D-STAR 9, AIRBAND 10` —
including modes this tree does not implement, because the number is what the
channel store persists and what the other protocol branches must agree on when
they merge. A new mode takes 11 or more; it must not take the next free slot.
What is missing is the UI's side-key cycle: `openrtx/src/ui/default/ui.c` has a
`modes[]` array (around line 1101) guarded by

```c
#if defined(CONFIG_M17) || defined(CONFIG_TETRA) || defined(CONFIG_DMR)
```

so a build with `CONFIG_YSF` and none of those compiles the cycling out
entirely. The hook is two edits in that file — add `CONFIG_YSF` to the guard and
`OPMODE_YSF` to the array — and it was left alone on purpose, because three
other protocol agents are editing the same screens. Until then the mode is
reachable only from a channel programmed with it.

Messaging: `YSF_TEXT_MAX` and the reassembler are shaped for the shared store
(`core/messaging.h`) — transport, recipient and body — but no transport leg is
wired yet: receiving works, replying needs a transmitter, and this mode has
none.

## 6. Not done

* **No transmitter.** System Fusion needs C4FM and an AMBE+2 encoder at 2450
  bit/s; PTT in this mode does nothing rather than keying a PA with a waveform
  the radio cannot make.
* **No voice FR** (DT 3): needs an IMBE 7200 decoder.
* No Wires-X/network layer, no DG-ID filtering (the DG-ID is decoded and shown,
  not acted on).
* No AFC. The Level estimate removes the DC offset, which absorbs a carrier
  offset of a few hundred Hz; beyond that the slice thresholds drift.

## 7. What is unverified

Ranked by what would bite first.

1. **Text messages have never been received off air.** The block selection
   (BN = 2), the 20/10-character fields and the FN/FT sequencing follow dsd-fme
   and the FICH's block-type field. The decode path itself (whitening, CRC,
   convolutional code, interleave) is the same one that decodes the callsigns of
   §3, so a text block on air would go through proven code; the *assignment* of
   BN = 2 to text is the guess.
2. **V/D mode 1 voice layout.** The 36-dibit slot is taken to be the canonical
   `A(24)|B(23)|C(25)` channel frame in transmission order. No capture in V/D
   mode 1 exists to check it against. dsd-fme feeds that slot through the DMR
   per-burst dibit map instead, which is a different permutation and cannot be
   right for a system that has no bursts; if a V/D 1 capture ever disagrees, the
   permutation is one function (`voice72()`) in `YsfFrame.c`.
3. **Intelligibility of the decoded voice.** Structural checks pass; nobody has
   listened.
4. **The absolute deviation** (±1.8/±5.4 kHz) is nominal and not measurable from
   a recording without knowing the recorder's scale. It does not affect the
   receiver.

## 8. Sources

* op25, `gr-op25_repeater/lib/ysf_tx_sb_impl.cc`, `ysf_const.h` — a working
  encoder: sync dibits, FICH field packing, `generate_dch()`,
  `generate_vch_vd2()`, the PN(9,5) table, the convolutional masks.
* MMDVM, `YSFDefines.h`, `YSFRX.cpp` — frame geometry, the sync symbol values
  (with the two-symbol typo of §1.1).
* dsd-fme (dsd-neo), `src/protocols/ysf/ysf.c`, `ysf_frame.c` — the field
  decode, the block selection by BN/FN, the text message reassembly, the V/D 2
  voice path.
* RYSEN, `ysf_protocol.py` — spec citation (*Yaesu amateur digital
  specification, revision 1.02*, FICH figure 4-26, V/D 2 DCH figure 4-31, VCH
  figure 4-32), the FICH field table, the DG-ID in the squelch code, the PN
  register description.
* Rafe, "YSF: native C4FM DN-mode reception"
  (patrick.lidstone.net/wiki/protocols/ysf) — an independent write-up of the
  same chains; **explicitly not verified on air by its author**, and its
  whitening order is wrong (§1.5). Useful for the numbers, not as proof.
* sigidwiki, "Yaesu System Fusion" — the capture of §3.

## 9. Building it

```
# the mode alone
west build -b c62 -d build_ysf . -- -DCONFIG_C62_YSF=y

# with DMR, APRS and the shared message store
west build -b c62 -d build_both . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_YSF=y -DCONFIG_C62_APRS=y
```

`CONFIG_C62_YSF` is declared in `platform/targets/c62/Kconfig.board` (menu
"DMR", next to `CONFIG_C62_DMR_RX`), selects `C62_DMR_VOCODER`, and the
platform `CMakeLists.txt` adds the four sources and defines `CONFIG_YSF` for
them. Two failure modes worth recognising:

* `error: Aborting due to Kconfig warnings` followed by `warning: attempt to
  assign the value 'y' to the undefined symbol X` means the flag named a symbol
  that the tree being built does not declare — the checkout and the flags are
  out of step, not a fault in the option. The one that gets typed by accident
  is `-DCONFIG_YSF=y`: `YSF` is the *compile definition* the platform sets,
  `C62_YSF` is the Kconfig symbol. Check that the board Kconfig in the built
  tree has the `config C62_YSF` stanza before chasing the compiler.
* `CMake Error … Cannot find source file: subprojects/codec2/src/dump.c` is the
  meson subproject not being fetched: `(cd OpenRTX && meson subprojects
  download codec2)` once per west workspace.
