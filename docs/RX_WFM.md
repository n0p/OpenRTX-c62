# Broadcast FM reception (WFM), 85.0-108.0 MHz

Commercial FM broadcast, receive only, on the C62's dedicated broadcast
receiver. The band object is 85-110 MHz as requested; the chip stops at
108 MHz, so the plan does too and the reason is written down below.

- Mode: `OPMODE_WFM_BC = 11` (`openrtx/include/rtx/rtx.h`).
- Driver: `platform/drivers/baseband/BK1080.c` (register layer) and
  `radio_C62.cpp` (power, tune, audio path, status).
- Band, channels and status types: `openrtx/include/core/wfm.h`,
  `openrtx/src/core/wfm.c`.
- Mode handler: `openrtx/src/rtx/OpMode_WFM.cpp`.
- Zone: `ZONE_WFM_BC`, `BC85.0` .. `BC108.0` in `openrtx/src/core/zones.c`.
- Test: `tests/unit/wfm.cpp`; cost harness: `tests/platform/c62_wfm_cost.c`.

## 1. Hardware

The radio carries two receivers. The BK4819 is the transceiver: its widest
IF filter is 25 kHz (4.5 kHz doubled), so it cannot receive a 200 kHz wide
broadcast channel with usable audio. Next to it, on its own supply switch,
there is a **Beken BK1080** — a complete FM broadcast receiver: LNA, image
reject mixer, low-IF ADC, digital demodulator, stereo MPX decoder, de-emphasis,
soft mute and two audio DACs.

| Signal | SoC pin | Notes |
|---|---|---|
| BK1080 SCLK | GPIOA 5 (pin 51) | bit-banged clock, `platform/drivers/I2C/i2c_bitbang.c` |
| BK1080 SDIO | GPIOA 6 (pin 52) | bidirectional data |
| BK1080 PWR  | GPIOB 2 (pin 22) | **active low**: low = chip powered |
| BK1080 LOUT/ROUT | — | to the analog volume pot, then to the LM4898 input |
| BK1080 FMIN | — | broadcast antenna input, matched by L1/C5 on the RF sheet |

Source: `C62_Teardown/mcu_connections.md`, `C62_Teardown/README.md`,
`extract/schematics/C62_RF.txt` (net `FMPOWER`, `I2C_DATA`, `FMVOICE`), and the
stock firmware (section 2).

Bus protocol: the datasheet's "I2C" is not standard I2C. The first byte is a
fixed device ID `0x80` (a 7-bit address of 0x40 with the write bit), and the
direction is carried in the *control* byte that follows, `(reg << 1) | rw`.
Registers are 16 bit big endian and the address auto-increments, so one frame
reads or writes a run of registers. That is why the bus is bit-banged
(`i2c_bitbang.c` was written for this chip) and not driven by the SoC I2C
controller. The chip accepts up to 2.5 MHz; the build clocks it at 250 kHz
(`clkPeriod = 2` us in `platform/targets/c62/hwconfig.c`), ten times slower
than it can go, because `delayUs()` below 1 ms is a busy wait on this SoC and
every microsecond of half-period is CPU time (section 6).

### Audio path: not the codec, not the DSP

The BK1080 outputs **analog** audio on LOUT/ROUT. In this radio those go to the
volume potentiometer and then to the input of the LM4898 speaker amplifier, in
parallel with the codec's own contribution (`RX_VOICE`, resistor-summed with
`FMVOICE`). Nothing of the broadcast audio goes through the CSK6 codec, the
HiFi4 DSP or the CM33:

```
BK1080 LOUT/ROUT -> RV1 volume pot -> R26/R53/FB1 summing node -> LM4898 -> speaker
CSK6 codec DAC  -------------------------------------^ (other leg of the same node)
BK4819 AF       -------------------------------------^ (muted in this mode)
```

Consequences for the software, and what the mode does:

1. `audioPath_request(SOURCE_RTX, SINK_SPK)` is **not** used: it would start the
   codec DAC and mix it into the same amplifier input. The speaker switch is
   driven instead through `audio_setExtSource(true)` (`audio_c62.c`), a flag
   OR-ed into `updateSpeaker()`.
2. The BK4819's AF output is muted (`BK4819_AF_MUTE`, REG_47) so the
   transceiver cannot add noise under the station, and `radio_enableAfOutput()`
   refuses to unmute it while the mode is active.
3. Volume is the analog pot: the chip's digital volume stays at 0 dBFS
   (REG_05 VOLUME = 15), exactly as in the stock firmware.
4. Entering and leaving the mode is pop-suppressed the way the stock firmware
   does it: the tuner is muted across a retune, and the amplifier is switched
   on after the tuner is tuned and off before it is powered down.

This is not speculation: `forks/c62-analysis/fm-audio-path.md` records the
stock sequence *and* a hardware check with a probe build (`c62/fm-probe`,
WJRR 101.1 MHz) where FM audio came out of the speaker and the volume knob
worked. What is new here is the driver, the mode and the status readout.

## 2. What the factory firmware does (verified in the image)

Checked against `dumps/extract/app-cm33.bin` (the first 735,380 bytes of
`dumps/factory-c62.bin`, XIP at 0x18000000), not only against the earlier
reconstruction:

| Item | Address | What it is |
|---|---|---|
| 34-register init table | 0x180a3514 | `uint16_t[34]`, REG_00..REG_21, `{0x0008, 0x1080, 0x0201, 0x0000, 0x40C0, 0x0A1F, 0x002E, 0x02FF, ...}` |
| `bk1080_init` | 0x18024bf8 | `delay(1000)`, I2C pin setup, read REG_01, compare with 0x1080 (prints `fm_radio_Read(0x01) = 0x%02x%02x` at 0x180a3558), convert the table to bytes, write 0x44 bytes from REG_00 in one burst, wait 250, pulse REG_25 bit 7 (`0x283C` then `0x28BC`), wait 60 |
| `bk1080_tune` | 0x18024cac | `chan < 760` (76.0 MHz) ? REG_05 = `0x0ADF` (BAND=11, 64-76) : `0x0A5F` (BAND=01, 76-108); CHAN = freq - 760 (or - 640); clear SEEK, write CHAN, write CHAN with TUNE |
| `fm_radio_set_freq` | 0x18024e18 | Hz to 100 kHz channel, then tune |
| station check | 0x18024d30 | REG_07 FREQD/SNR plus REG_0A flags, with a previous-channel comparison to reject images |
| `bk1080_power_down` | 0x18024df0 | REG_02 = `0x0241` (ENABLE + DISABLE + SEEKUP), power pin high |

Reading that back:

- **BAND = 01 (76-108 MHz), SPACE = 01 (100 kHz), VOLUME = 0xF** on every tune
  to anything at or above 76 MHz. BAND = 00 (87.5-108) is never used, which is
  what makes 85.0-87.5 MHz reachable here at all.
- The pulse after the table is **bit 7 of REG_25** (`0x28BC` -> `0x283C` ->
  `0x28BC`). The first version of the OpenRTX driver pulsed bit 15
  (`0xA8BC`), a value the stock image never writes; this driver follows the
  factory table and keeps the pulse on bit 7.
- The 1000 ms wait at the top of the stock init is a fixed delay before the
  first bus access. This driver polls REG_01 instead (10 ms steps, up to 1 s,
  same bound) because the chip answers as soon as its reference clock is
  stable — normally on the first or second poll. The 250 ms and 60 ms waits
  around the calibration pulse are kept: they are part of the calibration
  sequence, not of the power-up settle.

One deviation from the factory table is deliberate: **REG_04 = 0x48C0 instead
of 0x40C0**, i.e. DE = 1, the 50 us de-emphasis Europe transmits (the factory
value is 75 us, the US setting). Everything else in the table is byte for byte
the factory's.

