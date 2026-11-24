# DMR receive mode

> **Superseded in the firmware.** The receive chain this document describes
> (DmrPhy/DmrFec/DmrSpeech) was replaced by the protocol package in
> `protocols/DMR/`; see `docs/DMR_PKG.md`. The measurements and the reasoning
> here remain valid for the vocoder and for what the package had to reproduce.

DMR (ETSI TS 102 361-1) receiver for the Retevis C62: 4FSK demodulation of the
radio's discriminator output, slot synchronisation, CACH, colour code and
timeslot, and voice through the AMBE+2 decoder. This document covers the
receive side; the transmitter is `docs/DMR_TX.md` and is off by default
(`CONFIG_DMR_TX=n`) until an AMBE+2 encoder exists.

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
tests/platform/c62_dmr_phy_cost.c    bare-metal cost harness (QEMU mps2-an505)
tests/unit/assets/dmr_air_bursts.wav two real voice bursts, for the air test
```

The protocol layer is C, like `protocols/TETRA`: the prototype was C++17 for
the convenience of `std::vector` on the host, and none of it survives the port.
`OpMode_DMR` is C++ because every mode handler is, and it does nothing but move
buffers.

## The chain, as it runs on the radio

```
48 kHz discriminator (BK4819 audio bypass, 25 kHz flat RX path)
  RRC α=0.2 matched filter, 81 taps, evaluated at 24 kHz (5 samples/symbol)
  cheap aperture pre-gate, 5 sampling instants over one symbol period
  sync search on the 4 slot words, 7 positions per 30 ms slot once locked
  per-burst fractional timing: 16 phases of a 4-point Lagrange interpolation
       over ±0.5 symbol, metric sync correlation − 0.35·EVM
  levels: median DC, then 4-means (outer, inner = outer/3) with a drift term
  polarity: Golay(20,8) of the 20 bits flanking the sync, latched by majority
  gate: EVM ≤ 0.42, CACH ≤ 1 correction, aperture ≥ 70 % of the sync window
  gap fill: the EMB bursts of a superframe are coasted on the 60 ms slot grid
  CACH → AT/TC/LS        slot type → colour code and data type
  voice: 216 payload bits → 3 × 72-bit AMBE+2 frames (no extra FEC)
  data:  BPTC(196,96) → 96-bit block, CRC-16 per mask
  voice → DmrSpeech queue → playback thread → dmr_vocoder_decode() → 8 kHz
