# P25 Phase 1 receive mode

P25 Phase 1 (TIA-102.AAAA/BAAC/BAAA/BABA) receiver for the Retevis C62: C4FM
demodulation of the radio's discriminator output, frame sync, the NID with its
NAC and DUID, IMBE channel decoding, the link control of an LDU1 and the
trunking blocks of a control channel.

Receive only. There is no transmitter and no trunking follow: entering a
frequency and decoding what is there is the whole feature.

The mode is a port of the host prototype in `work/p25-rx`, which validated the
chain against dsd-neo and op25 on real captures before any of it was written
into the firmware. This document says what the firmware does, what it cost to
make it decode, what it costs to run and what is not tested.

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y \
    -DCONFIG_C62_P25=y
```

`CONFIG_C62_P25` (`platform/targets/c62/Kconfig.board`, CMake option
`C62_P25`) adds the protocol sources, `OpMode_P25` and `CONFIG_P25` for the rest
of the firmware. It is off by default. Voice comes with it: `CONFIG_C62_P25_VOCODER`
(default **y**, see *Voice* below) builds `lib/imbe_vocoder`, our own decoder.
Turn it off and the mode still decodes, shows the call and counts frames, but
plays nothing.

`OPMODE_P25 = 7` in `rtx.h`. The numbering is shared with the other receive
modes landing in this fork (NXDN 5, dPMR 6, YSF 8, D-STAR 9, airband 10): use
the value, never renumber.

## Where the code lives

```
openrtx/include/protocols/P25/
    P25Defs.h    air-interface constants, DUIDs, bit-position tables
    P25Fec.h     Golay(23,12)/(24,12), Hamming(15,11)/(10,6), BCH(63,16),
                 RS(24,12) and RS(36,20) over GF(64), trellis, CRC-16
    P25Phy.h     streaming demodulator, frame struct, callback
    P25Speech.h  voice queue and playback thread
    P25Status.h  what the UI shows
    P25Vocoder.h IMBE decoder hook
openrtx/src/protocols/P25/
    P25Defs.c  P25Fec.c  P25Phy.c  P25Tables.c
    P25Speech.c  P25Status.c  P25Vocoder.c
openrtx/src/rtx/OpMode_P25.cpp
openrtx/include/rtx/OpMode_P25.hpp
tests/host/p25_phy_host/    the firmware PHY on a host, on the same captures
tests/unit/p25_phy.cpp      frame-cut tests on synthetic C4FM frames
tests/unit/p25_frame_bits.h one real LDU1 as bits, for those tests
tests/unit/p25_tsdu_bits.h  one real TSDU (720 bits) as bits
work/dsp-probe/p25phy/      bare-metal cost harness (QEMU mps2-an505)
```

## The chain

```
48 kHz discriminator (SOURCE_RTX -> SINK_MCU, 20 ms blocks)
  RRC matched filter, alpha 0.2, 81 taps        -> decimated to 24 kHz
  4-point Lagrange interpolation, 5 samples/symbol (4800 symbol/s)
  frame sync: 24-symbol correlation, five sub-phases per symbol period,
              refined over +-0.5 symbol, aperture gated, sign errors counted
  per-frame levels (DC and outer level from the sync, refined on the payload)
  slicing to dibits (01 -> +1800 Hz, 00 -> +600, 10 -> -600, 11 -> -1800)
  NID: BCH(63,16) plus the DUID parity bit    -> NAC and DUID
  LDU1/LDU2: 9 IMBE codewords (4 Golay + 3 Hamming + 7 raw bits, PN scrambled),
             low-speed data (24 Hamming(10,6)) and the LCW (RS(24,12))
  TSBK: three 196-bit trellis blocks, CRC-16 masked 0xFFFF, one per frame
