# Transmit safety: what may key the PA, and for how long

Two things went wrong with the transmitter of this radio, a week apart.

The first was a **change of mode keying the PA**: selecting DMR, with nobody
pressing the PTT, put the radio on the air and left it there until it was
switched off. The answer to that is a rule about *who* may key, and it is
`rtx/KeyGate.h`.

The second was worse, because it happened after the first was fixed: the radio
**transmitted until the battery was flat** and came back so hot that it would
not switch on. Nothing had asked it to keep transmitting; the thread that should
have unkeyed was blocked, and the mode -- or the DMR message path, which keys
with no PTT at all -- had left the PA keyed with nobody holding a phase for it.
The answer to that one cannot be a rule about who may key: it has to be a clock
that only knows whether the PA is on the air, and ends the transmission when it
has been on the air too long. That is `rtx/TxWatch.h`, this document's subject.

The transmitter is the one peripheral that can hurt the radio and the spectrum
around it, so the rule is not "the modes are careful": it is that only one
condition keys the PA, only one function drives it, and one clock decides that
it has been keyed for long enough.

## The incident

Selecting DMR ran `switchMode()`: the chip was retuned for DMR, the old mode
was destroyed, `OpMode_DMR` was constructed in the shared block and enabled.
The RX branch of `update()` then asked `platform_getPttStatus()` -- the *level*
of the PTT line -- and started the call if it read pressed:

```c
if (platform_getPttStatus() && (rtx->txDisable == 0) && !txBlocked)
```

Three things could make that level read pressed with no operator: a PTT held
across the change, a level sampled while the chip was being reconfigured, and a
mode's own latched state. Worse, `needDataTx()` keys with **no PTT at all** --
a queued short message goes out on its own -- and that path did not touch the
transmit guard, so the supervisor had no phase to time out. A call that blocked
in its first stream write therefore left the PA keyed for as long as the
battery lasted, with the RTX thread stuck in `outputStream_sync()`.

The host tests reproduce it: `tests/unit/mode_switch.cpp` walks all eleven
modes with the PTT injected as pressed, and with the old rule in `KeyGate.h`
two of its cases fail with the PA keyed.

## The rule for keying (rtx/KeyGate.h)

`keyGate_pttKey()` is the only thing a mode may ask before transmitting:

- a mode change vetoes keying from its start until `KEYGATE_GUARD_MS` after its
  end, whatever the PTT says;
- the PTT has to have been **released** once since the change: the level that
  was there before is not the operator asking to transmit;
- the press has to last `KEYGATE_HOLD_MS`, so one bad sample cannot key;
- an unattended transmission (queued SMS, APRS, the packet modem) waits
  `KEYGATE_SETTLE_MS` after the change, never runs with the PTT down during it,
  and is only permitted inside a grant (`keyGate_grantUnattended()`), so a mode
  that keys with the PTT up cannot pass for one.

The consequence the operator sees: after changing mode, the first PTT press
transmits only if it started after the change (or after releasing a PTT that was
already held), and keying starts one 30 ms tick later than before.

## The choke point

Every platform's `radio_enableTx()` refuses unless `rtx_txPermitted()` says a
permitted transmission is in progress:

```c
if (!rtx_txPermitted())
    return;
```

That single line is what makes a mode that forgets harmless, and it also covers
the paths that are not a mode: `core/packet_service.c` keys the PA from its own
thread and now has to be in FM, settled, and with a grant; `OpMode_FM`'s APRS
transmitter asks the unattended gate before it opens its streams.

## The mode change (rtx.cpp)

`switchMode()` does, in this order:

1. `switching = true` and `keyGate_beginTransition()`: nothing keys from here;
2. `radio_disableRtx()` and `radio_txPathOff()` -- the PA enable lines are read
   back from the chip (REG_33 on the C62), not trusted from a variable;
3. `radio_setOpmode()`, the old mode's `disable()` and destructor, the block
   back to the arena;
4. the new object constructed in the block and `enable()`d;
5. `keyGate_endTransition()` and `switching = false`.

If the mode cannot be constructed -- the arena refuses the block, or the object
does not match the registry -- the radio is left on `OPMODE_NONE`, PA off and
**in RX**, and the log says so.

## The absolute transmit clock (rtx/TxWatch.h)

`txw_sample(clock, now, keyed)` is fed with one thing: whether the PA is on the
air. It accumulates **air time** -- not transmission time, not message time --
since the PA was last released, and `txw_tick()` returns `TXW_ACTION_CUT` at
`TXW_LIMIT_MS`.