## 3. Registers used

Full map in `datasheets/BK1080_Datasheet_V2.7.pdf` §8 (registers 0x00-0x21;
0x10-0x1F are Beken-internal, write-only trim values that only the vendor table
can produce).

| Register | Bits used here |
|---|---|
| REG_01 | chip ID, 0x1080 |
| REG_02 | 15 DSMUTE, 14 MUTE, 13 MONO, 10 SKMODE, 9 SEEKUP, 8 SEEK, 6 DISABLE, 0 ENABLE |
| REG_03 | 15 TUNE, 9:0 CHAN |
| REG_04 | 14 STCIEN, 13 DEBPS, 11 DE (0 = 75 us, 1 = 50 us), 10 AGCD, 7:6 BLNDADJ |
| REG_05 | 15:8 SEEKTH, 7:6 BAND, 5:4 SPACE, 3:0 VOLUME |
| REG_06 | 15:14 SMUTER, 13:12 SMUTEA, 7:4 SKSNR, 3:0 SKCNT |
| REG_07 | 15:4 FREQD (148 Hz per count), 3:0 SNR |
| REG_0A | 14 STC, 13 SF/BL, 12 AFCRL, 9 STEN, 8 ST, 7:0 RSSI (dBuV) |
| REG_0B | 13:10 IMPC, 9:0 READCHAN |

Sequences in the driver:

- **Power up** (`BK1080_Init`): power pin low, poll REG_01 for 0x1080, one
  0x44-byte burst from REG_00 with the table, 250 ms, REG_25 = 0x283C, REG_25 =
  0x28BC, 60 ms, then 0 dBFS volume, 50 us de-emphasis, stereo (not forced
  mono), seek threshold 8 dBuV, SKSNR 2 / SKCNT 14.
- **Tune** (`BK1080_SetFrequency`): band from the frequency (below 76 MHz OIRT,
  everything else 76-108), channel = (freq - band bottom) / spacing with a
  10-bit check, read-modify-write of REG_05 to keep SEEKTH and VOLUME, clear
  SEEK (clears STC), write CHAN, write CHAN|TUNE, poll STC (5 ms steps, 200 ms
  guard), clear TUNE.
- **Power down**: REG_02 = ENABLE|DISABLE|SEEKUP, then the power pin high.
- **Status** (`BK1080_ReadStatus`): REG_07 (2 bytes) and REG_0A..REG_0B
  (4 bytes), two frames.

Mono/stereo: stereo is the default; `MONO` (REG_02 bit 13) forces mono, and the
chip blends to mono by itself as the signal degrades (BLNDADJ sets the range,
the factory value -6 dB is kept). No software is involved.

## 4. The mode

`OPMODE_WFM_BC = 11`, receive only:

- `opmode_canTransmit()` returns false and `radio_enableTx()` refuses before
  touching the PA: the PA is never keyed in this mode, and the band is receive
  only by law anyway.
- The zone `BC85.0 .. BC108.0` is 231 channels on the 100 kHz raster, named by
  frequency (`zones_channelName()` prints "101.1" for it), flagged
  `ZONE_FLAG_WFM | ZONE_FLAG_RX_ONLY | ZONE_FLAG_SIMPLEX`. A channel stored
  from it carries `CHSTORE_FLAG_RX_ONLY`.
- Raster: 100 kHz by default (CCIR, the factory setting). `c62_wfmSetSpacing(true)`
  switches the chip to SPACE = 10 (50 kHz) for the countries that offset
  stations by 50 kHz (Italy, among others); `wfm_freqValid()` accepts both
  rasters and `wfm_snap()` rounds a keyboard entry to the one in use.

### Why the band ends at 108.0 and not at 110.0

