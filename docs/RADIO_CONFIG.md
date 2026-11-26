# The C62 radio configuration: what each mode asks the BK4819 for

Every mode of this firmware has to put the BK4819 into a different shape: channel
mode and IF filters, audio path, de-emphasis and compander, AF gains, the TX
deviation word, the AM detector. That shape used to be the side effect of calling
the driver's setters in an order of its own, spread over `radio_setOpmode()`,
`radio_enableRx()`, `radio_updateConfiguration()`, `radio_enableAfOutput()` and
the spectrum analyser in `platform/drivers/baseband/radio_C62.cpp`. None of it
could be read without the radio in the hand.

It is now a table of register writes:

| | |
|---|---|
| `platform/drivers/baseband/radioPlan.h` | the plan: writes, knobs, options |
| `platform/drivers/baseband/radioPlan.c` | the table, one function per stage |
| `tests/unit/regplan.cpp` | the golden test: every mode, host, no radio |
| `radio_C62.cpp` | walks the plan and does nothing else to these registers |

The rule the refactor had to keep was absolute: **the sequence of writes the
driver produces is the one it produced before**. `tests/unit/regplan.cpp`
enforces it by compiling `BK4819.c` for the host against a recording fake bus,
running the pre-refactor call sequence (`baseline*()`, copied verbatim from
commit `4b8af88d`) and comparing it with the plan, access by access. A read of
`maskSetRegister()` shows up in both, so the comparison covers the
read-modify-write traffic and not only the final values.

## The shape of a plan

```c
struct radioPlanWrite { uint8_t reg; uint16_t mask; uint16_t value; };
```

`value` is what the masked bits are set to: the chip ends up holding
`(current & ~mask) | (value & mask)`. A `mask` of `0xffff` is a whole-register
write (`bk4819_writeReg()`), anything else is a read-modify-write
(`bk4819_maskSetRegister()`) — the two are not interchangeable on the bus, so the
plan keeps them apart and `radioPlan_apply()` picks the matching driver call.

A plan also carries the decisions that are **not** writes:

| knob | meaning |
|---|---|
| `rxReg43` | the word `c62_rx_reg43` mirrors for the baseband dump, 0 = untouched |
| `rxAfGain1` | initial REG_48<11:10>, the airband AGC's first step |
| `flat` | the DSP takes the raw discriminator (`radioPlan_isFlat()`) |
| `amAudio` | install the AM audio AGC on the receive path |
| `amTx` | the PA has an amplitude modulator (C62_AIR_TX) |

and the options it is built with, which are **runtime values and not `#ifdef`s**
so that the host test reaches every variant: `wideRx` (C62_WIDE_RX), `dmr12k5`
(C62_DMR_12K5), `tetraIfDcFilter` (C62_TETRA_IF_DC_FILTER), `spectrumAgcFix`
(CONFIG_C62_SPECTRUM_AGC_FIX), `airTx` (C62_AIR_TX) and the two FM deviation
words from the band calibration.

Five stages, one per call site:

| stage | built by | direction |
|---|---|---|
| `RADIO_PLAN_STAGE_MODE` | `radio_setOpmode()` | BOTH |
| `RADIO_PLAN_STAGE_RX` | `radio_enableRx()` | RX |
| `RADIO_PLAN_STAGE_UPDATE` | `radio_updateConfiguration()` | BOTH |
| `RADIO_PLAN_STAGE_AF` | `radio_enableAfOutput()` | RX |
| `RADIO_PLAN_STAGE_SPECTRUM` | `radio_spectrumSweep(true)` | RX |

`RADIO_PLAN_TX` returns an empty plan for every stage and mode, and that is a
statement about the design, not an omission: **keying writes no part of a mode's
configuration**. The transmitter comes up with the profile the mode change left;
`radio_enableTx()` writes the PLL, the CTCSS word and the signal-path enables and
nothing else. It is pinned by a test, because "fix the DMR deviation when
keying" is exactly the temptation the plan exists to make visible.

### The registers that matter