| Constant | Value | What it is |
|---|---|---|
| `TXW_LIMIT_MS` | 90 000 ms | the supervisor's cut |
| `TXW_MARGIN_MS` | 5 000 ms | what the driver's own barrier allows over it |
| `TXW_DRIVER_LIMIT_MS` | 95 000 ms | the driver barrier's cut |
| `TXW_RELEASE_MS` | 1 000 ms | off the air this long is a release |

**Only a release puts it back to zero.** A release is the PA being off the air
for `TXW_RELEASE_MS`. Everything else continues the same count:

- a mode that re-keys immediately after its over (the gap is one slot);
- a chain of unattended messages -- the count is of the air, not of the
  message, so forty-five 2 s SMS cannot put 90 s on the PA;
- a change of mode -- the PA is off for 150 ms while the block is rebuilt, and
  that is not a release;
- every status update in between: `txw_sample()` is called every 5 ms and a
  sample that does not change the level changes nothing.

Once it has cut, the clock is **blocked**: `txw_keyAllowed()` is false, so
`rtx_keyingAllowed()`, `rtx_unattendedTxAllowed()` and `rtx_txPermitted()` all
refuse and the driver never keys, whichever mode is running and whatever the
gate says about releases and hold times. It stays blocked until the radio has
been off the air for a release, which is the "next release edge" the operator
has to give it.

### Where the count is taken

`rtx_supervise()` runs in the **main thread** every 5 ms and samples
`radio_getStatus()`, so the clock starts at the first sample that finds the PA
keyed -- within 5 ms of the driver driving the lines. It is not told by the
modes: a mode that keys without reporting a phase, a message path that keys with
no PTT, and a PTT the operator is holding all look the same from there, which is
the point. The thread that owns the transmission is the one that may be stuck,
so the clock cannot live in it.

When it cuts, the supervisor takes the same path as any other abort:

1. `radio_disableRtx()` and a read-back of the PA enable latch (REG_33);
2. `currMode->abortTx()` -- the mode cancels its streams, which is what wakes
   the RTX thread if it is blocked in one, and unwinds through its own exit;
3. `rtxStatus.opStatus = OFF`, and the RTX thread repeats the unkey and puts the
   radio back in RX when it takes the abort.

and it leaves one line in the log:

```
TX TIMEOUT 90s: mode=DMR src=ptt air=90105ms ph=run | TX n=7 ab=0 ph=abort ... | 0:enter 31:open ...
```

`mode` is the mode that was running, `src` is who keyed it (`ptt`, `unattended`
or `mode`), `air` is the air time at the cut, and the rest is the transmit
guard's trace of that transmission, so the operator's report says what the radio
was doing when it stopped itself.

## The transmit guard (rtx/TxGuard.h), and what it is not

The guard is the other half of the picture and it answers a different question:
*where* a transmission is, not how much air it has put out. The modes report
their phases as they go -- `starting` (PTT seen, streams opening), `wait_first`
(streams open, no block handed to the sink yet), `running`, `draining` (PTT
released, terminator in flight) -- each with a budget (`TXG_START_MS`,
`TXG_FIRST_MS`, `TXG_DRAIN_MS`), and `txg_tick()` flags the stage that overran.
That is what catches a stream that never opens or a tail that never drains, and
its trace is what the `TX TIMEOUT` line and `PC_MSG_DBG_TX_TRACE` print.

It cannot be the last line, because it only knows what the modes tell it: a
keying nobody reported has no phase to overrun. Its own cap, `TXG_MAX_TX_MS`
(five minutes), is kept only as a backstop that the 90 s clock now makes
unreachable in practice.

## The second barrier, in the driver

The supervisor is a thread. The barrier is not: `radio_enableTx()` and
`radio_disableRtx()` feed a second clock with `TXW_DRIVER_LIMIT_MS`, and it has
two halves:

- **the clock** refuses a keying once the bar is spent and forces the lines back
  down, so a caller in a loop -- a mode retrying, a configuration update, a
  message queue that keeps asking -- gets "no" every time;
- **a `k_timer`**, armed at each keying for whatever is left of the bar,
  expires 95 s later and submits a work item to the system work queue, which
  performs the normal unkey. That half is what covers the case the supervisor
  cannot: the supervisor not running at all.

So the belt is the supervisor's 90 s, with the mode unwinding; the braces are
the driver's 95 s, from a timer, with no thread of ours in the path.

Both barriers count from the same events and neither resets on its own: only a
release does.

## Booting

`platform_init_csk6()` calls `radio_paForceOff()` as soon as the transceiver
answers -- before the DSP, the audio, the codec and any thread exist -- and
`radio_init()` repeats it as the driver's own contract. It drives the three
enable lines (PA VHF, PA UHF, ALC/bias) low and **reads REG_33 back**, three
times if it has to: what the chip latched is the truth, and the radio that came
back hot is the proof that a latch can survive a reset. The log says which:

```
PA: off at boot, REG_33 latch clear
PA: STILL KEYED AT BOOT: REG_33 latch set
```

## What the radio transmits with: frequency and power (the VFO case)

A channel carries its own transmit frequency and its own power, so a radio on a
memory channel always had something to key with. A **VFO has neither**: it is a
frequency and a mode, and `state.channel` is the record the UI happens to be
editing, which for a VFO comes from flash and can be as empty as the day it was
written. That produced the worst kind of failure, reported from a radio with an
empty bank and reproducible every time:

- `state.channel.tx_frequency == 0` → `radio_enableTx()` refused it with a bare
  `return` **after** dropping the enable lines and before driving PA_VHF_EN,
  ALC_TX_LED_EN or the chip's TX: no RF, no red LED, nothing in the log.
- `state.channel.power == 0` → the PA came up at the driver's minimum duty (it
  clamps to 1 W) or, on the bench, keyed with nothing behind it and
  `tx_state = 2` in the status.

Both numbers now come from one place, `core/state.c`, and neither can be zero by
accident:

| what | rule |
|---|---|
| `state_txFrequency()` | the channel's TX if it has one, otherwise RX (a VFO is simplex). It is what `threads.c` puts in the RTX configuration. |
| `state_txPowerMw()` | the channel's power if it has one, otherwise `settings_t.tx_power` (default **500 mW**, `SETTINGS_VERSION` 7). |
| `state_keyingPowerOk()` | false when the result is 0: `rtx_keyingAllowed()` then refuses and `rtx_keyingVetoReason()` says `no tx power`. |

`state_init()` also fills `tx_frequency` from `rx_frequency` when a VFO record
comes up without one, so the value is right even for a record written by an
older firmware (which is discarded as a whole on the version bump anyway).

A power of 0 is still allowed in the setting, on purpose: an operator may want a
radio that cannot transmit. What is not allowed is a *silent* zero. Three places
refuse it and all three say so:

1. `rtx_keyingAllowed()` -- the gate, with `rtx_keyingVetoReason()` = `no tx
   power`, which the protocol status and the diagnostics report.
