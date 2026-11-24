# DMR transmit on a repeater

The transmitter of `docs/DMR_TX.md` works out of a stationary channel: it keys
the PA, starts its own 60 ms frame and puts the call in the timeslot the
operator picked. On a repeater that is not enough, and it fails in three ways
that only appear on the air:

1. **The repeater is not listening.** A base station that has nothing to
   transmit is in the hibernating state of TS 102 361-2 clause 5.1.1.1 and
   ignores inbound traffic until somebody sends it a BS_Dwn_Act ("BS outbound
   activation") control block. A direct-mode transmitter never sends one.
2. **The frame is not ours.** On a two frequency channel the base station *is*
   the timing master (TS 102 361-1 clause 4.4.1 and 5.2.1.3): it transmits
   continuously and every mobile derives its inbound timing from that downlink.
   A burst that starts when our own keying happens to start lands anywhere
   inside the repeater's timeslot, or outside it.
3. **The colour code may not be ours.** A repeater that speaks another colour
   code is another repeater (another site, another network) and transmitting at
   it is interference. TS 102 361-1 clause 5.2.1.6 calls the behaviour that
   avoids it "polite to own colour code".

This document describes what the firmware does about each of them, what the
time budget is, what half duplex allows and what it does not, how the numbers
are measured, and how to test against a repeater.

Everything lives in two new files plus the glue:

```
openrtx/include/protocols/DMR/DmrRpt.h    the whole interface, documented
openrtx/src/protocols/DMR/DmrRpt.c        the implementation (pure C, no OS)
openrtx/include/core/audio_latency.h      the audio path measurement
openrtx/src/core/audio_latency.c          chirp + correlation, fixed point
openrtx/src/rtx/OpMode_DMR.cpp            the glue: the radio side
tests/unit/dmr_rpt.cpp                    27 host tests, 1 345 assertions
work/rpt-cost/ (outside the tree)         instructions per call on a Cortex-M33
```

## Status

| piece | state |
|---|---|
| BS_Dwn_Act CSBK (TS 102 361-2 table 7.5a) with the annex B CRC mask | done, host test + CRC residue |
| wake machine: send, listen, retries, policy when nobody answers | done, host test |
| downlink frame phase (TS1 of the 60 ms frame) + drift in ppm | done, host test with jitter, drift, losses, outliers |
| start plan: slot parity + sub-slot delay, drift slew during the call | done, host test at 0/5/10/20 ms of latency |
| colour code / timeslot / busy gate | done, host test |
| audio path measurement (chirp, correlation, uncertainty) | done, host test; **needs a bench run: the DSP in this radio is the LSF image, not the stock one** |
| parrot calibration pass (five transmissions, median of the valid ones) | done, host test with a simulated repeater |
| real air test against a repeater or a parrot | **pending: needs the bench** |
| deviation calibration, adjacent channel power | still pending, see `docs/DMR_TX.md` |

## 1. Waking the repeater

On the inbound channel the access burst is one CSBK with the MS sourced sync
(TS 102 361-2 clause 5.1.1.1). The block is fixed by table 7.5a:

```
octet 0   LB = 1, PF = 0, CSBKO = 111000 (0x38)          -> 0xB8
octet 1   FID = 0
octets 2-3  reserved, zero
octets 4-6  BS address, 0xFFFFFF = any base station
octets 7-9  source address, our radio ID
octets 10-11 CRC-16 of the 96 bits, XORed with the CSBK mask 0xA5A5
```

`dmrRpt_csbkDownlinkActivity()` builds it and the host test checks both halves
of the CRC rule: the block as transmitted leaves the residue 0xA5A5 that
`DmrPhy.c` expects from a real repeater, and with the mask undone it leaves
zero. The burst goes out through the same chain as any data burst
(`dmrTx_dataSlot()` with `DMR_DT_CSBK`), in the timeslot of the channel.

The machine (`dmrRptWake_*`, in `DmrRpt.h`) is a pure function of time:

```
IDLE ──PTT on a repeater channel──▶ TX ──burst on the air──▶ LISTEN
                                    ▲                          │
                                    │   no outbound in 300 ms  │
                                    └──────── retry ───────────┤ (2 retries)
                                                               ▼
                                     answered ──▶ CALL     no answer ──▶ FREE/RUN
                                                                  └──▶ ABORT
```

* **A repeater that is already up is not woken.** If the receiver is locked to
  an outbound burst with our colour code, the machine is skipped: the standard
  only asks for the activation when the base station is not transmitting.
* **The answer** is any outbound burst of the base station with a valid slot
  type — the repeater comes up by transmitting Idle PDUs in both slots with
  the CACH access type set to Idle (TS 102 361-2 clause 5.1.1.1.3). It does not
  have to be a CSBK: what proves the channel is up is that the repeater is
  transmitting, not what it says.
* **300 ms of wait per attempt, two retries.** A repeater that is up transmits
  within a few frames; the whole access costs at most about one second, five
  transmissions of a single burst each.
* **No answer is a decision, not a silence.** `settings.dmr_noanswer` is
  *free running* (call anyway, with the timing counted from our own keying,
  which is what the transmitter did before this work) or *abort* (stay silent
  and say so). The default is free running.
* Every exit — answered, refused, cancelled by releasing the PTT, abort from
  the transmit supervisor — leaves the radio receiving. The wake burst is
  bounded by its own four slots and never touches the transmit guard's
  absolute cap.

## 2. The downlink frame phase, in samples and in ppm

The receiver already tracks the 30 ms grid to demodulate, and the CACH of every
outbound burst says which of the two slots that burst belongs to. What is new is
turning that into a position in *our* sample clock:

* `struct dmrBurst` now carries `slotSample`: the 48 kHz sample index (in the
  receive stream's own numbering) of the start of the timeslot the burst
  belongs to. The sync sits 66 symbols after that boundary in the base station
  layout (12 of CACH plus 54 of the first half) and 60 in the handheld one, and
  the fractional timing of the burst refines it to a sample.
* The start of TS1 of the repeater's 60 ms frame is therefore
  `slotSample - (timeslot2 ? 1440 : 0)`, one measurement per decoded outbound
  burst with a valid CACH. Coasted bursts (decoded on the grid, no sync word)
  are not fed back: they were predicted from the grid and would only confirm
  it.

The tracker (`dmrRpt_phaseFeed()`) keeps a straight line through the anchor:

```
p(t) = p0 + 48 (t - t0) + intercept + slope (t - t0)        [48 kHz samples]
```

with two windows, because they answer two different questions:

| window | what | why |
|---|---|---|
| the last 8 post-wake bursts (~240 ms) | median → clip outliers at 5 ms → mean | where the grid is *now*: the correction the transmitter applies |
| a baseline of 2 s to 8 s between the residual now and a reference | least squares slope | how fast the grid runs: 1 sample of 48 kHz over 2 s is 10 ppm, so a shorter baseline cannot say anything about a crystal |

* **Outliers**: a measurement more than 5 ms (240 samples) from the median is
  rejected. Three in a row mean the grid moved rather than a burst being
  mis-decoded, so the tracker restarts from that burst and reports it.
* **Limit**: a fitted drift past 25 ppm is a broken reference (a free running
  codec, a capture played back), and the estimate is then *not used* to align:
  `dmrRpt_phaseSettled()` says no and the call falls back to free timing.
* **Settled** needs five accepted measurements, a spread of at most 0.5 ms and
  a drift inside the limit. What the drift is *not* resolved to is stated
  plainly: with a 10 s baseline the fit is good to a couple of ppm, and 2 ppm
  over a minute of transmission is 0.12 ms — two orders of magnitude inside the
  budget below.
* **Log**: the fitted ppm, the filtered offset, the acceptance ratio and the
  delay applied are published in `struct dmrStatus` and shown by the quick menu
  (`RPT` and `DRIFT` rows) and by the remote control command `rpt`.

## 3. The colour code and timeslot gate

`dmrRpt_gate()` runs before anything is keyed, on what the receiver last heard:

| condition | verdict | what the operator sees |
|---|---|---|
| colour code of the last burst ≠ colour code of the channel | `CC` | the `RPT` row shows `NO CC`, nothing is transmitted |
| the channel fixes a timeslot and the repeater is on the other one | `TS` | same |
| the CACH access type of the last burst *in our slot* says busy | `BUSY` | same, and the call waits |
| nothing heard yet, or everything matches | `OK` | the call goes ahead |

The timeslot check is only meaningful when the channel fixes one: a channel can
be set to *follow the repeater* (`settings.dmr_ts_follow`), in which case the
transmitter uses the slot the repeater was last seen on — which is what a
repeater in 1:1 mode with its traffic on TS2 needs.

The gate runs twice: when the PTT is pressed and again when the repeater
answers, because the answer may come from another colour code than the burst
that started the sequence.

## 4. The start plan, and where the latency comes from

`dmrRpt_plan()` turns "TS1 of the repeater's frame is at sample *p*" into two
numbers the transmitter can apply:

* **`burstFirst`** — whether the first or the second slot the transmitter
  renders carries the voice LC header. It is the slot parity, in whole 30 ms
  steps.
* **`shift`** — a delay of 0 to 1439 samples (0 to 30 ms) applied to the
  baseband waveform. The modulator renders whole slots, so the delay is built
  from the previous rendered slot and the current one
  (`dmrRpt_txShift()`): the two halves of `txSamples` alternate as the
  rendering target and the stream gets the tail of the one before.

The arithmetic, with the sign spelled out:

```
off      = mod2880(ts1 - latency + (ts == 2 ? 1440 : 0))    latency = see below
shift    = off mod 1440            the waveform leaves `shift` samples later
burstFirst = off >= 1440 ? 2 : 1   ... and the burst is in the second slot
```

so the burst starts on the air at `shift + latency + (burstFirst-1)*1440`
samples after the keying instant, which is exactly the repeater's slot boundary
by construction. A latency that is wrong by *e* moves every burst by *e* and
nothing else, which is why the number has to be measured and not guessed.

### The delay is two delays

| part | value | how it is known |
|---|---|---|
| software: the double buffered output stream hands the writer one slot while the other one plays | **1440 samples, 30 ms** (`DMR_RPT_STREAM_LATENCY`) | exact, it is the firmware's own buffering |
| software: the lead slots the plan asks for while the PA settles | **1 slot, rounded up to 2 (60 ms)** (`DMR_RPT_LEAD_SLOTS`) | exact, it is the plan's own knob |
| hardware: codec FIFO, DSP hop, resampler, the time from the last sample written to the modulation | **measured**, `settings.audio_latency_us` | a measurement, and it *changes when the DSP image changes* |

The radio this was written for runs the **LSF image of the SDK at 48 kHz**
because the stock `rpcsrv` image runs at 16 kHz and cannot carry the DMR
baseband, so the stock latency figure does not apply and neither does any figure
from another image. The setting is a single number in microseconds with an
uncertainty beside it, and an uncalibrated radio starts at a placeholder of
5 000 µs ± 2 000 µs which is *deliberately* too uncertain to align with: the
firmware says `LAT?` in the `RPT` row and calls with free timing instead of
pretending. See "Calibration" below for how the number is produced.

### The time budget

Everything below is in samples of 48 kHz, and the margin is the one
TS 102 361-1 clause 10.2.3.1.3 gives before slots interfere: **1 ms, 48
samples**, of which 0.9 ms is usually spent on propagation at 135 km.

| item | samples | ms | note |
|---|---|---|---|
| slot | 1440 | 30 | one timeslot |
| frame | 2880 | 60 | two slots |
| one symbol | 10 | 0.208 | the granularity of the phase measurement |
| allowed deviation inside the slot | 48 | 1.0 | the budget |
| phase measurement, per burst | ±2 | ±0.04 | sync correlation jitter |
| phase measurement, filtered (8 bursts) | ±1 | ±0.02 | median + mean |
| extrapolation from the last burst to the keying instant | ±1 per 20 ms | — | 48 samples/ms nominal, error from the ppm fit |
| drift fit, 10 s baseline | ±2 ppm | — | 0.12 ms over a 60 s transmission |
| **hardware latency, measured** | ±2 (spread) | ±0.04 | the calibration's peak width |
| **hardware latency, uncalibrated** | ±96 | ±2.0 | the placeholder: does not fit, and it says so |
| software latency | 0 | 0 | exact |
| **margin left, calibrated** | **≈ 40** | **≈ 0.83** | `dmrRpt_fits()` |
| **margin left, uncalibrated** | **−50** | **−1.04** | refused |

`dmrRpt_fits()` is the check: latency spread plus the drift over the
transmission against the 1 ms budget. When it does not fit, the plan is not
used, the `RPT` row says `LAT?`, and the call either goes out free running or
not at all, following `settings.dmr_noanswer`. During the call the delay also
follows the fitted drift, one sample at a time (`dmrRpt_txShiftStep()`): 25 ppm
is 1.2 samples per second, so the slew never has to catch up.

## 5. What half duplex allows, and what it does not

The BK4819 is one chip: while the radio transmits, it does not receive. That
single fact shapes every compromise here.

**Possible, and implemented**

* Measure the repeater's frame phase while receiving, key the PA and start the
  first burst on the grid of a frame that is up to tens of milliseconds in the
  past, extrapolated with the fitted drift. The error over that hop is well
  under a tenth of a millisecond.
* Wake a sleeping repeater and wait for the outbound channel to come up, because
  the wake is *itself* a transmission and the wait a reception, one after the
  other, not at the same time.
* Keep the burst inside the slot for the length of one transmission, by
  following the fitted drift.
* Refuse to transmit at a repeater that speaks another colour code, and to
  transmit into a slot the CACH marks busy.

**Not possible**

* **Hearing ourselves, or the other slot, while transmitting.** No late entry
  beyond what was measured before the PTT, no watching the other timeslot, no
  detecting a collision while it happens. A repeater that is busy by the time
  we key is only noticed at the next reception.
* **Closing the loop during a call.** The alignment is open loop for the length
  of one transmission: the phase cannot be corrected while the radio is on the
  air, only extrapolated. That is why the drift budget is what it is: with a
  2 ppm fit and a 60 s transmission the burst moves 0.12 ms, and the 1 ms
  budget has room for a much worse crystal than the standard allows.
* **Anything that needs the repeater's own measurements** — its RSSI, its
  timing reports, a Channel Timing CSBK. The radio can only use what the
  downlink says out loud.
* **A true "caller hears the channel clear before keying" politeness.** The busy
  check is the CACH of the last burst heard, which is up to 300 ms old by the
  time the PA is keyed.
* **Simultaneous audio and timing**. The receiver is stopped before the PA is
  keyed (`docs/DMR_TX.md`), so the last phase measurement is from before the
  switch.

## 6. Calibration

Two procedures, one setting. Both write `settings.audio_latency_us` and its
uncertainty, and both are one-shot: nothing keeps running afterwards.

### 6.1 The audio path, with a cable (laboratory, and the initial value)

What it measures: codec + DSP + resampler, i.e. the hardware part of the table
above. What it does not measure: the transmitter's own group delay between the
DSP output and the modulator, which only a second receiver can see.

Procedure:

1. Connect a cable from the loudspeaker/earphone output to the microphone input
   (attenuated: the marker is at a quarter of full scale and the microphone
   path has gain). A quiet room with the radio on a table works too, with a few
   milliseconds of acoustic delay and a worse uncertainty.
2. Run the measurement: the remote control command `lcal`, or the menu entry
   the UI hook sets `rtxStatus.audioCalRequest` for.
3. The firmware plays a 10 ms chirp (400 Hz to 4 kHz, Hann windowed) into the
   loudspeaker and captures 100 ms of the microphone at 48 kHz, correlates, and
   stores the delay of the correlation peak and its half width as the
   uncertainty. `rpt` shows both.
4. Repeat it a few times and keep the reading that repeats; the number is only
   as good as the loop, and the cable loop is the good one.

A wrong reading is not dangerous: the next procedure corrects it, and an
uncertain one makes the firmware refuse to align rather than transmit badly.

### 6.2 The parrot pass (in the field, and the value that matters)

With a repeater that repeats — a parrot, an echo/repeater test facility, or your
own repeater with a talkgroup that comes back — the alignment can be measured on
the air, which is the only place where the *whole* path is visible, DSP image and
modulator included.

What it does, in the order it does it:

1. **One pass of five short transmissions**, each one the same synthetic call —
   a voice LC header and two voice bursts of digital silence, never the
   microphone, so two attempts are identical on the air — at a different slot
   offset: 0, −0.5, +0.5, −1.0 and +1.0 ms.
2. **After each one, 400 ms of listening.** The echo is counted (bursts that
   decode, and data bursts whose CRC passes) and its arrival is compared with
   the repeater's own grid.
3. **Discard and take the median.** Attempts that do not come back, or that come
   back with less than half the quality of the best one, are discarded; the
   median of the offsets of the rest is the correction. With fewer than two
   valid attempts nothing is stored: the previous value stays and the radio says
   `CAL NO`.
4. **The result is folded into the same setting**: the winning offset is a delay,
   so it is added to `audio_latency_us`, and the dispersion of the winners
   becomes its uncertainty. One persistent number, in microseconds.
5. **A summary** at the end: valid attempts, median and spread, in the `CAL`
   states of the `RPT` row and as a table over the remote control (`rpt`).

Why the offset varies between the attempts: a parrot re-times its outbound to
its own frame, so the *arrival time* of the echo cannot measure our alignment —
only its quality can. What the echo proves is that our burst fell inside the
window of the far end, and a small search over offsets with quality as the
criterion is what turns that proof into a number. On a repeater that relays with
a fixed turnaround the echo delay is meaningful too, and the trace shows it: it
comes out as the alignment error plus the turnaround, and a parrot shows the
slot structure of the box instead.

Safety, because this is a transmitter that keys itself:

* It starts only from an explicit request (`rtxStatus.rptCalRequest`, set by the
  menu entry) on a channel marked as a repeater.
* Five attempts, a hard wall clock limit of 12 s of channel time, and every
  stage has its own deadline: a repeater that answers nothing, or a caller that
  never reports the burst on the air, cannot hang the pass.
* Pressing the PTT or disabling TX cancels it: the machine stops transmitting
  immediately and the result is not stored.
* It never runs on top of a call and never leaves the radio keyed: every
  attempt ends with the receiver back on.

**When to repeat it**: after every change of DSP image (the latency figure is a
property of that image), after changing the radio hardware (a different codec
crystal, a different PA), and whenever the `DRIFT` row shows a call that used to
work failing. Not after a change of repeater: the alignment is a property of
*our* transmitter, not of the box.

**Which repeaters work**: anything that repeats the digital signal — a parrot
function, an echo test, your own repeater with a talkgroup you can hear. **Which
do not**: a repeater that only repeats voice through its vocoder with a different
timing (still usable: the quality criterion works, the delay figure does not), a
network that answers minutes later (the pass gives up and stores nothing), a
repeater with a different colour code (the gate refuses before anything is
transmitted, which is the point).

## 7. What is tested, and what is left to the bench

`tests/unit/dmr_rpt.cpp`, 27 test cases, 1 345 assertions, no hardware:

* **The CSBK** — the exact octets, the parse back, and both halves of the CRC
  rule (residue 0xA5A5 as `DmrPhy.c` checks, and zero with the mask undone).
* **The tracker** — a quiet grid settles and predicts the truth modulo the
  frame; five drifts from −7 to +20 ppm are found within 3 ppm and extrapolate a
  second ahead to within 6 samples; a quarter of the bursts lost, ±3 samples of
  jitter and one in four landing a full slot away still give a settled grid with
  the drift and the offset right; three outliers in a row restart the tracker;
  a grid that never converges (200 samples of jitter, or 60 ppm) is reported as
  unsettled and not used.
* **The plan** — for seven phases, four latencies (0, 5, 10, 20 ms and up to a
  full frame), both timeslots and four lead-slots settings, the burst lands
  exactly on the target slot boundary, with a delay that always fits in one
  slot; a latency that is 0.5 ms wrong moves the burst by exactly 0.5 ms; a
  5 ms uncertainty is refused by `dmrRpt_fits()` and a 25 ppm drift over five
  minutes is refused too.
* **The delay line** — a delay of 0 is a copy, a delay of n takes the last n
  samples of the previous slot and then the first 1440−n of this one; the slew
  moves one sample at a time.
* **The wake machine** — retries twice and then decides per policy (all five
  attempts counted); an answer goes straight to the call; a repeater that is
  already up is skipped; a caller that never reports the burst cannot hang it.
* **The gate** — OK, colour code, timeslot, busy, and the "nothing heard yet"
  case that must *not* refuse.
* **The audio measurement** — the marker is generated with the right envelope; a
  marker delayed by 0, 1, 12, 120, 480 and 1 000 samples in noise is found to
  within one sample with a plausible uncertainty; silence is not a measurement.
* **The parrot pass** — a simulated repeater that only answers inside a window:
  the pass finds the window and the median of the valid offsets; a centred radio
  keeps its calibration; a noisy echo still finds the middle; no echo (or a
  single echo) stores nothing; a cancel transmits nothing more; the pass is
  bounded in wall clock; the monitor line fits.

**Left to the bench**, in order:

1. **Measure the audio path** on the radio with the LSF image in place (`lcal`
   with a cable loop) and write the figure into the defaults if it turns out to
   be a constant of the hardware. This is the first thing to do: without it the
   firmware refuses to align.
2. **Run the parrot pass** against a repeater that repeats and compare its
   median with the laboratory figure. They should agree to a few hundred
   microseconds; if they do not, the parrot pass is measuring the repeater's
   turnaround as well as our path, and the difference belongs in the
   documentation of that repeater, not in the setting.
3. **On the air**, with a second radio listening: the burst has to land inside
   the slot, and the `DRIFT` row has to stay small over a minute of
   transmission.
4. Adjacent channel power and deviation, still the open items of
   `docs/DMR_TX.md`, apply here unchanged: a burst that is on time and
   over-deviating is still a bad neighbour.

## 8. Cost

Instructions per call, measured on QEMU `mps2-an505` (Cortex-M33, DSP extension,
`-O2`, the firmware's own flags) with `work/rpt-cost/build.sh` (in the `work/` workspace, next to `work/dmr-tx/`),
which counts
every executed instruction of two builds of the same block (1 and 1 000 calls)
and subtracts:

| what | when | instructions |
|---|---|---|
| `dmrRpt_phaseFeed()` | once per outbound burst | 944 |
| `dmrRpt_plan()` | once per call | 115 |
| `dmrRpt_txShift()` | once per 30 ms slot, whole call | 1 707 |
| `dmrRpt_txShiftStep()` | once per slot (drift slew) | 21 |
| `dmrRpt_csbkDownlinkActivity()` | once per activation attempt | 1 794 |
| `dmrRpt_gate()` | twice per PTT | 35 |
| `dmrRptCal_next()` | once per calibration step | 114 |
| `dmrRptCal_result()` | once per calibration pass | 107 |
| `audioLatency_measure()` | once per measurement request | 12 762 122 |

So a voice slot costs **1 728 extra instructions** on top of the 117 669 a voice
burst already costs (`docs/DMR_TX.md`): 1.5 % more, i.e. **1.6 – 2.1 % of a
300 MHz core** for the transmitter as a whole. A receive burst costs 944 more
while a repeater is being tracked. The audio measurement is 12.8 M instructions —
42 ms of the core — which is why it is a menu request and not something that
runs by itself.

The phase tracker is the expensive per-burst piece, and the reason is the
64-bit division of the drift fit: the Cortex-M33 has no 64-bit divide and the
`__aeabi_ldivmod` calls dominate the 944. It runs once per burst per slot, so it
is 0.03 % of the core; if it ever matters, the fix is to keep the fit in 32-bit
Q16 arithmetic, which changes the resolution not at all (see the comment in
`evaluate()`).

## 9. Memory

Zephyr build of this branch, `-b c62` with `-DCONFIG_C62_DMR_RX=y
-DCONFIG_C62_APRS=y`, from the linker report:

| build | FLASH | RAM |
|---|---|---|
| receive only, `CONFIG_DMR_TX` off | 518 580 B (49.5 %) | 196 736 B (75.1 %) |
| receive + transmit, `CONFIG_DMR_TX=y`, **before this work** (`master`) | 579 120 B (55.2 %) | 209 640 B (80.0 %) |
| receive + transmit, `CONFIG_DMR_TX=y`, **this work** | **585 208 B (55.8 %)** | **209 896 B (80.1 %)** |
| reference of the task, receive only ("518 104 B / 196 688 B") | 518 104 B | 196 688 B |

So the repeater work costs **+6 088 B of flash and +256 B of RAM** on top of the
transmitter that already worked.  Of that, 3 399 B are the two new modules
(`DmrRpt.c` 2 713 B, `audio_latency.c` 686 B), the rest is the glue in
`OpMode_DMR.cpp` and the four new state blocks of `dmrStatus_t`.

Nothing allocates a buffer of its own:

* the tracker, the plan, the wake machine and the calibration pass live in the
  mode instance: 256 B of `.bss` in total, of which 100 B are the eight-entry
  measurement window;
* the delay line of the alignment **borrows the two halves of the existing
  30 ms slot buffer** (`txSamples`) — the two halves alternate as the rendering
  target and the stream reads the tail of the one before, so a 0–30 ms delay
  costs nothing;
* the audio measurement borrows the same slot buffer and the receive baseband
  buffer: 480 samples of marker out, 480 back, 2 400 of capture, and the
  receiver and the PA are both idle while it runs.  A first version had them as
  `static` arrays and cost 21 KiB of RAM for a measurement that runs when the
  operator asks; that is what the "+256 B" number is the result of fixing.

The rest of the difference against the receive-only image is the transmit chain
and the AMBE+2 encoder, unchanged from `docs/DMR_TX.md`.

The 476 B of flash and 48 B of RAM that the receive-only image grew by are the
always-present parts: the six new `rtxStatus_t` fields, the settings record
fields and its version bump, the repeater fields of `dmrStatus_t`, the slot
sample and CSBK opcode fields of `struct dmrBurst`, `dmr_fec_crc16_append()` and
the two new quick menu rows.

## 10. Using it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_DMR_RX=y -DCONFIG_DMR_TX=y
```

Then, on the radio:

1. Mark the channel as a repeater (`settings.dmr_repeater`, per channel through
   `CHSTORE_FLAG_REPEATER`). A channel edited in the radio currently takes the
   bit from the settings; the editor screen for it is part of the UI work.
2. Set the colour code of the channel and whether the timeslot follows the
   repeater (`settings.dmr_ts_follow`) or is fixed (`settings.dmr_timeslot`).
3. Choose what happens when the repeater does not answer
   (`settings.dmr_noanswer`): free running or abort.
4. Calibrate: `lcal` and then the parrot pass, once. The `RPT` row of the quick
   menu shows `SYNC sNNN` when the alignment is in use, `FREE` when it is not,
   `NO CC` when it was refused, `LAT?` when the latency is not known well enough
   and `WAKE` while the activation is in progress.

Over the remote control (`-DC62_REMOTE=ON`): `rpt` prints the whole state in one
line (`cc`, `ts`, `state`, `gate`, `ppm`, `q`, `sh`, `lat`, `cal`), `rcal` starts
the parrot pass, `lcal` starts the audio measurement.