```

The demodulation is the prototype's: fractional timing per burst, a cheap
spectral-aperture pre-gate, four-level fitting that does not depend on the
payload mix, polarity from the channel structure, a quality gate and a
conservative gap fill. What differs is what a streaming receiver has to do
differently, and there are five things:

1. **Matched filter at 24 kHz.** The prototype filters at 48 kHz (3.9 MMAC/s);
   the firmware decimates by two while filtering (1.94 MMAC/s) and interpolates
   the 24 kHz stream with the same four-point kernel. Five samples per symbol
   are enough for a 4FSK eye with α = 0.2; the interpolation, not the sampling
   rate, is what places the instant.
2. **The sampling instant is carried, and re-measured.** The fractional offset
   is carried from burst to burst and the coarse phase steps by two samples
   when it reaches ±4, so the symbol clock of the capture is followed. The
   prototype re-acquires it per burst around a phase it fixed on the first
   seconds of the file, and on a 34 s capture with 18 ppm of clock error that
   is not enough: its EVM climbs 0.02 → 0.39 and the frames degrade. Carrying
   it has one failure mode — the estimate drifts, and then the sync search
   samples the wrong instant and loses *every* burst, not just the weak ones —
   so the pre-gate measures the window at five instants a fifth of a symbol
   apart and takes the one whose levels are clearest. It costs three passes
   over 24 samples of the decimated stream and it makes the acquisition
   independent of the carried offset.
3. **Slot grid.** After a burst, the next sync can only be a whole number of
   144-symbol slots away. The firmware searches 7 positions around each
   predicted slot instead of all 4800 symbols/s (20 times less CPU) and, more
   important, a spurious correlation in the middle of a burst can no longer
   shadow the real sync of the next slot — that was the one false positive the
   prototype produced in the synthetic capture, and it costs three frames of
   audio each time. After twelve empty slots the grid is dropped and the scan
   goes back to free search; that is what picks up a second, unsynchronised
   transmitter, and the sigidwiki capture is the case: its data channel runs 72
   symbols off the voice slot grid, and nothing decodes it while the grid is
   held.
4. **The gap fill has no future.** The prototype fills a hole of the chain it
   has already built: it knows the burst after the hole, and it knows that the
   hole is short. A streaming receiver has neither. It decodes the slot when it
   goes by, holds up to four candidates, and emits them when the run closes on
   a *voice* burst — a data burst means the run was not this call — and drops
   them when the run reaches the grid timeout. The gates are what keep it
   honest: the coasted burst must have a Golay(20,8) field within distance two,
   an eye below 0.50 and a payload that is not a constant. A slot the call does
   not use fails at least one of the three (an empty slot decodes to all-zero
   bits, whose codeword *is* a valid Golay word, which is why the payload check
   is there).
5. **Burst by burst, not per superframe.** The prototype decodes a whole
   superframe in one call because it has the file; the firmware hands each
   burst over as soon as its last symbol is in, ~16 ms after the sync.

## Cost

Measured, not estimated: the demodulator alone on a QEMU `mps2-an505` (Cortex
M33, single-precision FPU, no DSP extension) with `tests/platform/c62_dmr_phy_cost.c`,
which pushes discriminator audio through `dmrPhy_push()` and drops a frame
marker every second of audio (`qemu-mcu-instruction-profiling`). The input is
12 s of a continuous voice call (one burst every 60 ms, 16.7 bursts/s), which
is the worst case: a scan that finds nothing is cheaper than one that decodes.

| stage | instructions per second of audio | % of 300 MHz |
|---|---|---|
| matched filter, 81 taps at 24 kHz, and the per-sample path | 11.2 M (1.94 M MAC at 5 instructions each) | 3.7 % |
| fractional timing, 4-level fit, EVM, one per burst | 6.0 M | 2.0 % |
| aperture pre-gate, sync correlation | 1.6 M | 0.5 % |
| CACH, Golay, BPTC, CRC, de-interleaving | 1.5 M | 0.5 % |
| **PHY total** | **20.2 M** | **6.7 %** |
| **vocoder** (measured, `docs/DMR_VOCODER.md`) | 751 k instructions per 20 ms frame | **15–20 %** |

For reference, the chain this replaces — integer timing, sync-only levels, no
polarity, no fill — measures **20.9 M instructions/s (7.0 %)** with the same
harness, so the whole of the new demodulation fits in less than the old one
cost: the matched filter was made to read a mirrored history (the 81 taps are
contiguous, no ring mask per tap) and the four sync patterns are expanded once,
which pays for the timing search, the 4-means and the gates. `-Os` instead of
`-O2`, which is what the Zephyr image uses, measures 20.6 M (6.9 %).

Two thirds of what is left is the matched filter, and it is a float dot product
of 81 taps per output sample: on a Cortex-M33 without DSP extension there is no
way around 1.94 M multiply-accumulates per second, and the loop already runs at
five instructions per tap (two accumulators, four taps per iteration). A
`vldm`-based inner loop in assembly would get to three and save 2 %; the
vocoder, at 15–20 %, remains three times the cost of the whole demodulator.

### AP or HiFi4

Everything on the AP. The PHY is 6.7 % and the vocoder 15–20 % of one core; the
HiFi4 would only be worth waking for a fixed-point vocoder or the stock
denoiser, and both are separate projects. What must stay on the AP is the
control logic (slot grid, FEC, state machine) — small, stateful, branchy. If
anything ever moves to the DSP it is the per-sample RRC, which SIMD likes.

## Memory

Measured with `-DCONFIG_C62_DMR_RX=y` (`scripts/c62/build.sh`):

| build | FLASH | RAM |
|---|---|---|
| no DMR (previous measurement) | 394 968 B (37.67 %) | 143 144 B (54.61 %) |
| **DMR RX, previous chain** | 440 332 B (41.99 %) | 163 176 B (62.25 %) |
| **DMR RX, this chain** | **442 976 B (42.25 %)** | **164 688 B (62.82 %)** |

The new demodulation costs **+2 644 B of flash** and **+1 512 B of RAM** over the
previous chain. The RAM is the slot window the 4-means works on (144 floats,
576 B), the doubled 48 kHz history that makes the filter taps contiguous
(+512 B) and up to four coasted bursts held until the run of holes closes
(~400 B). The image embeds `git describe`, so its size depends on the commit it
is built from; the figures above are from the build of the commit that carries
this paragraph.

`-DCONFIG_C62_DMR_VOCODER=y` on its own is not a measurable configuration: the
option builds `lib/dmr_vocoder` but nothing references it, so `--gc-sections`
drops every object and the image is the no-DMR one, byte for byte. The vocoder
is only paid for when the mode links it, which is what the DMR RX row shows.

The mode costs **+48 008 B of flash** (vocoder, PHY, FEC, speech, status,
handler) and **+21 544 B of RAM** over a firmware without DMR. The RAM is:

| object | bytes | note |
|---|---|---|
| vocoder instance | 8 540 | `.bss.dmr_instance`: AMBE parameter sets, arena and DCT cache |
| PHY state | 6 064 | `.bss.phy`: 1 024-sample 24 kHz ring, 144-symbol slot window, 48 kHz history, four held bursts |
| playback buffer and queue | 928 | 2 × 160 samples of PCM, 32 frames of AMBE |
| everything else in the mode | ~5 900 | thread stacks, audio path, status, handlers |

The vocoder is a single 8 540 B object: its arena was trimmed to the measured
high-water mark and the 23 KiB AMBE DCT cosine cache, a constant table that was
built at run time, now lives in flash. That is the whole difference between the
198 600 B / 75.76 % of the first port and today's 163 176 B / 62.25 %.

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

Everything below is measured with `tests/platform/c62_dmr_host.c` on the
captures in `work/dmr-rx/out/`, against the host prototype
(`work/dmr-rx/build/dmr_rx`) on the same files and with the same metrics.

| capture | chain | bursts | voice | CRC-16 | mean EVM | worst gap |
|---|---|---|---|---|---|---|
| sigidwiki, 34 s | prototype | 116 | 42 + 82 filled | 37/37 | 0.075 | 620 ms |
| | **firmware** | **128** | **53 (43 + 10 filled)** | **37/37** | **0.056** | **700 ms** |
| dmr_t3_cc, 2 s | prototype | 66 | — | 33/33 | 0.026 | — |
| | **firmware** | **66** | — | **33/33** | **0.027** | — |
| dmr_bs_weak, 5.4 s | prototype | 106 | 97 | — | 0.27 | 100 ms |
| | **firmware** | **85** | **78 (77 + 1 filled)** | — | 0.27 | 180 ms |

* **Bit-exact against a known transmitter.** The synthetic capture of the
  prototype (real speech → AMBE+2 → DMR modulator) decodes to **300/300 frames
  identical** to the frames the modulator started from, and it is still 100
  bursts for 100 sent: the gap fill does not invent a burst in a clean,
  continuous capture. That covers the matched filter, sync, timing, CACH,
  de-interleaving and the frame layout.
* **Real signal, sync.** On the sigidwiki repeater capture the chain decodes 118
  bursts from its own sync search and **117 of them with at most one sign error
  out of 48** (the remaining one has two). The prototype's 116 are all at one or
  zero. The extra bursts are the data channel of a second transmitter, 72
  symbols off the voice slot grid: the previous chain never unlocked from its
  grid and could not see it, this one does after twelve empty slots. Both
  chains get the same **CRC-16 37/37** on those data bursts, which is the hard
  bit-level check (a 96-bit block does not pass by chance).
* **Real signal, polarity.** The 20 bits flanking the sync are decoded as a
  Golay(20,8) word under both polarities and the majority latches the
  discriminator's sign. That is what makes an inverted capture decode as voice
  instead of as the complementary data sync word; on these three captures the
  vote comes out normal, as the prototype's does.
* **De-interleaving, against the reference receiver.** 44 of the firmware's
  bursts also decode in the prototype; of their 132 frames, **126 are
  byte-identical** (BER 2.2 %, 6 frames differ, all of them in bursts either
  side decodes at EVM > 0.15). **138 of the firmware's 159 frames pass the
  AMBE+2 Golay parity with no corrections at all** (87 %), against 199 of the
  prototype's 372 (53 %): the prototype fills every hole of its chain, the
  firmware only coasts a slot when the Golay field says a burst is there, so it
  decodes fewer frames and a larger fraction of them is real.
* **Continuity.** On `dmr_bs_weak` — a real call, weak (EVM 0.27), with gaps —
  the firmware decodes **4.46 s of voiced audio in 16 segments** where the
  prototype decodes 4.02 s in 21, and its worst gap inside the transmission is
  180 ms against 100 ms. On sigidwiki (one voice burst every 360 ms, so most of
  the timeline is empty air) both are dominated by where the capture has no
  signal at all: 620 ms worst gap for the prototype, 700 ms for the firmware.
* **Air regression.** `c62_dmr_host -s tests/unit/assets/dmr_air_bursts.wav`
  decodes two real voice bursts of that capture and requires all six frames to
  be the reference receiver's frames and to be accepted by the Golay parity
  (no network, no prototype build needed). This is the check the synthetic loop
  cannot do, because the test generator and the de-interleaver share the same
  tables: swapping the dibit or the MSB order in
  `dmr_fec_voice_deinterleave()` still gives 300/300 on the synthetic capture
  and 0/6 here.
* **Vocoder.** `dmr_vocoder_decode()` and OpenDMR's `opendmr_decode()` produce
  byte-identical PCM for the same frames (checked frame by frame).
* **Audio level.** On sigidwiki the harness reports **RMS 2 878, peak 31 128**
  for the decoded frames (prototype: 2 081 / 31 128 over its own timeline). The
  decoder is stateful, so the harness keeps it warm across gaps in the same way
  `DmrSpeech` does: only 1 s of silence or more resets it
  (`DMR_NEW_CALL_FRAMES`).
* **Cost and memory.** 20.2 M instructions per second of audio on a Cortex-M33
  (previous chain: 20.9 M), 442 976 B of flash and 164 688 B of RAM.
* **Firmware.** Builds with the Zephyr flow, no warnings from the new files;
  the M17, TETRA and FM modes are untouched and still link in the same image.

Reproduce the host part:

```sh
cmake -S lib/dmr_vocoder -B build_dmr_vocoder && cmake --build build_dmr_vocoder
cc -O2 -I openrtx/include -I lib/dmr_vocoder tests/platform/c62_dmr_host.c \
   openrtx/src/protocols/DMR/{DmrDefs,DmrFec,DmrPhy}.c \
   build_dmr_vocoder/libdmr_vocoder.a -lm -o c62_dmr_host

