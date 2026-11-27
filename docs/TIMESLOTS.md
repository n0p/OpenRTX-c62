# Timeslots: which modes have them, and what the radio does with them

A timeslot only exists where the air interface divides time. This radio speaks
two such interfaces, and the rest are frequency-division:

| mode | air interface | slots | what the radio does |
|---|---|---|---|
| DMR  | TDMA, 2 slots in a 60 ms frame | TS1, TS2 | the `TS` row of the quick menu: `AUTO`, `TS1`, `TS2` |
| TETRA | TDMA, 4 slots in a 60 ms frame | TN1…TN4 | receive only; follows the slot the call is in |
| NXDN | FDMA, 6.25/12.5 kHz channels | — | nothing to select |
| dPMR | FDMA, 6.25/12.5 kHz channels | — | nothing to select |
| P25  | Phase 1: FDMA, C4FM (Phase 2 would be TDMA and is not implemented) | — | nothing to select |
| YSF  | FDMA, 12.5 kHz | — | nothing to select |
| D-STAR | FDMA, 6.25 kHz GMSK | — | nothing to select |
| M17  | FDMA, 9 kHz | — | nothing to select |
| FM, AM/airband, WFM | FDMA | — | nothing to select |

A row for a mode that has no slots would be a menu entry that cannot do
anything, which is what `tests/unit/ui_menu_audit.cpp` refuses.

## DMR: the two slots

`AUTO` (the third answer of the `TS` row) follows the call:

- The receiver listens to both slots. The first one that carries a call is
  taken, and the receiver stays with it — audio, identifiers, colour code and
  the number on the main screen — while the call lasts.
- The call ends on its own terminator. A call that simply stops, with no
  terminator on the air, ends **five seconds** after its last voice burst
  (`VOICE_AUTO_HOLD_MS`, `protocols/DMR/dmr_voice.c`). Five seconds is a
  conversation's gap, not a burst's.
- Only then does the receiver listen to both slots again.

`TS1` and `TS2` are the operator's answer and are used as they are, receiving
and transmitting. In `AUTO` the transmitter answers in the slot the call is
being heard in — the slot the repeater is repeating — and in TS1 when nothing
has been heard (`OpMode_DMR::txTimeslot`).

Two things override or refine that:

- **A stored channel carries its own timeslot** and, when the channel is
  selected, it is written into the settings (`chstore_applySettings`). The
  quick menu is the source of truth at run time, but selecting a channel sets
  it from the channel.
- **A trunked system grants the slot** (T3, `dmrTrunk_poll`). The grant is the
  answer: the network assigns the slot, so the receiver follows it and the
  configuration's slot only means something when there is nothing granted.

What the screen reads is the *call's* slot, not the slot of whichever burst
came last: on a repeater with the other slot busy, taking it from the last
burst made the number alternate and the identifiers blink while the audio was
fine (`DmrStatus.c`, and the `AUTO`/stability cases in
`tests/unit/dmr_rx_status.cpp`).

## TETRA: the four slots

The TETRA receiver decodes every downlink burst and one decoder plays one
stream, because two calls on different slots would otherwise be interleaved
frame by frame (`protocols/TETRA/TetraSpeech.c`):

- The slot of the first speech heard is followed, and the others are counted
  and ignored.
- The slot is kept for **five seconds** after its last speech — the same hang
  as DMR's `AUTO`, one answer for both TDMA modes — and then the receiver is
  free to follow another call.
- The slot being followed is published (`tetraStatus.tn`) and drawn on the call
  screen (`Voice, call 8 TN2`, shortening to `Voice TN2` and then `Voice`).

There is no slot *selection* for TETRA: the mode is receive only and a monitor
does not belong to a cell, so there is no assigned slot to listen to. What it
does is follow what is being said.

## Why the hold is not zero

Giving the slot up as soon as a burst is missing hands the speaker to whoever
else is talking: the operator hears the first half of a sentence, a burst of the
other call, and then the rest. Both TDMA modes hold, for the same five seconds,
and both release on a clock — the DMR one from `dmr_voice_tick()`, the TETRA one
from its own playback path — so a call that goes quiet without saying goodbye
still ends.