| register | fields this firmware uses |
|---|---|
| REG_13 | RX AGC gain table, top step: `0x03be` FM, `0x03ff` AM |
| REG_28 | AF RX expander, `<15:14>`: `0x4000` = 1:2 (FM), `0` (AM) |
| REG_2B | AF filters `<10:8>` (de-emphasis) and TX ones; `0x0707` = all off |
| REG_31 | `<3>` compander, `<2>` VOX |
| REG_40 | `<12>` deviation enable, `<11:0>` deviation word |
| REG_43 | `<14:12>` RF filter, `<11:9>` RF filter weak, `<8:6>` AF TX LPF2, `<5:4>` channel mode, `<3>` undocumented, always 1 |
| REG_47 | `<11:8>` AF output (0 mute, 1 FM, 7 AM, 9 bypass), `<1:0>` AF TX filter bypass |
| REG_48 | `<11:10>` AF Rx gain1 (6 dB/step), `<9:4>` AF Rx gain2 |
| REG_4A | undocumented, `0x5450` FM and `0x5448` AM in the stock firmware |
| REG_73 | `<4>` AFC off, `<13:11>` AFC range |
| REG_7D | `<4:0>` microphone gain |
| REG_7E | `<15>` AGC fixed, `<5:3>` TX DC filter, `<2:0>` RX IF DC filter |

REG_43 channel mode is 0 for 12.5 kHz, 1 for 6.25 kHz and 2 for 25 kHz; the RF
filter is 0 for 1.7 kHz, 3 for 3 kHz, 4 for 3.75 kHz and 7 for 4.5 kHz, and in
the 25 kHz channel mode the chip **doubles** whichever one is selected — the
4.5 kHz filter is 9 kHz there. That doubling is behind the defect this table was
built for.

## The modes

Order: the analog modes, then the digital ones from the widest channel to the
narrowest. "Stock" means the value the factory firmware of this radio leaves,
quoted from the notes in `BK4819.c`, `BK4819_priv.h` and `docs/DMR_RX.md`; those
are the only vendor values this repository has and no others were invented here.

### FM (analog voice)

| write | value | why |
|---|---|---|
| REG_2B | `0` on `<10:8>` | de-emphasis **on**: the transmitter pre-emphasises |
| REG_31 | `0` on `<3>` | compander never enabled, here or anywhere else (see below) |
| REG_47 | `0` on `<1:0>` | AF TX filters **on** |
| REG_4B | `0` on `<5>` | ALC **on** |
| REG_7E | `0x002e` | TX DC filter 5, RX DC filter 6: an analog receiver wants DC blocked |
| REG_43 | `0x4048` | 12.5 kHz mode, 3.75 kHz IF, LPF2 6 kHz, at mode change |
| REG_40 | `0x14d2` | the chip's own deviation word; the band calibration replaces it |
| REG_48 | `0x03c0` | AF Rx gain1 0 dB, gain2 `0x3c` |
| REG_7D | `0x19` | microphone gain, the `bk4819_init()` value |
| REG_13/REG_4A | `0x03be` / `0x5450` | FM demodulator profile |

A mode change always leaves **12.5 kHz mode** in REG_43, because it goes through
`bk4819_setAudioMode()` and that is the spacing the chip's voice profile asks
for. The operator's spacing is written by the reconfiguration that follows, in
`RADIO_PLAN_STAGE_UPDATE`:

| channel spacing | REG_43 | channel mode | IF filters |
|---|---|---|---|
| 6.25 kHz | `0x3658` | 1 | 3 kHz / 3 kHz |
| 12.5 kHz | `0x4048` | 0 | 3.75 kHz / 1.7 kHz |
| 25 kHz | `0x3028` | 2 | 3 kHz doubled to 6 kHz |

`0x4048` and `0x3028` are the stock firmware's two FM words; `0x3658` is the
UV-K5 custom firmware's 6.25 kHz one.

### M17

Flat, DC-coupled receive path, 4FSK at 4800 symbols/s.

| write | value | why |
|---|---|---|
| REG_2B | `0x0707` | every AF filter off, de-emphasis off |
| REG_31, REG_4B, REG_70 | `0` | no compander, no ALC, no tone |
| REG_47 | `0x0003` | AF TX filter bypassed (flat TX baseband) |
| REG_7E | `0` | no TX DC filter, **no RX IF DC filter**: the slicer reads levels |
| REG_40 | `0x1582` | `0x582`, the A36Plus deviation word, for ±2.4 kHz |
| REG_43 | `0x7e28` wide / `0x7808` narrow | see below |
| REG_48 | `0x07f0` | AF Rx −6 dB, gain2 `0x3f` |
| REG_7D | `0` | microphone gain 0 |