# real-air regression (what CI would run if this had a CI)
./c62_dmr_host -s tests/unit/assets/dmr_air_bursts.wav

# whole capture: PCM, per-burst metrics, and the frames for an A/B against
# work/dmr-rx/build/dmr_rx --ambe-out
./c62_dmr_host -o out.wav -c bursts.csv -a frames.txt \
   work/dmr-rx/out/discr_sigidwiki.wav
```

Reproduce the cost figure (requires `arm-none-eabi-gcc`, `qemu-system-arm` and
the TCG plugin of `work/dsp-probe/common/insnframe.c`):

```sh
D=work/dsp-probe
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard \
   -O2 -ffunction-sections -fdata-sections -DMARK_FRAMES \
   -DWAV_PATH='"/tmp/cost.wav"' -Iopenrtx/include \
   tests/platform/c62_dmr_phy_cost.c openrtx/src/protocols/DMR/{DmrPhy,DmrDefs,DmrFec}.c \
   -T $D/common/mps2.ld -specs=rdimon.specs -Wl,--gc-sections \
   $D/common/startup.o -lrdimon -lm -o /tmp/cost.elf
INSNHIST_MARKER=$(arm-none-eabi-nm /tmp/cost.elf | awk '$3=="frame_marker"{print $1}')
INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
   -semihosting-config enable=on,target=native -kernel /tmp/cost.elf \
   -plugin file=$D/common/libinsnframe.so -d plugin
