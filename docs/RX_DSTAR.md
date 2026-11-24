# D-STAR DV mode (receive, slow data and transmitter)

D-STAR DV for the Retevis C62: GMSK at 4800 bit/s in a 6.25 kHz channel, the
radio header with its convolutional FEC, the 20 ms voice/data frames, the
slow-data channel that carries the 20-character message and NMEA/GPS
sentences, and the transmitter that puts the same structure on the air.

The reference for every constant is the JARL specification, *Digital Smart
Technologies for Amateur Radio (D-STAR)*, version 7.0 (May 2025), cited below
as **STD-7 §x.y** — the PDF is `STD7_0_E.pdf` from jarl.org, and the DV mode is
chapter 4 with the scrambler and FEC in appendices 1 and 2. Where the
specification leaves a layer undefined (the slow-data block format) the
reference is G4KLX's analysis of on-air dumps, *The Format of D-Star Slow
Data*, v0.2.

## Status at a glance

| piece | state |
|---|---|
| bit/frame sync, 660-bit header (convolutional, interleave, scrambler, CRC) | done, soft-decision Viterbi |
| 96-bit frames, resync word every 21st, end pattern | done |
| slow data: 20-character message, header copies, NMEA 0183, D-PRS, squelch | done |
| message into the inbox, position into the heard list | done (`CONFIG_DSTAR_MESSAGING`) |
| transmitter: bit stream, GMSK shaper, PTT, end pattern | done, verified against the receiver |
| voice (AMBE 2020 3600x2400) | **decoder done** (2.8–3.7 % of the M33), encoder not — see "Voice" |
| deviation calibration, adjacent channel power, on-air test | pending (bench) |
| UI: mode entry and screens | **hook only** — see "UI hooks" |

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_DSTAR=y
```

`CONFIG_C62_DSTAR` (`platform/targets/c62/Kconfig.board`) adds the protocol
sources, `OpMode_DSTAR` and `CONFIG_DSTAR`; `CONFIG_DSTAR_TX` (default **y**
with it) adds the transmit chain. The mode number is `OPMODE_DSTAR = 9`
(`openrtx/include/rtx/rtx.h`), from the project's canonical mode numbering, so
the feature branches do not collide when they merge.

## Where the code lives

```
openrtx/include/protocols/DSTAR/
    DStarDefs.h      rates, sync words, frame geometry, header offsets, slow-data types
    DStarFec.h       header packing, P_FCS, convolutional code, interleaver, scrambler
    DStarPhy.h       streaming demodulator interface
    DStarSlowData.h  6-byte blocks, message, NMEA/D-PRS parsing
    DStarVocoder.h   72-bit frame -> 48 AMBE parameter bits (the FEC front end)
    DStarSpeech.h    voice queue and playback thread
    DStarStatus.h    call in progress, message, position, heard stations
    DStarTx.h        transmit chain
