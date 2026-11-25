# Transmit safety: what may key the PA

A change of mode keyed the PA once, on the DMR mode, with nobody pressing the
PTT, and the radio then stayed on the air until it was switched off. The
transmitter is the one peripheral that can hurt the radio and the spectrum
around it, so the rule is not "the modes are careful": it is that only one
condition keys the PA and only one function drives it.

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

## The rule (rtx/KeyGate.h)

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

## The watchdog

`rtx_supervise()` (main thread, every 5 ms) unkeys **first** and then cancels
the mode's streams, which is what wakes a thread blocked in one of them; the RTX
thread repeats the unkey when it takes the abort and, after the mode's update,
puts the radio back in RX if the mode did not. The guard goes back to idle on
every failed entry to transmit, so a refused PTT cannot read as a stuck
transmitter 250 ms later.

## Tests

- `tests/unit/key_gate.cpp`: the rule itself -- held across a change, a press
  during it, the release requirement, the hold time, the settle time and the
  unattended grant.
- `tests/unit/mode_switch.cpp`: all eleven modes selected with the PTT injected
  as pressed (no keying); a press during a change (no keying) followed by a
  release and a press (keys once, then back to RX); `radio_enableTx()` called
  outside a permitted transmission (refused); a mode the build lacks (PA off,
  RX); a simulated supervisor timeout (PA off, RX).
- `tests/unit/mode_walk.cpp`: the TX to RX loop over one mode object, after the
  eleven-mode walk.

The DMR transmit chain itself is not linked into the host build (demodulator and
vocoder), so its call loop is verified on the radio; what the host proves is the
decision that starts it.