```

## Capturing what the radio hears

Everything above is verified on files. The one question files cannot answer is
what the BK4819 hands to the demodulator on this particular receiver: level,
bandwidth, continuity. The baseband dump exists for that, and it is the first
thing to run when a digital mode decodes nothing (or decodes badly) on the air.

**It is a build option, and it is off in the default image**
(`-DC62_BASEBAND_DUMP=ON` for a capture session): compiled in it holds 1 MiB of
PSRAM — 77,8 % of it with the dump against 20,7 % without — for something that
is used once in a while. A build without it says so at power on:
`C62: baseband dump not built in`.

**Arm it from the keypad**: hold the A/B key (the red one, `KEY_ESC`) while
switching the radio on. The log prints `baseband dump armed` and is muted from
then on; without the key the dump is inert and the console is a normal console.
The first capture starts when a mode opens the radio receive path, so switch to
DMR, tune the channel and wait for the bursts.

**Get the samples out**: the console UART is the Kenwood programming cable
(`/dev/ttyUSB0` with the usual USB-serial adapter), and the default build fills
1 MiB of PSRAM (10.4 s of 48 kHz audio) and then sends it in one burst, gapless,
at 115200 baud, taking about 91 s:

```sh
scripts/c62_bbdump.py -p /dev/ttyUSB0 -t 100 -o dmr_air
```

Each burst gives `dmr_air_burst000.wav` (48 kHz mono, exactly what
`dmrPhy_push()` receives), a `.f32` for GNU Radio, a per-packet `.csv` with
REG_43 and RSSI, and `dmr_air_radio.txt` with the radio's own report: the
BK4819 registers read back over SPI when reception started, and once a second
the level into the codec ADC and the number of buffer halves the reader lost.

Raise the rate if the adapter allows it: `-DC62_BASEBAND_DUMP_BAUD=1500000`
with the s16 format, or 921600 with `-DC62_BASEBAND_DUMP_FORMAT=2` (12-bit
packed, 16 dB of dynamic range less, still enough to see a 4FSK eye).

**Decode it on the host**, which separates a PHY defect from an audio-path one:

```sh
make -C tests/host/dmr_phy_host
tests/host/dmr_phy_host/build/dmr_phy_host dmr_air_burst000.wav   # the firmware code
/home/n0p/src/c62/work/dmr-rx/build/dmr_rx dmr_air_burst000.wav --no-wav  # the prototype
```

* Both decode it, and the radio's live numbers are worse: the firmware is
  losing blocks or the blocks are stale, not the signal (check `misses=` and
  `rxpk=` in the radio report, and the `sequence gaps` count of the tool).
* Neither decodes it: the analog path is the problem, and
  `tests/host/dmr_phy_host/run.sh` says which defect produces the numbers the
  radio showed. A baseband high-pass is the one that keeps the sync and the
  CACH intact while destroying the payload, because the 4FSK slicer needs the
  absolute levels, not the sign.
* Only the firmware decodes it: a timing or level bug in `DmrPhy.c`; the
  prototype's `--csv` gives the per-burst offset and EVM to compare.

## When DMR decodes badly: the value to try for each symptom

The full study is in `reports/dmr_rx_hw.md` (outside this repository); this is
the part to keep at hand when a capture has been made and the radio still does
not decode. Decode `dmr_air_burst000.wav` on the host first — that separates
"the signal is bad" from "the radio is losing it".

| What the capture shows | Diagnosis | What to change, and to what |
|---|---|---|
| The host decodes (CRC >= 90 %) and the radio does not | the signal is fine, blocks are lost or late | `misses=` in `dmr_air_radio.txt` and the `sequence gaps` of the dump. `misses` growing: raise the rtx thread priority (`Rtx` in `openrtx/src/core/threads.c`) or cut the per-slot cost. Dump gaps: raise the dump baud rate (extraction only, it does not affect decoding) |
| `rxpk` pinned at 32767, eye flat at the extremes | codec ADC clipping | `C62_ADC_RX_GAIN_D` 20 -> 14 in `platform/targets/c62/hwconfig.h`; if it persists, `REG_48<9:4>` (AF Rx Gain2) `0x3f` -> `0x33` in `radio_C62.cpp` |
| `rxpk` below 5 % of full scale (< 1600) | too low for the slicer | `REG_43<2> = 1` (+6 dB after the demodulator): `0x7e28` -> `0x7e2c`; or `C62_ADC_RX_GAIN_D` 20 -> 26 |
| Spectrum falling below 100-300 Hz (compare with the PSD of `dmr_t3_cc`) | **baseband high-pass** | this is what the current code fixes: `REG_7E<2:0> = 0b000` for DMR, i.e. the DC filter off, same as M17. If it persists with the fix in, the high-pass is before the BK4819 or in the DSP: write `REG_7E<2:0> = 0b000` explicitly after `bk4819_enableRx()`, and if that does not help, leaky-integrate in `dmrPhy_push()` |
| Strong tone at 16,9 kHz, rest of the spectrum low | IF DC offset artefact, the reason TETRA filters it | re-enable `REG_7E<2:0> = 0b110` for DMR only and lower `C62_ADC_RX_GAIN_D`, or notch 16,9 kHz in the DMR path |
| Neighbouring channels inside the filter | `REG_43` too wide (25 kHz mode doubles the RF filter: 7 -> 9 kHz) | build with `-DC62_DMR_12K5=ON`: 12.5 kHz channel mode and `REG_43 = 0x7e48`, what the stock firmware uses for its own 4FSK. Alternatives: `0x7828` (25 kHz, RF 4/4) or `0x7808` (12.5 kHz, RF 7/4, as M17 without `C62_WIDE_RX`) |
| Eye open but the prototype's `timing` goes past 0.3 symbol | sample clock or repeated blocks | compare the sample index of the `.csv` with the dump's; a jump means the stream (first row) |
| None of the above, and the host does not decode either | what arrives is not DMR, or is far too degraded | check with the radio's own transmitter (`CONFIG_DMR_TX=ON`, local loop to another frequency) that the receiver decodes what it sends |

The signature that identified the hardware fault is worth remembering, because
it is easy to misread as "bad RF": with a 100-300 Hz baseband high-pass the sync
and the CACH survive (they are two-level and sign-based), the colour code and
the timeslot start jumping (the 20-bit Golay field fails), and the payload CRC
falls to 0/32 while the EVM stays at 0.17-0.25 — under the 0.42 quality gate, so
the bursts are accepted with garbage inside. `tests/host/dmr_phy_host/run.sh`
reproduces each distortion on a clean capture and prints the same figures the
radio shows, which is how the two were matched.

Registers worth reading back on a bad capture, all through the dump's control
channel (`regs` line in `dmr_air_radio.txt`): `0x43` (channel mode and IF
filters), `0x7E` (baseband DC filters), `0x48` (AF RX gains), `0x10`-`0x14`
(RX AGC gains), `0x2B`/`0x31`/`0x47`/`0x4B` (AF filters, compander, output, ALC)
and `0x73`.

## Limitations

* **No TX in this mode's default build.** The transmitter exists
  (`docs/DMR_TX.md`) but `CONFIG_DMR_TX` is off until an AMBE+2 encoder is
  linked in; the encoder does not fit in the M33 as it is (4.2 M instructions
  per frame, 84–112 % of the core, `docs/DMR_VOCODER.md`).
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
* **Not yet verified on the radio.** Every number above comes from files decoded
  on the host with the same C code the firmware runs. What the BK4819 of a real
  C62 hands over (level, bandwidth, continuity) is exactly what the baseband
  dump captures — see *Capturing what the radio hears* — and DMR RX on hardware
  is what `reports/dmr_rx_hw.md` is about.

## Next step: transmit

The transmitter itself is done — `docs/DMR_TX.md` has the chain, the state
machine, the costs and the loopback (150/150 frames, both layouts). What is
left on the receive side is what the two halves still need:

1. **RS(12,9) decode** for the LC: source and destination IDs on the screen,
   and a real call-state machine (header → voice → terminator) instead of the
   300 ms sync timeout. The encoder half already exists
   (`dmr_fec_rs129_encode()`), so the GF(256) is half written.
2. **The AMBE+2 encoder**, which is the only thing between
   `CONFIG_DMR_TX=y` and a radio that talks: see `docs/DMR_TX.md`.
3. **Deviation calibration** of `REG_40` against 1944 Hz and an adjacent
   channel measurement, which need the bench, not more code.