The request was 85-110 MHz. The BK1080's VCO and band tables cover
76.0-108.0 MHz (BAND = 01) or 87.5-108.0 MHz (BAND = 00); 108.0-110.0 MHz is
outside the chip and cannot be tuned at any setting. Rather than clamp
silently, `WFM_HIGH_HZ` is 108 MHz, `wfm_freqValid()` is false above it and the
zone's last channel is 108.0. The low edge of 85.0 MHz *is* reachable, because
BAND = 01 starts at 76.0 — this is the reason the stock band choice matters and
the reason the plan is not built on BAND = 00.

## 5. Stereo, and no RDS

**Stereo: yes.** The BK1080 demodulates the MPX, recovers the 19 kHz pilot and
decodes L/R itself; REG_0A gives `ST` (stereo received) and `STEN` (decoder
locked). Both are read at 10 Hz and published in `state.wfm.stereo` /
`state.wfm.stereoSync`. There is nothing to write in software, and the pilot
detection is the chip's own.

**RDS: no, on this hardware, and not by a firmware decision.** The BK1080 has
no RDS demodulator: its register map ends at 0x21, 0x0A/0x0B are the last
status registers, and there is no group, block or PI/PS/RT register anywhere in
the datasheet (`extract/bk1080_ds.txt`, §8). The stock image confirms it: no
RDS code, no RDS strings, and the stock screens show a frequency and a stereo
flag only. Nor can RDS be decoded outside the chip: what leaves the BK1080 is
**decoded stereo audio** on LOUT/ROUT, not the 57 kHz-modulated MPX that an
external decoder would need, and the CM33 has no audio input from it at all
(the codec's ADC only ever sees the BK4819 discriminator). So no PI, PS,
RadioText or PTY — `wfm_status_t.rds` exists as a field that is always false,
so a later chip could be wired in without changing the mode interface, and the
CPU cost of RDS decoding is zero because there is nothing to decode.

## 6. Cost

### QEMU, Cortex-M33 (mps2-an505), TCG instruction counter

`tests/platform/c62_wfm_cost.c` links `core/wfm.c`, `BK1080.c` and
`i2c_bitbang.c` against a stubbed GPIO layer (the bus acknowledges every byte)
and `delayUs()`/`delayMs()` that count the microseconds instead of waiting.
Instructions counted by `work/dsp-probe/common/libinsnframe.so`, 1000
iterations per block.

| Operation | Instructions | Busy-wait time at 250 kHz |
|---|---|---|
| one 16-bit register write (one frame, 2 data bytes) | 2,528 | 240 us (120 half-delays) |
| one status read (REG_07 + REG_0A/0B, 2 frames) | 6,894 | 688 us (344 half-delays) |
| per-poll channel arithmetic (snap, chan, name, dBuV->dBm) | 814 | — |
| **one status poll (what the mode does 10 x per second)** | **7,708** | **688 us** |
| one channel conversion (scan arithmetic) | 27 | — |

At the mode's 10 Hz polling rate that is 77 k instructions/s — 0.03 % of a
300 MHz M33 — and **6.9 ms of busy wait per second (0.7 %)**, because
`delayUs()` is a busy wait on this SoC. At the old 10 us half-period it would
be 3.4 ms per poll (3.4 % of the core), which is why `clkPeriod` is 2 us: the
chip allows 400 ns, so there is room to halve it again if the CPU ever matters.

A retune is about eight register accesses (band read-modify-write, two REG_03
writes, the mute around it, the TUNE clear) ≈ 20 k instructions and ≈ 2 ms of
busy wait, plus the STC wait (about 60 ms, spent in `k_msleep`). Entering the
mode powers the chip and writes the configuration burst — 69 bytes in one
frame, so an upper bound of 69 x 2,528 ≈ 175 k instructions if it were done
byte-frame by byte-frame, and 310 ms of sleeps (250 + 60), which are sleeps,
not CPU. Nothing runs per audio sample: there is no audio in the CM33 at all.

### Flash and RAM

Same toolchain, same options, `west build -b c62` defaults, ccache warm:

| Build | FLASH | RAM | ITCM | PSRAMAP |
|---|---|---|---|---|
| `master` (44ece688) | 464,072 B | 174,288 B | 8,468 B | 379,064 B |
| this branch | 467,496 B | 174,384 B | 8,468 B | 379,064 B |
| **delta** | **+3,424 B** | **+96 B** | 0 | 0 |

The 96 bytes of RAM are the mode's state and its cached status; there is no
buffer of any kind, so nothing needs PSRAM (`EXT_RAM_BSS_ATTR` is not used —
there is no sample buffer to place). The figure quoted as the baseline
elsewhere in these notes (518,104 B / 196,688 B) does not come out of this tree
at its default options (that build is richer: DMR RX and APRS), which is why
the table above compares against `master` at the same options; against any
baseline the delta is what this feature costs: 3.4 kB of flash, under a tenth
of a kilobyte of RAM.

## 7. UI hooks (nothing under `openrtx/src/ui/` was touched)

The screens are a separate job. What they need is already there:

- `state.wfm` (`wfm_status_t`, `core/state.h`): `freq`, `chan`, `rssi` (dBuV),
  `snr`, `freqd`, `tuned`, `afcrl`, `stereo`, `stereoSync`, `rds`. Written by
  the mode at 10 Hz under `state_mutex`; also readable as a copy through
  `wfm_status()` (`core/wfm.h`).
- `wfm_formatFreq(freq, buf, cap)` prints "101.1" (or "101.15" on the 50 kHz
  raster), the same call `zones_channelName()` uses for the BC channels.
- `OpMode_WFM::rxSquelchOpen()` is true while a valid station is received
  (STC set, AFC not railed), for a signal indicator.
- `c62_wfmSetSpacing(bool fine)` selects the 50 kHz raster;
  `c62_getRxOffset()`/`c62_setRxOffset()` still apply, so a fine-tuning
  control works as in the other modes.
- Seek: `BK1080_SeekStart()` / `BK1080_SeekPoll()` / `BK1080_SeekStop()` sweep
  with the chip's own qualifiers (SEEKTH, SKSNR, SKCNT) and report READCHAN.
  Not wired to a key yet.
- The mode appears through the existing zone/channel machinery: pick a channel
  of the `FM 85-108 MHz` zone. `chstore_modeName()` returns "WFM".

## 8. What only the radio can confirm

Everything above about the chip's registers and the factory sequence is read
out of the datasheet and the factory image. These points need the hardware:

1. **That the tuner answers at 250 kHz on this board** (`clkPeriod = 2`). The
   chip allows 2.5 MHz and the stock firmware bit-bangs with minimal delays, so
   it should, but the first power-on must show `BK1080_Init()` returning true.
   If it does not, the log prints `BK1080: no answer on the bus` and the
   half-period is the first thing to raise (back to 10 us).
2. **The 1000 ms settle replaced by an ID poll.** The factory waits a flat
   second; this driver polls and gives up after the same second. If the first
   tune after a cold start lands on nothing, add the wait back.
3. **De-emphasis.** 50 us is right for Europe; if the high end sounds dull or
   bright on a known station, REG_04 DE is the bit (75 us is the factory
   value).
4. **Level calibration.** RSSI is dBuV from the chip and is converted with
   `P(dBm) = P(dBuV) - 107`; the S-meter offset can only be checked against a
   real signal generator.
5. **The audio summing node.** The schematic and the earlier probe say the
   BK1080 reaches the amplifier through the pot in parallel with the codec leg;
   the mute of the BK4819 AF plus the speaker switch is what keeps them from
   mixing. Worth an ear test with a beep (the codec path) while a station plays.
6. **85.0-87.5 MHz.** The chip tunes it with BAND = 01; there is no station in
   that range in most of Europe to prove it with, so the check is that the
   register write and the channel arithmetic are right, which the unit test
   covers.