openrtx/src/protocols/DSTAR/       the same, in C
openrtx/src/rtx/OpMode_DSTAR.cpp   the mode (buffers, audio paths, PTT)
tests/host/dstar/dstar_host.c      self-test, loopback, BER sweep, file decoder
tests/host/dstar/dstar_vocoder_test.c  bit-exactness against mbelib-neo
tests/platform/c62_dstar_cost.c    QEMU instruction count, receive
tests/platform/c62_dstar_tx_cost.c QEMU instruction count, transmit
```

The protocol layer is C (like `protocols/DMR` and `protocols/TETRA`), the mode
handler is C++ because every mode handler is, and it does nothing but move
buffers between the audio paths and the decoder.

## The specification, as implemented

### Waveform

* 4800 bit/s, one bit per symbol, GMSK (STD-7 §4.1.1), **BT 0.5**, modulation
  index h = 0.5, i.e. 1200 Hz of peak deviation at 4800 bit/s, inside a
  6.25 kHz channel.
* "1" is a **positive** frequency deviation (STD-7 Ap1.5).
* Bits inside a byte go out **LSB first** (STD-7 Ap1.4).
* Transmission: 64 bits of bit sync `1010…` (STD-7 §4.1.1 a), the 15-bit frame
  sync `111011001010000` = 0x7650 (§4.1.1 b), the coded header, then frames,
  then the end pattern.

Note on BT: MMDVM's transmitter uses a Gaussian with BT 0.35; the specification
and the published D-STAR material say 0.5. This implementation shapes with 0.5
and receives a 0.5 matched filter, which tolerates either (the loopback also
decodes with a 0.35 filter, §"Evidence").

### Frame geometry

96 bits per 20 ms frame (STD-7 §4.1.2 b): 72 voice bits and 24 slow-data bits.
Every 21st frame's slow-data field is the 24-bit resync word
`101010101011010001101000` = 0xAAB468 (§4.1.2 c/d: 10-bit bit sync + the
M-sequence `1101000` twice). The transmission ends with 32 bits of `1010…`,
the 15-bit `000100110101111` and a `0` (§4.1.2 h) — six bytes
`55 55 55 55 C8 7A` sent LSB first.

### Radio header

41 bytes: three flag bytes, RPT2 (destination repeater), RPT1 (access
repeater), UR ("other station", `CQCQCQ` for a general call), MY (own
callsign), a four-character suffix, and P_FCS (STD-7 §4.1.1 c–k). Flag 1 bit 7
is voice/data, bit 6 repeater/direct, bit 5 interruption, bit 4 control, bit 3
emergency, bits 2..0 the control/ACK/retry code (§4.1.1 c). A simplex call
spells `DIRECT` in both repeater fields (§4.1.1 f/g).

P_FCS is CRC-16/X.25: reflected 0x8408, init 0xFFFF, final inversion, low byte
first (§4.1.1 k).

Coding (STD-7 Ap2): the 328 information bits plus two zero flush bits go
through a rate-1/2, **K=3** convolutional code with generators 7 (`b^d1^d2`,
sent first) and 5 (`b^d2`), the 660 coded bits are written into a 24-column
matrix and read column-major (660 = 24·27 + 12, so the last column is ragged),
and the result is XORed with the scrambler sequence S(x) = x⁷ + x⁴ + 1 from an
all-ones seed (STD-7 Ap1.1). The interleaver is derived arithmetically
(`k += 24` with the two wrap corrections) rather than stored: 1.3 kB of flash
saved and no table to get wrong.

### Slow data

24 bits per frame, XORed with the first three bytes of the PN sequence
(`70 4F 93` in transmission order). Two frames make a 6-byte block whose first
byte is a type nibble and a low nibble that is a length — except for the free
text, where it is a fragment number (G4KLX, slow data v0.2):

| type | content |
|---|---|
| 0x4n | 20-character message, five characters per block, four fragments |
| 0x5n | copy of the 41-byte radio header, five bytes per block (late entry) |
| 0x3n | five characters of a position sentence: `$GPGGA`, `$GPRMC`, free text or APRS `$$CRC` (D-PRS) |
| 0xCn | code squelch |
| 0x6n | filler (`f`) |

**There is no packed 20-bit coordinate in the DV slow data.** GPS arrives as
fragments of an NMEA 0183 sentence or of a D-PRS line, and it is parsed as text
with its own checksum (NMEA XOR, or the D-PRS CRC-16 over the text after the
comma). The "20-bit GPS" one reads about in D-STAR forums belongs to the
ICOM-specific compressed formats above this layer (GPS-A/D-PRS), not to the
on-air slow-data block.

### Voice

The 72 bits are the AMBE-2020 **channel** frame of the full-rate 2400 bit/s
codec (STD-7 §4.1.2 b: "in accordance with the AMBE-2020 (with FEC)
standard"), with the FEC inside the 72 bits: a Golay(24,12) word for c0 (plus
an overall parity bit), a Golay(23,12) word for c1 XORed with a pseudo-random
sequence seeded by c0, and 24 unprotected bits. That is **not** the DMR frame.

## What the decoder does

```
48 kHz discriminator
  Gaussian matched filter BT 0.5, decimated 2:1 to 24 kHz (5 samples/symbol)
  symbol timing in blocks of 64 symbols: 3 phases x 3 quarter-sample offsets,
      the widest eye wins (re-measured every 13 ms, so clock drift is followed)
  slice + soft value (+-96 normalised by the block's mean |sample|)
  15-bit frame sync on either polarity (a discriminator can be inverted)
  660 soft bits -> descramble -> de-interleave -> Viterbi -> P_FCS
  96-bit frames: 72 voice + 24 slow, resync word every 21st, end pattern
  slow data -> 6-byte blocks -> message / header / sentence -> status
```

Four things are worth knowing about the implementation:

1. **No tables for the interleaver or the scrambler.** Both are generated, so
   the encoder and the decoder cannot drift apart.
2. **Soft decisions in the header Viterbi.** The 660 soft values are kept (660
   bytes of stack) because that is where the ~2 dB of coding gain of the header
   lives; on hard decisions the header collapses about 2 dB earlier.
3. **Polarity is discovered, not assumed.** A false lock on the wrong polarity
   is discarded by the header CRC; a good one is latched and the frame bits are
   inverted from then on.
4. **The frame counter is free-running.** A resync word that does not decode
   does not lose the call; two superframes without one do.

## Evidence

### Header FEC self-test (`dstar_host selftest`)

Round trip of a header through the encoder and the soft Viterbi: 0, 6 and 20
flipped channel bits all decode with a valid P_FCS, the slow-data message
reassembles, and a `$GPGGA` sentence cut into five-character fragments comes
back with the right date, position and altitude.

### Loopback with a real channel model (`dstar_host loop` / `stress`)

Transmitter → AWGN → receiver, 100 voice frames (2 s) per run, noise quoted
over the 6.25 kHz channel against the measured signal power:

| channel SNR | payload BER | header | text |
|---|---|---|---|
| 14 dB | 0 | ok | 4/4 |
| 13 dB | 1.4e-4 | ok | 4/4 |
| 12 dB | 6.9e-4 | ok | 4/4 |
| 10 dB | 5.7e-3 | ok | 4/4 |
| 8 dB | 2.2e-2 | ok | 4/4 |
| 7 dB | 3.7e-2 | ok | 3/4 |
| 6 dB | 5.5e-2 | ok | 2/4 |
| 5 dB | — | **fails** | — |

Eb/N0 is 1.15 dB above the channel SNR here (6250/4800), so the header decodes
down to about 7 dB Eb/N0, which is where a rate-1/2 K=3 code with a soft
decision Viterbi should be, and the raw payload BER matches uncoded GMSK theory
within about 1.5 dB. The slow data has no FEC, so the text is the first thing to
go — exactly what a real D-STAR receiver does.

Impairments that are handled (all with the header decoding and 100/100 frames):

* clock error ±100 ppm and ±500 ppm (the block timing re-measures the eye);
* fractional timing offsets of 0.4, 0.7 and 1.5 samples;
* an **inverted discriminator** (polarity is discovered from the header CRC);
* the combination at 8 dB SNR: +200 ppm, 1.3 samples and −200 ppm, 0.4 samples
  with inversion.

### Independent implementation

The host prototype in `work/dstar-proto/` was written first, from STD-7 and
MMDVM's tables, in Python. Given this firmware's transmitter output it finds
the sync, decodes the header with a valid P_FCS and the exact callsigns
(`ED1ZAR G` / `ED1ZAR B` / `CQCQCQ` / `EA5TEST` / `C62`) and recovers all 20
voice frames bit-identically. Two independently written implementations of the
same specification agreeing on the whole chain is the strongest check available
without a captured signal.

### Voice front end against mbelib-neo

`tests/host/dstar/dstar_vocoder_test.c` feeds frames built by mbelib-neo's own
encoder to both `dstarVocoder_params()` and mbelib's
`mbe_decodeDStarDVData()` + `mbe_decodeAmbe3600x2400Frame()`: **20 002 frames,
9 938 of them with one to three channel bit errors, 0 mismatched parameter
bits**.

### Real captures: what was available

No public baseband or IQ recording of D-STAR DV was found, and this machine has
no SDR attached (`SoapySDRUtil --find`: no devices). What the usual sources
actually contain:

* sigidwiki `DStar_Sound.mp3` — the *decoded audio*, not the discriminator:
  energy below 1 kHz, envelope coefficient of variation 0.86 (a GMSK baseband
  is constant-envelope and bimodal), no sync at any polarity.
* qsl.net/kb9mwr `dstar.wav` (8 kHz) and `dstar-undecoded.wav` — the first is
  decoded audio; the second, despite its name, is speech-band audio too
  (same test).
* utahvhfs.org `dstar_WFM_sig_level_*SINAD*.mp3` — receiver audio recorded at
  12/7/3 dB SINAD for a sensitivity article, not the discriminator output: no
  2400 Hz bit-sync tone, envelope CV 0.86.

So the evidence above is loopback + an independent decoder + bit-exactness of
the codec front end. **A captured D-STAR transmission is still the one thing
that would settle the last open question** (below).

### One open question a capture would settle

The 2009 G4KLX *D-Star Reference* (the "D-Star Reference" package on
qsl.net/kb9mwr, in `work/dstar-proto/` next to the host prototype) encodes
the header bytes **MSB first**; STD-7 Ap1.4 says LSB first, and MMDVM (the
implementation that talks to real radios) also uses LSB first. This
implementation follows STD-7 and MMDVM. The two conventions are both
self-consistent and produce different 660-bit streams, so only a real capture
(or a radio) can prove which one the air uses. Nothing else in the chain
depends on it.

## Voice: the AMBE 3600x2400 decoder

D-STAR's DV frame carries **AMBE 3600x2400**: 72 bits of which 12+12 are two
Golay(24,12) half-words (each with its parity bit) and 24 are unprotected,
leaving **48 parameter bits**. `lib/dmr_vocoder` cannot be reused for it: that
one decodes **AMBE+2 3600x2450** for DMR, 49 parameter bits with a *predictive*
spectral quantiser and its own tables. The FEC primitives are shared; the
payload is not, and feeding a D-STAR frame to the DMR decoder produces
plausible-looking noise.

So the decoder is its own library, `lib/dstar_vocoder/` (1 328 + 68 lines of C,
1 033 lines of generated tables):

| file | what it is |
|---|---|
| `dstar_vocoder.c` | `dstarVocoder_decode()`: state, the weak-hook wiring for the RTX |
| `dstar_decoder.c` | the ported core: parameter decode, model enhancement, the MBE synthesis (harmonic/interpolated-voiced/unvoiced FFT, LPC, comfort noise) |
| `dstar_tables.h` | every codebook, generated by `tools/gen_tables.c` |
| `openrtx/src/protocols/DSTAR/DStarVocoder.c` | air-bit map, Golay(24,12) + parity, LCG PN, packing into the 48 MBE bits |

The front end (`dstarVocoder_params()`) was verified against mbelib-neo before
the core existed, and the evidence is in §"Voice front end against mbelib-neo"
above: 20 002 frames, 9 938 of them with one to three channel bit errors, 0
mismatched parameter bits.

### Licensing and attribution

| part | licence |
|---|---|
| `lib/dstar_vocoder/dstar_decoder.[ch]`, `dstar_tables.h` (derived from mbelib-neo) | **GPL-2.0-or-later** |
| `lib/dstar_vocoder/dstar_vocoder.[ch]`, `DStarVocoder.[ch]`, the tools and tests | **GPL-3.0-or-later** |
| this firmware | GPL-3.0-or-later |

The core is a **port of mbelib-neo v2.6.2's D-STAR path** (`src/ambe/ambe3600x2400.c`,
`src/ecc/`, `src/core/`, GPL-2.0-or-later, © the mbelib-neo authors; the AUR
tuning warnings it inherited come from OP25's model, © Max H. Parke KA1RBI).
GPL-2.0-or-later can be taken under GPLv3, so the combination is distributed as
GPL-3.0-or-later, exactly as `lib/dmr_vocoder` already is (see
`docs/DMR_VOCODER.md`, *Licensing*). `dstar_tables.h` is **generated from the
reference's own tables** by `tools/gen_tables.c` — no number was re-typed by
hand, and regenerating it from another mbelib-neo revision is a one-command
change.

The version is pinned for a reason: v2.6.2's synthesis and later revisions'
differ. Decoding the same 2 625-frame bank with v2.6.12 gives a waveform 55 520
LSB away from v2.6.2's (SNR −3.6 dB) — a *different decoder*, so "matches
mbelib-neo" is only meaningful with the tag.

As with the DMR vocoder, none of this settles the AMBE-2020 patent question:
a free software licence does not grant patent rights, and shipping a product
with the decoder is a business decision independent of the source licence.

### PCM against the reference

`tests/ref/ref_dstar.c` calls mbelib-neo's own `mbe_decodeDStarDVData()` +
`mbe_processAmbe3600x2400Frame()`; `tests/pcm_dump.c` runs this port through
its firmware entry point. Same 2 625-frame bank, both in float:

| | value |
|---|---|
| frames bit-identical | 602 / 2 625 |
| samples differing at all | 58 156 / 420 000 (13.8 %) |
| samples differing by more than 1 LSB | 17 677 (4.2 %) |
| largest difference | **27 LSB** (0.04 % of full scale) |
| SNR | **72.9 dB** |

For scale: **the reference against itself across optimisation levels is worse.**
mbelib-neo v2.6.2 built `-O0` and `-O3` decodes the same bank to waveforms that
differ by up to **323 LSB, SNR 59.5 dB**. The port is closer to the reference
than two builds of the reference are to each other, which is the honest way to
read "bit-exact": the parameters are exact, the PCM is not, and the reference's
own PCM is not stable at that level either.

### Where the PCM residue comes from

It is **rounding, not a wrong decision**. The firmware has no libm: sin/cos,
exp2, log2 and sqrt come from `lib/dmr_vocoder/dmr_math.h`, whose documented
envelope is ≤ 2 ulp (sqrt is exact). The reference uses the host's `sinf`/
`cosf`, which round differently, and the synthesis is a recursive oscillator
over 160 samples, so a 1–2 ulp difference in one rotation shows up as a few LSB
of accumulated phase at the end of the frame.

The tempting explanation — a `floorf()` somewhere wrapping a phase that lands
almost exactly on a multiple of 2π, where one ulp of phase picks the other
branch — was **measured and does not happen**. The only phase fold of that kind
is in the interpolated-voiced renderer,
`floor_((deltaphil + π) / 2π)`: instrumented over the whole bank, the closest
that argument ever comes to an integer is **0.0044** (harmonic 1, frame 2 608).
Flipping the branch needs a relative difference of ~10⁻⁹, five orders of
magnitude below the port's 2 ulp, so it cannot be the cause. The 27 LSB is
ordinary single-precision drift, which is also why the reference cannot
reproduce itself across `-O0`/`-O3`.

### End to end

`dstar_host voice` runs the whole chain on the host: bank frames → GMSK
transmitter → AWGN channel (SNR quoted over the 6.25 kHz channel) → the
firmware demodulator → FEC → this decoder → 8 kHz WAV. 2 625 frames is 52.5 s.

| channel SNR | voice frames decoded | payload bit errors |
|---|---|---|
| 10 dB | 2 505 / 2 625 | 6.1 × 10⁻³ |
| 12 dB | 2 505 / 2 625 | 8.2 × 10⁻⁴ |
| 14 dB | 2 505 / 2 625 | 2.8 × 10⁻⁵ (1 bit in 36 000) |
| 20 dB | 2 505 / 2 625 | **0** |

The 4.6 % that do not come out are lost to the demodulator's 13 ms timing
block, not to the codec: the *same* 120 frames go missing at every SNR, and
"good enough" here means 95 % of the frames with a payload BER under 1 %,
which is what the harness asserts.

### Cost of the decoder

Instructions per 20 ms frame, QEMU `mps2-an505` with the TCG counter, **linking
the objects the firmware build itself produced** (`FIRMWARE_BUILD=<west build
dir> lib/dstar_vocoder/tests/qemu/run_qemu.sh`), so `-Os`, Zephyr SDK 0.16.8
and the shipped code:

| | per frame | 300 MHz, CPI 1.2 | CPI 1.6 |
|---|---|---|---|
| shipped objects (`-Os`, firmware flags) | **138 711** | 2.8 % | 3.7 % |
| same sources, `-O2 -ffp-contract=off` | 120 661 | 2.4 % | 3.2 % |

The `-Os` object is 15 % *more* expensive than the `-O2` one — the number to
quote is the first row. Both host and ARM outputs of the shipped objects are
the same PCM as the host build (0 of 64 000 samples differ), which is the
bit-exactness check that matters for the target.

Stack, `-fstack-usage` with the firmware's flags: the deepest **normal** chain
is `dstarVocoder_decode` (88 B) → `dstar_decoder_frame` (160 B) →
`decode_parms` (520 B) → `comfort_noise` (20 B) = **788 B**; a frame that has
to be repeated from the model (`parms_repeat_model`, 2 456 B) uses **2 704 B**.
The air-bit front end alone is `dstarVocoder_frame` + `dstarVocoder_params` =
152 B, which is not the path to budget for.

Memory: the whole decoder state is one object of **14 096 B**, placed in PSRAM
(`DSTAR_VOCODER_STATE_ATTR`) with `bss` growth of 8 B; flash **+38 636 B**
(see the table below).

## Transmitter

The chain is the receiver's inverse, in `DStarTx.c`:

```
64-bit bit sync, 15-bit frame sync, 660 coded header bits,
N x 96 bits (72 voice + 24 slow data, resync word every 21st frame),
48-bit end pattern
-> NRZ (+-1) convolved with the Gaussian pulse BT 0.5 at 10 samples/symbol
-> 48 kHz modulating waveform for the FM exciter
```

The slow data carries the message from the message store
(`MSG_TRANSPORT_DSTAR`, 20 characters) followed by repeated copies of the
header, which is what a radio does and what gives a late entrant the callsigns.
The voice payload is the D-STAR **null frame** unless a 3600x2400 encoder is
linked in (`dstarTx_setVoiceSource()`): that is exactly what an Icom radio
transmits with no microphone signal, so framing, header, slow data and
modulation are all exercised on air even without a codec.

The transmitter's bit counter is 32-bit: at 16 bits it wrapped after 65 536
bits, i.e. **682 frames (13.6 s)**, after which `bitIndex % 96` no longer fell
on a frame boundary, the resync word landed mid-frame and the receiver lost the
frame sync for the rest of the transmission (29 % BER in the loopback with not
one payload bit actually wrong). `dstar_host voice` now asserts on the resync
words so this class of failure cannot hide behind a BER figure: 2 625 frames /
52.5 s decode with 125/125 resync words and 0 bit errors at 20 dB, and the
`loop` self-test prints the count (`resync=`) for the same reason.

The receiver decodes the transmitter's output frame for frame in the loopback
above, and the independent Python decoder agrees bit for bit, so what is
unproven is the analogue part: **deviation**. `DSTAR_TX_AMPLITUDE` scales the
waveform into the same `REG_40` deviation register the DMR transmitter uses, and
that number is still a bench-calibration item (DMR's is marked "a starting
point, not a measurement" in `docs/DMR_TX.md`). Adjacent channel power is
equally pending.

## Cost

### Flash and RAM

`west build` on the C62, whole image:

| build | FLASH | RAM | PSRAM |
|---|---|---|---|
| `CONFIG_C62_DMR_RX=y CONFIG_C62_APRS=y` (baseline) | 518 112 B (49.4 %) | 196 688 B (75.0 %) | — |
| + `CONFIG_C62_DSTAR=y -DCONFIG_DSTAR_TX=n` | 524 324 B (50.0 %) | 207 536 B (79.2 %) | |
| + `CONFIG_C62_DSTAR=y` (receive **and** transmit) | 526 368 B (50.2 %) | 208 512 B (79.5 %) | |
| + `CONFIG_C62_DSTAR_VOCODER=y` (DMR RX + APRS + D-STAR, shipped) | **565 004 B (53.9 %)** | 208 520 B (79.5 %) | 393 160 B (21.4 %) |

So D-STAR costs **+8 256 B of flash (+1.6 %)** and **+11 824 B of RAM
(+4.5 %)**, or +6 212 / +10 848 without the transmitter; the vocoder adds
**+38 636 B of flash**, **+8 B of RAM** and **+14 096 B of PSRAM** (its whole
state lives there). The RAM is dominated by
the 5.7 kB half-duplex audio buffer, the 1 kB of filtered-sample ring, the 660
soft bits of the header and the heard-station table; receive and transmit share
one buffer because the radio is half duplex, which is what brought the image
back under 80 % of RAM.

### Instructions (QEMU, Cortex-M33)

Measured with the TCG plugin of `work/dsp-probe`, bare metal on `mps2-an505`,
markers every 50 frames:

| path | instructions per second of audio | per 20 ms frame | % of a 300 MHz M33 |
|---|---|---|---|
| receive (demod + header FEC + slow data) | 8.03 M | 160 600 | 2.7 % |
| transmit (bit stream + GMSK shaper) | 6.20 M | 124 000 | 2.1 % |
| voice decode (AMBE 3600x2400, shipped objects) | 6.94 M | 138 711 | 2.8–3.7 % |

The receive cost is dominated by the decimating matched filter (21 taps at
24 kHz) and the nine-candidate timing search every 64 symbols. Adding the
vocoder leaves the whole mode at roughly 5–6 % of one core per direction, and
one frame of audio (20 ms) still costs ~0.7 ms of decoding, so the mode has
about an order of magnitude of headroom.

## UI hooks (not implemented: `openrtx/src/ui/**` is off limits here)

The mode is registered in the RTX (`OPMODE_DSTAR`, `rtx.cpp`) and reachable
from a channel whose mode is D-STAR, but the default UI still needs:

1. **Mode entry in the cycle list** — `openrtx/src/ui/default/ui.c` around line
   1109 enumerates the modes; `OPMODE_DSTAR` goes there (and in
   `ui_channels.c`'s mode table, around line 339, for the channel editor).
2. **A status screen** reading `dstar_getStatus()`: MY/RPT1/RPT2/UR, the flag
   byte ("via repeater", "emergency"), the 20-character message, the position,
   and `viterbi` as a link-quality hint. `dstarStatus` is copied out, so the
   screen only reads.
3. **A heard list** reading `dstarHeard(i)`: callsign/suffix, repeater,
   destination, age, position — the D-STAR counterpart of the APRS station
   list, newest first, eight entries.
4. **Quick-menu entries**: the transmit message (the slow-data text), the
   destination (`CQCQCQ` or a station), RPT1/RPT2 for the repeater in use, and
   a read-only "voice: AMBE 2.4k" line from `dstarVocoder_available()` (it is
   true in this build). Per the standing UI rules the quick menu carries what
   only it can reach, and every value has to fit its row.
5. **Contacts/messaging**: `MSG_TRANSPORT_DSTAR` is already in the store
   (`msg_service` reports it as "D-STAR", 20 characters, available in this
   mode); the compose screen will pick it up from `msg_service_default()`. The
   text a user queues becomes the slow-data message of the next transmission.
6. **Channel data**: a D-STAR channel needs its own repeater fields
   (RPT1/RPT2, UR). Today the mode fills them from the callsign and the
   repeater flag; a channel record extension is the clean way to carry them
   (`core/channel_store`, which the other feature branches also touch — the
   fields should be added once, centrally).

## What is still missing

* **An encoder**: the voice path is decode-only (as on the DMR side, an AMBE
  encoder does not fit the M33 alongside everything else), so a transmission
  from this radio carries the null frame unless `dstarTx_setVoiceSource()` is
  given frames from somewhere.
* **A listener telling us how it sounds**: the PCM is 72.9 dB against the
  reference and the bank decodes to intelligible speech, but no human has
  compared it with an Icom radio on the same signal.
* **Deviation and ACP of the transmitter** on the bench, and the deviation
  figure in `REG_40`.
* **A real capture**, to close the header bit-order question of §"One open
  question" and to confirm the slow-data and BT assumptions on air.
* **The UI**, as listed above: the mode is complete at the protocol and RTX
  level, not yet visible on the radio's screen.