With **C62_WIDE_RX** (the default) the chip runs the 25 kHz channel mode with the
4.5 kHz filters, `0x7e28` = 9 kHz of IF; without it, 12.5 kHz mode with 4.5 kHz on
strong and 3.75 kHz on weak signals, `0x7808`. Both are reachable from the host
test and both are frozen there.

### DMR

The one that hid a defect. DMR channels are 12.5 kHz.

| write | value | why |
|---|---|---|
| REG_2B … REG_7E | as M17 | the same flat, DC-coupled path |
| REG_40 | `0x1476` | `0x476` = `0x582 × 1944 / 2400`, ETSI's ±1.944 kHz symbols |
| REG_43 | `0x7e28` (default) / `0x7e08` (C62_DMR_12K5) | see below |
| REG_48, REG_7D | `0x07f0`, `0` | AF Rx −6 dB, microphone gain 0 |

`-DC62_DMR_12K5=OFF` (the default) runs the 25 kHz channel mode, where the chip
**doubles** the 4.5 kHz IF filter to 9 kHz: wider than the channel, so a
neighbouring 12.5 kHz channel lands inside the receiver. Stock runs its own 4FSK
in 12.5 kHz mode with 4.5 kHz filters. On the air the symptom is not silence: the
sync and the CACH survive (they are sign-based) and the colour code and timeslot
start jumping while the payload CRC falls to zero.

`-DC62_DMR_12K5=ON` selects 12.5 kHz mode, `0x7e08`: channel mode 0 with the
4.5 kHz filters, which is `0x7e48` before the TX audio filter is set. See "what
the chip holds" below.

### NXDN

4FSK at 2400 symbols/s, 2.9 kHz of occupied bandwidth in a 6.25 kHz channel. The
chip has no 6.25 kHz mode, so 12.5 kHz mode is used and the IF filter follows the
operator's channel spacing; the demodulator's 81-tap matched filter does the
channel selection either way.

| write | value | why |
|---|---|---|
| REG_43 | `0x7e08` (12.5 kHz) / `0x3608` (6.25 kHz) | 12.5 kHz mode, 4.5 or 3 kHz IF |
| rest | as DMR | same flat path, same deviation word (unused: RX only) |

### dPMR

6.25 kHz channels, half of what the chip can filter. 12.5 kHz mode with the
4.5 kHz filters (`0x7e08`), and the mode's own square-root-raised-cosine matched
filter, alpha 0.2, does the selectivity (TS 102 490 clause 12.2.2.2). Receive
only. A wider filter costs adjacent-channel immunity, not sensitivity; the
alternative, 25 kHz mode with 9 kHz of IF, lets a whole neighbour in.

### P25 Phase 1, System Fusion, D-STAR

C4FM at 4800 symbols/s and GMSK at 4800 bit/s: constant-envelope digital
basebands that need the same flat, DC-coupled path as DMR, with no de-emphasis,
no compander and no high-pass, or the slicer sees a shaped constellation instead
of the discriminator. All three take 12.5 kHz mode with the 4.5 kHz filters
(`0x7e08`) and the demodulator's matched filter, like NXDN. None has been
measured on air yet; the values are stock's digital word, not a measurement.

### TETRA

25 kHz channels, receive only.

| write | value | why |
|---|---|---|
| REG_43 | `0x7e28` | 25 kHz mode, 4.5 kHz IF doubled to 9 kHz |
| REG_2B … REG_7E | as DMR | flat path |
| REG_7E `<2:0>` | `0b110` on **receive start** | the one difference from the other flat modes |

The RX IF DC filter (REG_7E<2:0>, chip default `0b110`) is cleared by the audio
bypass and put back by `radio_enableRx()` when `C62_TETRA_IF_DC_FILTER` is on
(the default). With it off, the IF DC offset of this low-IF receiver reaches the
demodulator as a one-sided tone at 16.9 kHz and distorts the +3π/4 symbols much
more than the −3π/4 ones; TETRA decodes phase differences and needs it gone. DMR
slices absolute 4FSK levels and must not inherit it — that is why this write is
TETRA's alone and not the whole flat group's.

The 25 kHz / 9 kHz receive path is also what the measured equaliser taps in
`CONFIG_TETRA_EQ_INIT` were taken on.

### Airband AM

Receive, `CONFIG_C62_AIR_TX` optionally transmitting.