2. `pc_bench_ptt()` -- the bench answers `invalid_value` and does not key.
3. `radio_enableTx()` -- the driver's own check, `PA: keying refused, TX power
   is 0 mW` in the log, throttled to one line a second.

The same treatment for the frequency: `radio_enableTx()` used to drop out of
band silently. It now prints `PA: keying refused, TX <n> Hz outside 136-600 MHz`
and leaves `radio_txRefusedReason()` = `tx freq out of range` for the status.

### Visible from the PC

`PC_MSG_STATUS_GET` (and every status event) carries `PC_TAG_TX_POWER` (u16, mW
the radio would key with) and `PC_TAG_TX_REASON` (`none`, `no tx power`, `tx
disabled`, `tx freq out of range`, `wfmbc rx only`, or what the key gate is
waiting for). `PC_MSG_DBG_INFO` carries the same two fields. `cps.py status`
prints them, so "the PTT does nothing" is answered by one command over the
cable, without opening the radio:

```
$ tools/cps/cps.py -d /dev/ttyUSB0 status
...
power_mw            500
tx_reason           none
```

## What keys the transmitter: the accessory pad (docs/ACCESSORY.md)

`platform_getPttStatus()` is the other way in, and on this radio it is not a
button: GPIOA_15 is the accessory jack's PTT *and* UART2 RXD, so a level read at
random is a level that goes low for the start bit of every byte the PC sends.
Keying from it directly would put the radio on the air on a `HELLO`.

The answer is the same shape as the rest of this document -- one rule, one
place -- and it lives in `core/port_ptt.h`:

| port mode (`Settings ▸ Radio ▸ PC Port`) | what `platform_getPttStatus()` answers |
|---|---|
| `Auto` | the sustained-low detector, fed by a window: the pad is muxed to GPIO for about 130 ns every 4 ms, read, and put back on the UART; **more than 30 ms** of it low is a press (a clean character is not evidence, a 1-3 ms break is not a press) |
| `Always` | `false` unless the remote/bench override is set: the pad belongs to the cable |
| `Never` | `gpio_pin_get_dt(&button_ptt)`, the plain level, because the pad is a plain GPIO with its pull-up |

The GPIO level read *with the pad still on the UART* is not the source in `Auto`:
there it does not follow the line (it reads low from boot, which is how the first
version of this detector keyed the radio with nobody touching anything). Only
inside the window -- pad muxed to GPIO for a microsecond at most, then put back
-- is the level the line's own; and the window is skipped entirely while the
receiver is framing clean characters, so a talking client is never disturbed. The
detector also requires a release first, so a pad that is already low when the
radio starts is unknown, not pressed.

`Auto` is the only mode where both work at once (it is selected from the CPS, not
from the menu, until the switch is confirmed on hardware: see
`docs/ACCESSORY.md`); the frames that
arrive while the switch is held are discarded and counted (`PC_TAG_PORT_LOST`),
and the half frame the parser had is dropped. `docs/ACCESSORY.md` has the wiring,
the numbers and the limits. The 90 s air clock above still applies to whatever
this keys: a stuck switch or a shorted pad ends at the same wall as a stuck mode.

## Why 90 s

Three numbers decided it, and only the first is about policy.

1. **A transmission with nobody holding the PTT is an accident.** The longest
   legitimate over on this radio is a voice call with a talkgroup, which is
   tens of seconds; a net control reading out a list is the longest anybody
   keeps a key down, and that is under a minute. 90 s is one and a half times
   the longest plausible one, so it never interrupts an operator and it always
   ends a stuck key before a battery is in danger.
2. **The PA is not built for a continuous carrier.** It is a handheld PA with a
   small thermal mass and no temperature sensor of its own: this radio has no
   way to measure the PA die, so the firmware cannot throttle on temperature
   and must bound the air time instead. A minute and a half of carrier is
   already at the edge; the radio that came back too hot to switch on had been
   keyed for minutes.
3. **The battery is the second casualty.** A keyed PA draws the receive current
   plus the transmit current; there is no thermal cut-out, so the first thing
   the operator notices is that the radio will not turn on.

The 5 s margin between the two barriers exists so that the supervisor is
normally the one that stops the radio -- with the mode unwinding cleanly, the
streams cancelled and the log line written -- and the driver's timer is only the
backstop when the supervisor itself is the thing that died.

## Tests

- `tests/unit/key_gate.cpp`: the keying rule itself -- held across a change, a
  press during it, the release requirement, the hold time, the settle time and
  the unattended grant.
- `tests/unit/tx_watch.cpp`: the clock. 89 s keep transmitting and 90 s cut; a
  release at 60 s starts the count again; a 300 ms gap and a change of mode do
  not; a chain of unattended sends cuts at 90 s of air; after a cut nothing keys
  until a real release; the driver's margin cuts at its own limit.
- `tests/unit/mode_switch.cpp`: all eleven modes selected with the PTT injected
  as pressed (no keying); a press during a change (no keying) followed by a
  release and a press (keys once, then back to RX); `radio_enableTx()` called
  outside a permitted transmission (refused); a mode the build lacks (PA off,
  RX); a simulated supervisor timeout (PA off, RX); **the 90 s cut with the RTX
  thread not running** (PA off, clock latched, keying refused, then the thread
  comes back and the radio is in RX); **the driver's barrier refusing eight
  keyings in a row** (nothing keyed, PA off each time); and `radio_init()`
  leaving the PA off.
- `tests/unit/mode_walk.cpp`: the TX to RX loop over one mode object, after the
  eleven-mode walk.
- `tests/unit/tx_power.cpp`: the VFO route -- a blank VFO record gets its TX
  frequency from RX, its power from the settings and its keying permitted; a
  channel keeps its own both; a setting of 0 is kept as 0, the gate refuses with
  `no tx power` and no minimum is ever conjured.
- `tests/host/pc_proto_host` (the Python suite): the radio booting on a stored
  channel still answers HELLO and reports its frequency and power; a VFO reports
  a valid TX frequency and 500 mW; a VFO with the setting at 0 reports
  `power_mw 0` and `tx_reason no tx power`.

The DMR transmit chain itself is not linked into the host build (demodulator and
vocoder), so its call loop is verified on the radio; what the host proves is the
decision that starts it and the clocks that end it.

## Visibility from the PC

`PC_MSG_DBG_INFO` carries `PC_TAG_TX_SECONDS` (u16, air time since the last
release) and `PC_TAG_TX_REASON` (`idle`, `ptt`, `unattended`, `mode`,
`timeout`; and, since the VFO fix, the refusal words of the section above). `PC_MSG_DBG_TX_TRACE` carries the live guard trace plus

```
tx=43s/ptt air=43105ms cut=0/0
```

where the first `cut` is the supervisor's count and the second the driver
barrier's: the second number must stay at zero, and if it ever is not, the
supervisor was not running. The serial log carries the same in the `TX exit`
line of every transmission.