```

A voice frame is 1728 bits (864 symbols, 180 ms): 48 sync bits, a 64-bit NID,
and a 24-symbol status field inside both. Frames arrive back to back, so once
the sync is found the next one is predicted; the prediction is checked over
+-8 samples and the search falls back to a scan of the ring when it fails.

Not every P25 frame is 864 symbols, and the demodulator cuts each one to the
length its own data unit announces: 864 for an LDU1/LDU2 (or a PDU), 396 for an
HDU, **360 for the TSDU of a control channel**, 216 for a TDULC and 72 for a
TDU. `p25_frame_symbols()` in `P25Defs.c` is that table, and *The frame cut*
below is why it exists.

## The defect that kept the firmware from decoding

The first port found the sync and decoded nothing: on `samples/vc_af_48k.wav`
it reported a valid candidate at ring sample 814 with a correlation of 0.91 and
two sign errors, and then no frame at all.

Everything downstream was already right. Sliced at the right phase, the
firmware produced the prototype's bits exactly (`sync=5575f5ff77ff`,
`next48=29300cced77e`, NID `29300f3b5df801c2`) and the FEC, the levels, the
LCW and the IMBE decoders all agreed with dsd-neo. The fault was in how the
frame was cut out of the ring:

* the search only evaluated the sample instants of its 24 kHz ring, stepping
  the frame start in whole symbols, so the sampling instant was anywhere
  inside a symbol, uniformly distributed over the five sample phases;
* the aperture gate (the fraction of the 24 sync symbols at the outer level,
  threshold 0.75) was applied at that arbitrary phase;
* at 0.4-0.45 symbol off the eye, the matched filter attenuates the outer
  levels enough that only 16 of the 24 sync symbols clear the threshold, the
  aperture reads 0.67 and the true sync is discarded before the phase
  refinement can run.

Measured on the capture: at phase 814.0 the aperture is 0.67, at 815.0 it is
0.92, at 816.0 it is 1.00 — and the search only ever looked at 814, the
integer-sample phase, because the next attempt was five samples further on.
Four sampling phases in five were lost that way.

The fix is the prototype's algorithm: evaluate the sync at the five
interpolated sub-phases that cover one symbol period, then refine over
+-0.5 symbol by maximising the DC-free correlation with the pattern. The
refinement had to be widened from +-0.25 symbol because the correlation is flat
to within its own rounding over the coarse grid (1 sample = 0.2 symbol): which
sub-phase wins the coarse pass is arbitrary among the good ones, and a narrow
refinement clamps against its own edge instead of finding the eye.

Two defects found afterwards, both in the same place:

* a frame that does not start where the grid predicted it (a truncated header
  before a call, a lost frame, a late entry) was skipped for ever, because the
  search never looked behind its prediction. The ring now holds two frames and
  a failed prediction restarts the scan from just past the last sync;
* restarting one symbol past the integer start decoded the same frame twice
  (the refinement can pull a start up to a sample past the sync), so the rescan
  position comes from the interpolated phase of the sync that was decoded.

A third, measured rather than reasoned: the first version put the aperture gate
in front of the sub-phase search as a cheap rejection. That gate is exactly the
metric that collapses at a bad phase, so a true sync that only the *interpolated*
phase would have saved was rejected at the sample phase first. The gate now runs
on the interpolated phases like in the prototype.

## The frame cut: a fixed 864-symbol window

With the sync and the sampling phase right, the demodulator still cut every
frame to the 864 symbols of a voice LDU, whatever the NID said that frame was.
That is the right window for voice and the wrong one for everything else, and
the control channel is where it shows: a TSDU is 720 bits, 360 symbols.

Instrumented (`p25Phy_syncTries()`, `p25Phy_gridMisses()` and the span in the
frame struct), on the captures of the repo, the old code was:

| capture | frame on the air | window cut | frames | grid misses | candidates/frame | host cpu/frame |
|---|---|---|---|---|---|---|
| voice, 5.4 s | 864 (LDU) | 864 | 31 | 9 | 338.9 | 688 us |
| control, 54.4 s | **360 (TSDU)** | **864** | 725 | **725** | 374.9 | 551 us |
| 20 s of contiguous LDUs | 864 | 864 | 112 | 1 | 53.8 | 404 us |

Where the window went wrong, in numbers, for the TSDU case:

* the frame is 360 symbols and the window 864, so **504 symbols of every window
  (58 %) belonged to the frames that follow the frame** - their sync, their NID
  and part of their payload were sliced into the same `bits[]`;
* the per-frame DC and outer level were fitted over 2.4 frames, two of whose
  sync fields are all outer levels, so the levels were measured on a different
  stretch of air than the one being sliced;
* the grid predicted the next sync at `start + 864 symbols`, i.e. **504 symbols
  (140 %) past where it is**, so on a control channel the prediction could never
  be right: **725 grid misses in 725 frames**;
* every miss fell back to scanning the ring one candidate per symbol, which is
  where the 375 candidates per decoded frame came from.

The fix is the frame's own structure, not a shifted offset. The sync and the NID
are 57 symbols for every data unit (the NID's 64 bits carry one status symbol,
which is why the payload of a TSDU starts at symbol 57), so they are sliced
first, on the levels of the sync field alone: that field is all outer levels and
sign balanced, so it fixes DC and outer level by itself. The NID then names the
data unit, `p25_frame_symbols()` turns that into a length, and the rest of the
frame is sliced to it, with the levels refitted over the frame and nobody
else's symbols in the window. The grid advances by the same length. A frame
longer than what the ring holds yet is left where it is and cut on a later push,
instead of reading the samples the ring held one wrap ago.

Two more defects the same instrument turned up, both in the same place:

* the grid was predicted from the integer ring position the candidate search
  started at, not from the phase the sync was actually found at. That error (up
  to a symbol) ate most of the +-8 sample window that exists for the symbol
  clock's own drift, so a voice frame whose phase sat late was re-acquired by
  hand: 9 of 31 frames on the voice capture. Predicting from the interpolated
  phase leaves 1 - the frame after the last one, which is genuinely absent.
* `p25Phy_reset()` started the search at the bottom of the ring, a negative
  position with nothing written in it, and walked forward from there: every
  acquisition paid a few thousand candidates that all interpolated sample 0.
  Starting at 0 took the voice capture from 10505 candidates to 1217.

After the fix, same captures, same instrument, median of seven runs on the host:

| capture | frames | grid misses | candidates/frame | host cpu/frame |
|---|---|---|---|---|
| voice, 5.4 s | 31 | 1 | **39.3** | **418 us** (-39 %) |
| control, 54.4 s | 725 | **1** | **5.3** | **158 us** (-71 %) |
| 20 s of contiguous LDUs | 112 | 1 | 19.6 | 372 us (-8 %) |

The host figures are x86-64 CPU time per decoded frame (`tests/host/p25_phy_host`
timing `p25Phy_push()` with `CLOCK_PROCESS_CPUTIME_ID`), median of seven runs of
each binary interleaved on an otherwise idle machine; run to run they spread by
+-10 % on this host, so the ratio is the number to read, not the absolute. They
are a ratio, not a budget - the M33 numbers are the QEMU ones under *Cost*. What did **not** change is the payload: the voice
capture still gives NAC 0x293, 15/15 link controls and 261/261 IMBE frames with
zero corrected bits, byte for byte the same as the reference receiver's own
dump of the same file, and the control channel still gives 725/725 TSBK blocks
with a good CRC-16.

**Phase and level.** The cut does not depend on either, and the old and new
chains were run side by side on the same voice capture, resampled and
re-levelled:

| capture variant | frames before / after | IMBE frames | corrected bits |
|---|---|---|---|
| as recorded | 31 / 31 | 261 | 0 |
| clipped at 2x amplitude | 29 / 29 | 261 | 1197 |
| scaled to 0.25 | 31 / 31 | 261 | 0 |
| + noise, 24 dB | 31 / 31 | 261 | 0 |
| + noise, 20 dB | 31 / 31 | 261 | 15 |
| + noise, 16 dB | 31 / 31 | 261 | 85 |
| + noise, 12 dB | 31 / 31 | 261 | 371 |
| + noise, 8 dB | 24 / 24 | 207 | 1046 |

Identical in every row, which is the point: the frame length is not a tuning
constant that trades robustness for cost. One thing that measurement did force:
on a marginal signal 24 sync symbols are a thin estimate of DC and outer level
for the first pass, so when the NID does not decode there the cut is retried
with the levels the previous frame settled on (864 symbols of measurement on
the same channel). That is what keeps the 8 dB row at 24 frames instead of 23;
it costs a second BCH decode on candidates that were about to fail anyway.

## Evidence

Firmware PHY (`tests/host/p25_phy_host`), fed the same files as the prototype:

| capture | result |
|---|---|
| `samples/vc_af_48k.wav` (5.4 s, voice) | 31 frames: 1 HDU, 15 LDU1, 14 LDU2 and the TDU the prototype misses; NAC 0x293; link control 15/15 with talkgroup 1, source 1 and 0 RS symbols corrected; 261/261 IMBE frames, 0 uncorrectable, **0 corrected bits** |
| same file, discriminator inverted | identical: 31 frames, the same fields, polarity detected |
| control channel (`P25_C4FM-CC_BEE0014C_IF.wav`, discriminator converted to 48 kHz) | **725/725 TSBK blocks with 725/725 CRC-16 good**, NAC 0x140 |
| 20 s of contiguous frames built from one LDU1 (`gen_stream.py`) | 112/112 frames, 1008/1008 IMBE frames, 0 corrected bits |

The `syncErrors` counter in the frame struct is the sign errors found in the
sync field at the phase the frame was actually cut at: 0 over the 31 frames of
the voice capture since the cut lands on the eye (it was 18 while the grid was
predicted from the integer start position and frames were re-acquired by hand).
It is a diagnostic, not a defect — a frame whose sync has a sign error still
slices bit-exactly, which is why the IMBE and link-control error counts above
are what the assertions use.

**Zero tolerance against the reference.** The firmware's dump of the voice
capture (261 IMBE parameter sets of 11 bytes, 15 link controls of 9 bytes) is
byte-identical to `work/p25-rx`'s own `--imbe-out` dump of the same file: 0
differences over 261 frames, and the link control bytes are the same 9 bytes on
all 15 LDU1s. Both directions of that comparison are reproducible with
`--imbe-hex` on the harness and `--imbe-out` on the prototype.

The prototype reports 30 frames on the voice capture (it stops at the last LDU2
and misses the terminator); the firmware finds the TDU as well. On the control
channel the last three frames of the file only decode if the stream does not
stop there: they are cut by the end of the recording, and the harness feeds two
frames of silence after the file the way a radio always has silence after a
transmission. Without that padding the last three are reported missing, which is
a property of the file, not of the demodulator.

Host unit test (`tests/unit/p25_phy.cpp`, `test('P25 PHY Test')`): the 1728 bits
of a real LDU1 are shaped into a C4FM waveform at 40 sub-sample phases over one
symbol period (with a residual carrier of 0.15 of the outer level and the
amplitude of a real discriminator), fed in 20 ms blocks, and the PHY has to
return the frame bit for bit: NAC 0x293, LDU1, talkgroup 1, source 1, nine IMBE
codewords with zero corrected bits and zero corrected LC symbols. That is the
regression for the sync defect above: four phases in five used to be lost. The
test also checks an inverted discriminator and that noise produces no frame.

Three more cases in the same file pin the frame cut:

* the link control (9 bytes) and the nine IMBE parameter sets (9 x 11 bytes) of
  that frame have to be exactly the ones the reference receiver decodes from the
  same off-air LDU1 - zero tolerance, 108 bytes, at all 40 phases;
* a real TSDU (`tests/unit/p25_tsdu_bits.h`, 720 bits, 360 symbols, NAC 0x140)
  has to come out with `spanSymbols == 360` and a good TSBK CRC at ten phases
  over the symbol period;
* a train of 12 TSDUs and a train of 8 LDUs have to keep the grid: at most one
  grid miss (the frame after the last one, which is not there) and at most 200
  candidate positions over the whole train. Cutting 864 symbols for a TSDU gave
  one miss per frame and 4650 candidates; the fixed code measures 126 for the
  TSDUs and 132 for the voice train.

The steady-state capture is built by `tests/host/p25_phy_host/gen_stream.py`,
which tiles one real LDU1 back to back at 4800 symbols/s: the demodulator has to
hold its grid for the whole file, which is what a live control channel looks
like. It is also where the 20 s row of the tables under *The frame cut* comes
from.

FEC: the tables were ported from the prototype without being rewritten, and the
exhaustive round-trip self-test that checks them lives there
(`work/p25-rx/src/selftest.cpp`, `make test`): Golay(23,12) over all patterns of
0-3 errors on 512 words (16384 exact, 0 wrong), Hamming(15,11) and Hamming(10,6)
over every single-bit error, RS(24,12) and RS(36,20) over 1-6 and 1-8 symbol
errors, the trellis against the code table, and CRC-16. It is not duplicated in
the firmware tree because the exhaustive checks need the *encoders*, which the
firmware does not carry; in-tree, every decoder is exercised end to end by the
captures above (261 IMBE frames with no corrected bit, 15 link controls with no
RS symbol corrected, 725 trellis blocks with a good CRC-16).

## Cost

QEMU `mps2-an505` (Cortex-M33, `-O2`, the plugin counts executed instructions
per second of audio; `work/dsp-probe/p25phy`), measured before the frame cut was
taken from the frame structure:

| scenario | instructions per second of audio | per 180 ms frame | of 300 MHz at 1 IPC |
|---|---|---|---|
| synced, contiguous frames (steady state) | 23.6 M | 4.25 M | **7.9 %** |
| noise, no signal (the search runs all the time) | 89.1 M | 16.0 M | 29.7 % |
| the control-channel capture (frames 360 symbols apart, so the grid missed and the scan ran every frame) | 93.5 M | 16.8 M | 31.2 % |

The control-channel row is what the frame cut fixed: with the grid holding
(1 miss in 725 frames) that capture stops paying for the ring scan and lands
on the steady-state row instead of the three-times-the-steady-state one. The
host measures the ratio directly, 551 us to 158 us per decoded frame
(**-71 %**); the QEMU row was not re-run here because the probe needs the
cross toolchain under QEMU, so the figure to quote for the M33 is the
steady-state 23.6 M, not a new number.

Where the steady-state cost goes: the matched filter (81 taps at 48 kHz) is
about a third of it, the slicing and the per-frame levels most of the rest. The
first working version cost 109 M instructions per second (36 %) because the
interpolator paid a 64-bit modulo and an FP division per tap and the matched
filter carried two wrap-around tests per tap pair; both are gone (ring length a
power of two with a mask, history written twice so the kernel reads forward).
The frame cut added one NID decode and one 57-symbol slice per frame (the NID
has to be read before the length is known) and removed the extra interpolation
work of a window that was 2.4 frames long on a control channel.

The search is what costs while unsynced, and that is the honest number for a
scanner: 5 sub-phases x 24 interpolations per sample position, one position per
sample. Two thirds of the candidates the old code examined were worse than
that - negative ring positions with nothing written in them, and the walk back
after a grid prediction that could not hold - which is why the same captures
dropped from 339 and 375 candidates per frame to 39 and 5.

### What the front end could still do, measured, not done

* **`p25Frame.evm` is declared and never computed.** The four levels are in
  hand when the frame is sliced, so the RMS distance of the symbols to them
  (what the prototype prints as `evm=0.10-0.11` on the voice capture) is one
  more pass over the frame: **0.76 us per 864-symbol frame on this host**,
  0.2 % of the 418 us the demodulator takes per frame, so it is affordable on
  the M33 and would give the UI a real squelch/quality figure instead of "a
  frame decoded".
* **The trellis decoder takes hard bits.** The slicer already knows how far each
  symbol sat from its level and throws that away; soft input is where a coded
  196-bit block earns its 2-3 dB. That is a change of the FEC interface
  (`p25Fec_trellisDecode()` takes `uint8_t` bits), so it is a project of its own
  and is not in this change.
* **The search still walks one candidate per symbol while unsynced.** A
  correlator that runs on every sample (the matched filter output is already
  there) would find the sync without decimating the hunt to symbols, at 5x the
  correlations: worth measuring on the M33 before believing it, since the
  unsynced case is already 29.7 % of the core.

## Memory

`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`, with and without P25:

| | without P25 | with P25 | delta |
|---|---|---|---|
| FLASH | 518 104 B | 531 272 B | **+13 168 B** |
| RAM | 196 688 B (75.0 %) | 197 760 B (75.4 %) | **+1 072 B** |
| PSRAM | 379 064 B (20.7 %) | 422 360 B (23.0 %) | **+43 296 B** |

The 256 KiB of SRAM is the scarce resource: DMR RX + APRS + NXDN already sit at
80.8 % of it, so the mode keeps only what a sample touches in SRAM and puts the
rest in the 4 MiB of PSRAM (`.psram_section`, NOLOAD — `p25Phy_reset()` clears
it, `p25Speech_start()` resets the queue):

| buffer | size | where | why |
|---|---|---|---|
| matched-filter history (81 taps, written twice) | 648 B | SRAM | one read per input sample, twice the taps |
| PHY state (counters, search position, sync pattern) | ~90 B | SRAM | touched per symbol |
| status struct and mutexes | ~70 B | SRAM | read by the UI |
| ring of matched-filter output (16 384 samples) | 32 768 B | PSRAM | written once per decimated sample, read by interpolation |
| sliced frame, 1728 bits one per byte | 1 728 B | PSRAM | once per frame |
| symbol buffer, 864 floats | 3 456 B | PSRAM | once per frame |
| discriminator block, 20 ms | 3 840 B | PSRAM | once per block |
| speech queue and output buffer | 1 504 B | PSRAM | 50 Hz |

The decode tables (`P25Tables.c`, 4 612 B) are `const` and therefore in flash,
not in RAM. The IMBE decoder state, when one is linked in, belongs in PSRAM too
(`P25Vocoder.c`); the built-in decoder's instance is 11.6 KiB there (mbelib-neo's
`mbe_parms` trio is 4.6 KiB).

If RAM ever needs to be cut further, the candidates in order are: the ring
(16 384 samples is 3.8 frames; two would do with 8 192, at the cost of the
rescan window that recovers a frame which does not follow the grid), the
speech queue depth, and the discriminator block (10 ms instead of 20 ms).

## Voice, licences and patents

`P25Vocoder.c` is the hook: 88 IMBE parameters (11 bytes, packed MSB first,
already FEC-corrected) in, 160 samples of 8 kHz PCM out. **The decoder now ships
in this tree**: `lib/imbe_vocoder`, our own C port of mbelib-neo's IMBE path,
**195 k instructions per 20 ms frame (3.9 – 5.2 % of the M33)**, in the default
`-DCONFIG_C62_P25=y` image. `docs/IMBE.md` has the bit allocation, the
verification against the reference on 261 real off-air frames plus 1000 encoder
frames, the sizes, the licence and the encoder verdict.

The two candidates evaluated with the host prototype before that port, one of
which is kept as a selectable backend (`-DCONFIG_C62_P25_MBELIB=y`):

* **mbelib-neo** — the decoder `work/p25-rx` uses, validated frame by frame
  against dsd-neo on the captures above. Licence: GPL-2.0-or-later. Cost:
  9.36 M instructions per 20 ms frame, more than the 6 M the frame has, so it
  cannot run in real time on this MCU; that measurement is what motivated the
  port. Its rendering (DVSI's 10 ms subframes) is what the port measures itself
  against in `docs/IMBE.md` §4.
* **op25's `imbe_vocoder`** — fixed-point C++, GPL-3.0-or-later. Not needed: our
  port is integer-free float like the AMBE+2 one and already fits.

**Licence compatibility.** The firmware is GPL-3.0-or-later. mbelib-neo as
GPL-2.0-or-later can be combined with it (GPLv2-or-later code may be taken
under v3), and op25's vocoder is already v3. The rest of the mode — PHY, FEC,
tables — is our own code under the tree's licence and carries no third-party
terms.

**Patents.** IMBE and its AMBE relatives (TIA-102.BABA, the 4400 bit/s vocoder
this mode decodes) are covered by the DVSI patent family, of which the two most
cited are **US 5,701,390** (multi-subframe quantisation of the spectral
parameters, expired) and **US 8,359,197** (the 2:1 interleaving of the pitch
and voicing parameters, running to about 2028). This is a statement of what the
literature and the patent record say, not a legal opinion, and it applies
wherever software patents are enforceable; the user considers them inapplicable
in their jurisdiction and has asked not to treat this as a blocker. Which
decoder to link, if any, is the user's decision.

## UI hooks

No file under `openrtx/src/ui/` was touched. What the mode needs there, in the
order it matters:

* **the mode selector**: key 5 cycles FM → M17 → TETRA → DMR. P25 needs its own
  stop in that cycle (and in whatever list the new UI keeps); `opmode_canTransmit()`
  already returns false for it, so the PTT path is safe before the UI lands.
* **the status screen**: `p25Status_get()` gives everything a screen needs —
  NAC, DUID, talkgroup, source, `encrypted`, and the counters (frames, IMBE
  frames, corrected bits, TSBK with a good CRC). `p25Status_sync()` drives the
  sync indicator; the mode itself lights the green LED on a decoded frame.
* **the quick menu (F2 short)**: frequency first, then, read-only, NAC and
  talkgroup. The mode takes ONE frequency, like TETRA: RX only.
* **the channel editor**: mode = P25 with a single frequency; nothing else to
  edit (no colour code, no timeslot).

Until the selector entry exists, the mode is reachable by setting
`settings.opMode` to 7 (the remote commands and the settings record both carry
it).

## Not tested

* **The frame lengths other than the LDU and the TSDU.** Those two are measured
  frame after frame on the captures (863-865 and 359-361 symbols between two
  syncs, 725 and 261 frames). The HDU's 396, the TDULC's 216 and the TDU's 72
  come from the frame structure *and* from one capture each (396 symbols
  HDU->LDU1 on the voice file; 216 and 72 on the APCO data capture), so a system
  that pads its headers differently would still decode - the cut would be
  short and the grid would miss - but the numbers are less worn than the other
  two.
* **On the air.** Every number above comes from captures on a host and from
  QEMU. The firmware has not decoded P25 with the radio's own discriminator:
  the audio path, the receiver's filters in this mode and the sample rate the
  codec actually delivers are unverified. The DMR experience applies — if the
  files pass and the radio does not, look at the audio chain (high-pass,
  de-emphasis, AGC, block continuity, inverted discriminator) before the
  demodulator; `p25Phy_reset()` on a lost block and the polarity detection are
  the two hooks already in place for that.
* **Voice**. No decoder is linked in, so the playback path is compiled and
  reviewed but has never produced audio. The queue, the thread and the silence
  concealment are exercised only by construction.
* **Trunking.** The TSBK block is decoded and the CRC checked, and the control
  channel capture yields 725 blocks, but nothing follows a grant, tracks a
  neighbouring site or retunes. There is no `radio_setRxFreq()` in this
  firmware, so a follow would need one first (the same gap TETRA has).
* **Encryption.** The header's algorithm id and the service options are shown;
  nothing decrypts, and an encrypted call decodes as noise, which is expected.
* **Field decode of every TSBK opcode.** The block is validated by CRC and the
  opcode is shown; the individual fields are decoded for the twelve opcodes the
  prototype needed (grants, registration responses, the three status
  broadcasts), not for all of them.
* **HDU/MI and RS(36,20) beyond the header.** The header's Golay words and the
  RS(36,20) are decoded; the message indicator is not checked against a
  re-encryption, because there is nothing to re-encrypt with.
* **Rate and levels of a real system.** The demodulator slices on levels
  measured per frame, so deviation and level differences between radios are
  absorbed; what is not measured is how far off a real system can be before the
  sync is lost (the capture that decodes is the only one available).