| write | value | why |
|---|---|---|
| REG_43 | `0x0048` (12.5 kHz) / `0x3628` (25 kHz) | whole write: 1.7 kHz IF for the 8.33 kHz raster, 3 kHz doubled to 6 kHz for 25 kHz |
| REG_47 `<11:8>` | `7` | AM detector at the AF output (an undocumented selector value) |
| REG_13 | `0x03ff` | AGC top step 0 dB on every stage, stock's AM value |
| REG_4A | `0x5448` | stock's AM value, one bit away from its FM one |
| REG_2B `<8>` | `1` | de-emphasis **off**: AM carries no pre-emphasis |
| REG_28 | `0` | expander off: it would undo part of the audio AGC |
| REG_48 | `0x07f0` | AF Rx −6 dB, one step down from FM |

`0x0048` is the stock firmware's AM word. The detector has no bandwidth of its
own — the RF/IF filter of REG_43 is what limits it — so the channel spacing is
chosen by picking the filter that fits it.

The AM **transmit** path is not a register: the BK4819 emits a bare carrier and
`core/am_tx.c` drives the PA bias through the APC line with the voice envelope.
The carrier power and the ends of the linear window of that bias,
`RADIO_PLAN_AM_CARRIER_MW` (1000), `RADIO_PLAN_AM_DUTY_MIN_PCT` (10) and
`RADIO_PLAN_AM_DUTY_MAX_PCT` (190), are in `radioPlan.h` with the mode's other
numbers; `docs/TX_AIRBAND.md` has the bench procedure that measures them. None of
the three is measured yet.

### Broadcast FM

The BK4819 is not in the audio path at all: the BK1080 drives the amplifier
through the analog pot. The mode's writes are the **voice** profile (so that
leaving it lands on FM) plus REG_47 AF output muted, and `RADIO_PLAN_STAGE_AF`
produces **no write** for it, so nothing here can unmute the transceiver under a
station.

### The spectrum analyser

Not a mode, but it owns the chip while it is open:

| write | value | why |
|---|---|---|
| REG_43 | `0x7e28` | 25 kHz mode, 9 kHz of resolution: narrower than the smallest analyser step, so two bins never see the same channel |
| REG_47 | mute | the bins are not to be heard |
| REG_73 `<4>` | `1` | AFC off: a carrier near the bin would pull the synthesizer onto itself and move what is being measured |
| REG_7E `<15>` | `1` only with CONFIG_C62_SPECTRUM_AGC_FIX | AGC fixed, so the trace does not depend on the gain the previous sweep left |

Parking restores what was read back at entry (REG_43, REG_73 and, with the fix,
REG_7E), so that path is not a plan: its values are the chip's, not a table's.

## What is not in the plan, and why

`bk4819_enableRx()`, `bk4819_enableTx()` and `bk4819_disableRtx()` (REG_30/REG_37
signal-path enables), the PLL (REG_38/REG_39), the CTCSS/CDCSS words
(REG_07/REG_08/REG_51), the squelch thresholds (REG_4D/REG_4E/REG_4F/REG_78) and
the beep (REG_70/REG_71) are not per-mode: they are the same for every mode or
they are the operator's channel, and they already live in single functions in
`BK4819.c`. The plan covers what a mode decides.

`radio_disableAfOutput()` (REG_47 AF mute) is likewise constant.

One runtime loop keeps a setter of its own: `amAfGainStep()` moves REG_48<11:10>
one 6 dB step at a time, from the peak level the codec reports, while the airband
mode receives. That is a control loop following the signal and not a decision a
mode takes, which is why it is not in a plan; the value it starts from is
(`knobs.rxAfGain1`), and the value of gain2 it keeps beside it is written down
next to the loop so the two cannot drift apart unnoticed.

## What the chip holds, the word the dump reports, and the stock word

Three different things, and they are not the same number:

* **what the chip holds** after a mode's profile: `0x7e28` in the 25 kHz modes,
  `0x7e08` in the 12.5 kHz digital ones (M17 without C62_WIDE_RX: `0x7808`);
* **`c62_rx_reg43`**, which the baseband dump reports and the REG_43 sweep
  writes: `0x7e28`, `0x7e48`, `0x7808` — the words in the comments of
  `radio_C62.cpp` before the plan;
* **the stock firmware's** digital word, `0x7e48`.

They differ in REG_43<8:6>, the AF TX LPF2 field: the digital modes set the IF
filters and then ask for the 3 kHz TX audio filter, which clears that field. So
"REG_43 = 0x7e48" in `docs/DMR_RX.md` and in `reports/dmr_rx_hw.md` describes the
chip **before** the last of the three writes, and the word a DMR capture reports
in its header is `0x7e48` while the chip holds `0x7e08`. In receive the field is
irrelevant (it is a transmit audio filter, and the TX path is bypassed anyway),
which is why nothing has ever noticed; the test freezes both numbers so that a
future change has to decide which one it means.

dPMR carries a second slip: it reports M17's narrow word `0x7808` while the chip
holds the same `0x7e08` as NXDN. Also frozen, also metadata only.

One more finding, frozen rather than fixed: **REG_31<3>, the compander, is off in
every mode**. `bk4819_setAudioMode()` clears it in both of its profiles and
nothing in this firmware sets it back, so what an FM operator hears is the 1:2
expander of the voice profile and not a compander.

And one that was a defect in waiting and is now closed: **only M17 and DMR set
the TX deviation word**.  NXDN, dPMR, P25, YSF and D-STAR entered the flat audio
path without writing `REG_40`, so a transmission in one of those modes
(`CONFIG_NXDN_TX`, `CONFIG_DPMR_TX`, `CONFIG_P25_TX`, `CONFIG_YSF_TX` exist) went
out with whatever the *previous* mode left in the register: M17's `0x582`, FM's
band-calibrated word, or the chip's default after a reset -- a transmission whose
deviation depended on what the operator had done before.

Every transmitting mode now writes it: the 4FSK and GMSK modes take the wide
digital word M17 uses (`0x582`), DMR keeps its own (`0x476`, a M17-scaled
estimate until the bench calibrates it), and FM keeps the band-calibrated pair.
`tests/unit/regplan.cpp` asserts that every digital transmitter writes the
enabled word and that airband never touches `REG_40`.  What is *not* proven is
the number itself for NXDN, dPMR, P25, YSF and D-STAR: none of them has been
measured on the air, and a bench run is what would tune each one.

## Checking it, and changing it

```sh
meson setup build_linux -Dcpp_args=-fpermissive
ninja -C build_linux regplan_test
./build_linux/regplan_test              # or: meson test -C build_linux --no-rebuild
```

The test compiles `BK4819.c` for the host, runs it against a recording fake bus
and asserts, for every mode, every channel spacing and 16 build-option variants:

* the plan is the sequence the pre-refactor driver issued — access by access,
  including the reads of a read-modify-write;
* the channel mode and filter fields of REG_43, and the whole REG_43 word;
* the RX IF DC filter and TX DC filter fields of REG_7E;
* the audio path: de-emphasis, compander, ALC, AF TX filter bypass;
* the AF output selector and the RX and TX gains;
* the TX deviation word, per mode, and FM's calibrated one per spacing;
* the airband carrier and duty constants, and the AM filter words;
* that keying writes nothing.

A failure prints the whole plan of the failing mode (`radioPlan_format()`), so a
diff between what the table says and what the chip is asked for reads directly.

Changing a mode's configuration means changing `radioPlan.c` and the matching
`baseline*()` in `tests/unit/regplan.cpp` in the same commit: the baseline is the
frozen pre-refactor sequence and it is what makes the change visible. Changing a
**setter** in `BK4819.c` does not need the baseline touched — the test will fail
until the plan agrees with the new setter, which is the point.

## What cannot be checked off the radio

Everything here is "what the chip is asked for", which is now provable on the
host. None of it is a statement that the mode works:

* whether the 12.5 kHz DMR setting (C62_DMR_12K5) decodes better than the 25 kHz
  one on this radio's IF, and by how much;
* whether the deviation words produce the deviation they are meant to
  (`0x476` for ±1.944 kHz, `0x582` for ±2.4 kHz): the varactor's sensitivity per
  band is not measured and the words are arithmetic, not calibration;
* whether P25, YSF and D-STAR work at all on this hardware: their values are
  stock's digital word, not a measurement;
* the airband carrier and duty window;
* whether the compander is worth turning on for FM.

`docs/TEST_PLAN.md` and the `docs/RX_<MODE>.md` / `docs/TX_<MODE>.md` files carry
the bench procedures for those.
