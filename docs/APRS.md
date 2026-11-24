# APRS on the analog FM path

APRS (APRS Protocol Reference 1.0.1, `APRS101.PDF`) over AX.25 UI frames
(AX.25 v2.2), Bell 202 AFSK at 1200 baud, on the Retevis C62. The C62 has no
GPS, so positions come from the settings (a fixed station, or a hand-entered
one).

The primary function is **messaging between stations**: receive a message,
show it with the sender's callsign and the time, acknowledge it, reply, and
keep an inbox and an outbox. The **beacon is a separate option** that ships
off, and neither messaging nor the beacon is tied to the PTT: the PTT stays
the voice PTT.

> **APRS is not an operating mode.** It runs inside `OPMODE_FM`: the decoder
> taps the discriminator with a second audio path while the speaker path keeps
> playing, and it only runs when the operator turns decoding on. See
> [Integration](#integration) for the exact hooks.

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_APRS=y     # default: y
```

`CONFIG_C62_APRS` (`platform/targets/c62/Kconfig.board`) adds the protocol
sources, `aprs_service.c` and `ui_aprs.c`, and defines `CONFIG_APRS` for the
rest of the firmware. It costs **23 124 bytes of flash and 12 520 bytes of
RAM** (measured on the full image with and without the option, see
[Cost](#cost)).

At run time everything is off until the operator says otherwise:

| Setting | Default | Where |
|---|---|---|
| `aprs_decode` | **off** | APRS setup, field 1 (`Decode`) |
| `aprs_beaconEn` | **off** | APRS setup, field 2 (`Beacon`) |
| `aprs_msgAck` | on | APRS setup, field 9 (`Ack msgs`) |

## Where the code lives

```
openrtx/include/protocols/APRS/
    ax25.h        addresses, FCS, UI frames
    hdlc.h        flags, zero insertion, NRZI, deframer
    afsk.h        Bell 202 modulator and demodulator
    aprs.h        information fields, station list, distance/bearing
    aprs_msg.h    inbox, outbox, retries, acknowledgements
openrtx/src/protocols/APRS/*.c        the same, freestanding C (no malloc)
openrtx/include/core/aprs_service.h   the glue: tap, TX scheduler, state
openrtx/src/core/aprs_service.c
openrtx/include/ui/ui_aprs.h          screens
openrtx/src/ui/default/ui_aprs.c
```

The protocol sources are also built on the host, unmodified, by the prototype
in `work/aprs` (see `work/aprs/README.md`): the same code that decodes the
reference captures is the code in the firmware.

## AX.25 frame format

AX.25 v2.2 §2.2 and §2.3. Every byte is shifted left by one and sent least
significant bit first; the address field carries the callsign in six
characters plus an SSID byte.

```
|<----------------------- address field ----------------------->|<-ctl->|<-pid->|<---- info ---->|<-fcs->|
  destination (7)  source (7)  digipeater 1 (7) ... digipeater 8 (7)  0x03    0xF0      APRS data    2
```

Address byte layout (AX.25 v2.2 table 2.2):

| Bit | Meaning |
|---|---|
| 7 (C) | Command/response; APRS leaves it 0 and uses it as the "repeated" flag on digipeaters |
| 6:5 | Reserved, always `11` |
| 4:1 | SSID, 0..15 |
| 0 (extension) | 0 for all but the last address, 1 for the last (H bit) |

Control field `0x03` = UI frame; PID `0xF0` = no layer 3 protocol. The FCS is
CRC-16/X.25 (reflected polynomial `0x8408`, init `0xFFFF`, final XOR
`0xFFFF`), sent low byte first, and the receiver checks the residue `0xF0B8`
over the whole frame (`ax25_fcs_ok()`).

HDLC framing (§4.3): the frame is between `0x7E` flags, every run of five
contiguous ones is followed by an inserted zero, and the line code is NRZI (a
zero bit is a transition). `hdlc.c` implements the transmitter and a deframer
that holds runs of ones until the next zero decides whether they were a flag
(six ones), five data bits plus a stuffed zero, or an abort (seven or more).

## APRS information fields

| Type | Field | Format | Used here |
|---|---|---|---|
| Position, no timestamp | `!` | `!DDMM.mmN/DDDMM.mmW<sym><comment>` | yes |
| Position, messaging | `=` | same, `=` says the station can receive messages | yes |
| Position, compressed | `!`/`=` | 1 byte table, 4+4 base-91 bytes, 1 symbol byte | yes |
| Status | `>` | `>text` | yes |
| Message | `:` | `:ADDRESSEE:text{msgno` (addressee padded to 9) | yes |
| Acknowledgement | `:` | `:ADDRESSEE:ackmsgno` | yes |
| Rejection | `:` | `:ADDRESSEE:rejmsgno` | yes |
| Telemetry, weather, objects, third-party | `T`, `_`, `;`, `}` | — | no |

Compressed position (APRS101 chapter 9): latitude `y = 380926 * (90 - lat)` and
longitude `x = 190463 * (180 + lon)` in four base-91 characters each
(`char = value + 33`, most significant first). It saves four bytes per beacon
and, with a long comment, one whole HDLC frame.

Symbol table: `/` primary, `\` alternate, or an overlay character in the same
byte. The default here is `/` with `-` (a fixed station).

## The modem

Bell 202: mark 1200 Hz (binary 1 level), space 2200 Hz (binary 0 level),
1200 baud, 833 µs per bit.

### Transmitter

`afsk_mod_bit()` keeps a 32-bit phase accumulator (one wrap per cycle, a
512-entry Q15 sine table, 0.7° of quantisation) and adds the increment of the
tone for the current NRZI level, so the phase is continuous across bit
boundaries: no clicks, and no spectral splatter outside the channel. The
optional pre-emphasis is a one-pole shelf (+6 dB/oct above 800 Hz) for
transmitters whose audio chain has no pre-emphasis of its own, with headroom
so the boosted high tone does not clip.

TX timing, as APRS101 recommends for 1200 baud:

| Parameter | Value | Why |
|---|---|---|
| TXDELAY | 300 ms | the far end needs to open its squelch and settle its AGC |
| Opening flags | 4 | 33 ms of `0x7E` for the receiver's clock |
| Closing flags | 2 | so the last data byte is flushed even if it ends in ones |
| Amplitude | 0.80 of full scale | -2 dB of headroom for the pre-emphasis |

### Receiver

The discriminator audio arrives at 24 kHz (the codec decimates from 48 kHz in
hardware, which is free). The chain is:

1. DC blocker (the FM detector offset) and an AGC on the *mean magnitude* over
   about two bits. A peak detector is wrong here: the two tones beat during
   every transition, the envelope of an FSK signal swings 2:1, and a peak
   tracker leaves the gain 6 dB off for tens of milliseconds — measured, it
   was the single largest source of bit errors during development.
2. Quadrature correlators at both tones with a **one-bit sliding window**
   (20 samples at 24 kHz), maintained incrementally: four multiply-accumulates
   and four subtractions per sample.
3. Soft metric `m = E_mark - E_space`.
4. Bit clock: the metric crosses zero at the **centre** of a symbol (the
   window is one bit long, so it straddles the transition evenly there), and
   peaks at the symbol end. The loop is anchored to that centre: `mu = 0` at a
   crossing, error `mu - 0` wrapped to ±half a bit, proportional plus integral
   correction, integral clamped to ±0.001 bit per bit (24 baud of pull-in, far
   more than any crystal or Doppler at 145 MHz). Crossings only count when the
   metric was armed by a real excursion (|m| > 0.2 of the total), otherwise
   the dither around zero fires a correction on every sample.
5. Decision: the metric is accumulated over the last 90% of the bit (which is
   where it describes the current symbol and is free of the neighbours), split
   in three slices, and decided by majority with the middle slice weighing
   double.
6. Carrier detect: `energy > 1.2e9 && |m| > 0.5 * (E_mark + E_space)`. The
   second test is what separates data from noise: an FSK signal puts almost
   all of its correlator energy on one tone, white noise splits it.

The whole modem is integer-timed (20 samples per bit at 24 kHz) and needs no
allocation: one `afsk_demod_t` is about 600 bytes of state.

## Verification

### Loopback, AWGN, 3 kHz bandwidth

`./build/aprs_test ber` builds a 75-byte position frame, modulates it at
48 kHz, adds white noise for a given SNR in a 3 kHz bandwidth, demodulates,
aligns the received level stream with the transmitted one and counts bit
errors; frame success is what the HDLC layer delivers with a good FCS. 20
trials per point, random sub-bit start offset per trial.

| SNR (3 kHz) | BER | Frame error rate |
|---|---|---|
| 0 dB | 3.8e-1 | 1.00 |
| 2 dB | 2.3e-1 | 1.00 |
| 4 dB | 1.0e-1 | 1.00 |
| 5 dB | 2.9e-2 | 1.00 |
| 6 dB | 3.7e-3 | 0.80 |
| 7 dB | 1.4e-3 | 0.55 |
| 8 dB | 3.1e-4 | 0.10 |
| 9 dB | 3.9e-4 | 0.05 |
| 10 dB | 0 | 0.00 |
| 12 dB | 0 | 0.00 |

The BER floor of ~2e-4 at high SNR is the acquisition region (the first flag,
before the bit clock is anchored), not the steady state. For reference,
non-coherent orthogonal BFSK at 1200 baud is `0.5 exp(-Eb/2N0)`; with 1200 Bd
in 3 kHz, 8 dB of SNR is 12 dB of Eb/N0, i.e. 1.8e-4 theoretical against 3.1e-4
measured: under 1 dB of implementation loss. Real APRS links run at 10-15 dB,
where this modem has no errors at all.

### Real captures, against direwolf

Five recordings, decoded by this code and by direwolf 1.7 (`atest`, the same
demodulator as the daemon). Frames compared byte for byte.

| Capture | This decoder | direwolf | Identical bytes | What it is |
|---|---|---|---|---|
| `data/aprs_handiko_random.wav` | 68 | 69 | 68 | 221 s of generated APRS (Arduino-APRS), 24 kHz |
| `data/aprs_handiko.wav` | 82 | 25 | 25 | same station, 406 s; direwolf deduplicates repeated frames |
| `media/PACKET_1200.wav` | 46 | 94 | 45 | **over the air**: 64 s of a TSTR1/TSTR2 packet QSO, I-frames, tones 100 Hz low, peak 8% of full scale |
| `media/Radio-packet.wav` | 0 | 0 | — | not Bell 202 (energy at 790-960 Hz): both refuse it |
| `media/PACKET_1200_PSK_Sattelite.wav` | 0 | 0 | — | 1200 baud PSK, not AFSK: both refuse it |

The over-the-air capture is the interesting one. Its frames are I-frames
(`control 0x00, 0x02, 0x14, ...`), not APRS UI frames — a packet-radio test
exchange — and every frame this decoder produced is byte-identical to one
direwolf produced. direwolf gets 94 because it runs **two demodulators in
parallel** and the recording contains collisions: bursts of three frames 120 ms
apart cannot be three sequential 0.77 s packets, they are two stations on the
air at once. Half the frames of that capture are only recoverable from the
weaker of two simultaneous signals, which a single demodulator cannot do.

Two negative controls are included on purpose: the decoder must refuse
signals that are not Bell 202 AFSK, and it does.

## Cost

Measured on QEMU `mps2-an505` (Cortex-M33) with the TCG instruction-count
plugin, the same harness and plugin as `work/dsp-probe` (the
`qemu-mcu-instruction-profiling` skill). Per phase, with the harness's fixed
cost removed (16 685 instructions of startup and printf):

| Phase | Instructions | Per second of audio |
|---|---|---|
| Demodulator, 24 kHz stream | 196 per sample | **4.7 M** |
| Demodulator, 48 kHz stream (decimating) | 117 per sample | 5.6 M |
| Modulator, 48 kHz | 91 per sample | 4.4 M |
| Frame build + HDLC bit stuffing | 34.7 k per packet | — |
| Deframe + AX.25 + APRS parse + mailbox | 106 per bit | 0.13 M |

At 300 MHz with roughly one instruction per cycle, 4.7 M instructions per
second of audio is **under 2 % of the M33**. The modulator only runs while a
packet is being sent (under 1.5 s), and the frame work is per packet, so the
receive path is the only continuous load — and it is zero when decoding is
off, because the audio path is not even opened.

`-O3 -funroll-loops` moves the demodulator by 0.3 %: the compiler is not
leaving anything on the table, and there is no reason to move the modem to the
HiFi4 DSP.

### Cost of the integrated image

| | FLASH | RAM |
|---|---|---|
| `c62/full` (464 932 / 170 392 B) | — | — |
| `c62/full-aprs` (488 056 / 182 912 B) | +23 124 | +12 520 |
| Region use | 46.5 % of 1 MB | 69.8 % of 256 KB |

The RAM figure is the receive tap buffer (2 × 480 samples), the deframer, the
station list and the mailbox. The flash figure is the protocol sources, the
service, the six screens and `printf`-heavy screen code.

## Integration

### The receive tap (no audio interruption)

`OpMode_FM::update()` calls `aprsReceiveStep(channelBusy)` on every pass while
receiving. With `aprs_decode` set and the channel open, the service asks for a
**second** audio path:

```
SOURCE_RTX -> SINK_SPK   (the normal FM path, untouched: audio keeps playing)
SOURCE_RTX -> SINK_MCU   (the APRS tap, 24 kHz, circular double buffer)
```

`audio_checkPathCompatibility()` in `platform/drivers/audio/audio_c62.c` has
to allow that pair. It is safe because the codec already fans a capture out:
`captureFrame()` in `csk6_codec.c` hands every frame to both
`captureToStream()` (the application buffer) and `captureToFmQueues()` (the
passthrough queues), so the discriminator feeds the speaker and the decoder
from the same frame. The change is three lines and is documented in place.

`aprs_rxPoll()` blocks on `inputStream_getData()` for one 20 ms block, which
also paces the mode; the 30 ms sleep at the end of `update()` is then
redundant but harmless.

With decoding off no sample is ever read, and it can be checked by following
the call sites rather than by trusting a flag:

* `aprsReceiveStep()` is the only caller of `aprs_rxBegin()`, `aprs_rxPoll()`
  and `aprs_rxEnd()`, and it starts with
  `want = state.settings.aprs_decode && channelOpen`;
* with `want` false it closes an open tap (releasing the path) and returns
  before the poll, so `inputStream_getData()` — and with it the demodulator —
  is never reached; the poll sits one branch below the gate;
* decoding off since boot means `svc.active` is false, so the close is a no-op
  too: no audio path, no buffer, no CPU.

The only other entry point is the setup screen, which calls `aprs_rxEnd()`
when the operator switches decoding off, so the tap does not survive the
setting by one squelch cycle.

### Transmitting

`OpMode_FM::update()` also asks the service whether a packet is due, and only
when the PTT is **not** held:

```cpp
if ((status->opStatus == RX) && (!platform_getPttStatus()))
{
    if (aprs_txPending(now)) { aprs_txPrepare(...); aprsSendPacket(info); }
}
```

The queue is the only gate: `aprs_txPending()` answers false unless there is a
pending acknowledgement, a queued message or a due/requested beacon, so a
message typed in the APRS screens goes out even with decoding switched off (it
will not see the answer, and the setup screen says so). Decoding and the
beacon stay independent toggles; neither one is a condition for the other.

`aprsSendPacket()` releases both receive paths, requests
`SOURCE_MCU -> SINK_RTX`, keys the radio, and streams the modulated samples
through a circular double buffer exactly as the M17 modulator does: TXDELAY
silence, flags, frame, flags, tail. It blocks for the packet (< 1.5 s), which
is the price of not having a second thread for a job that happens a few times
an hour.

Priority inside the service: acknowledgements first (they unblock the other
station's retry timer), then messages, then the beacon. Between any two
transmissions there is a gap of at least 10 s, and the retry backoff starts
above that gap so a retry can never make the station transmit faster than the
courtesy rule allows.

### Settings

`settings_t` (`openrtx/include/core/settings.h`), inside `#ifdef CONFIG_APRS`
so other targets keep their NVM layout:

| Field | Default | Meaning |
|---|---|---|
| `aprs_decode` | false | decode APRS on the FM path |
| `aprs_beaconEn` | false | beacon transmission enabled |
| `aprs_msgAck` | true | acknowledge received messages |
| `aprs_ssid` | 9 | SSID appended to `callsign` |
| `aprs_lat_e4`, `aprs_lon_e4` | 0 | position, degrees × 10000 |
| `aprs_table`, `aprs_symbol` | `/`, `-` | APRS symbol |
| `aprs_comment[26]` | "OpenRTX C62" | beacon comment / status |
| `aprs_path[24]` | "WIDE1-1,WIDE2-1" | digipeater path |
| `aprs_interval_s` | 600 | beacon interval, clamped to ≥ 30 s |
| `aprs_txdelay_ms` | 300 | TXDELAY |

The settings record is stored as a raw blob with a CRC over `sizeof(settings_t)`
(`platform/drivers/NVM/nvmem_C62.c`), so any change of the struct length
invalidates the stored record instead of being misread: `nvm_readSettings()`
fails its CRC check, `state_init()` falls back to `default_settings` and
writes them back on the next change. The consequence is one settings reset on
the first boot of a build with a different layout — including this merge,
because `c62/full` added `rxFineHz` before the APRS block. With `CONFIG_APRS`
`settings_t` is 156 bytes (88 without), well inside the 256-byte record
`NVM_RECORD_MAX` allows. Safe defaults matter here: `aprs_decode` and
`aprs_beaconEn` are off, so a stray settings record can never make the radio
transmit or spend CPU decoding without being asked.

### UI

Six screens in `ui_aprs.c`, reachable with a **long press of P2 on the FM main
screen** (VFO or memory channel, `OPMODE_FM` only):

| Screen | Green (MENU) | P1 (F1) | P2 (F2) | Red (ESC) |
|---|---|---|---|---|
| Stations | detail | inbox | setup | exit |
| Station detail | message this station | — | next station | back |
| Messages | open / mark read | reply | send beacon | back |
| Message view | reply | — | — | back |
| Message editor | send | delete char | delete char | cancel |
| APRS setup | next field | − | save to NVM | back |

The station list also clears itself on the `#` key (not in the hints row,
which only has four slots), and the setup screen is where decoding is switched
on: field 1, `Decode`. That is deliberate — the entry screen is the station
list and the two things an operator reaches for from it are the inbox and the
settings, not the decode toggle.

The FM main screen shows `APRS 12` (packets received) or `APRS !2` (unread
messages) on the mode line while decoding is on, and nothing when it is off.

#### Key precedence on the FM main screen

`c62/full` already uses P2 for two things, so the APRS entry has to be placed
in that key map rather than added on top of it:

| Key | Where | What |
|---|---|---|
| P2 short | FM VFO / memory | quick menu (unchanged) |
| P2 long | FM VFO / memory | **APRS screens** (new) |
| P2 long | every other screen and mode | macro grid (unchanged) |
| Side key (MONI) short or long | everywhere | macro grid (unchanged) |

The rule is: in FM, P2 long belongs to APRS; everywhere else it keeps the
macro grid, which stays reachable in FM from the side key. APRS is therefore
never reachable by accident (a long press, on one mode, from the main screen)
and never takes a key from the quick menu, which owns the keypad while it is
open — the APRS dispatch sits after the quick-menu block in `ui_updateFSM()`
and before the macro one. It is also skipped while the keypad is locked, while
the UI is editing or while the channel list is open.

While an APRS screen is on, the screens own the keypad (`ui_aprs_fsm()`
consumes every key) and the PTT is disabled the same way the channel list
disables it, so nothing transmits by pressing PTT in a menu; leaving APRS
re-arms it.

#### Hints row

The APRS screens draw their own four hints with their own helper, at the same
place the shared row goes (`SCR_H - 9`, 5 pt font). `ui_hints_for_screen()`
returns `NULL` for the six APRS screens, and `ui_hints_draw()` does nothing
with a `NULL`, so the shared row neither draws over them nor leaves a stale
one behind. When the APRS screens get translated strings they can move to
`ui_hints_draw()` without changing their layout.

The screens are still self-contained: apart from the enum values in
`ui_default.h`, the hooks are one dispatch block, one entry block and one draw
case in `ui.c`, one status line in `ui_main.c`, and `aprs_service_init()` in
`ui_init()`. Everything else is new files.

## Good practice implemented

* Never repeats its own packets: source callsign or own callsign in the path is
  dropped (`isOwnPacket()`), which is what stops a digipeated copy coming back
  and being processed.
* One transmission at a time, at least 10 s apart.
* Beacon interval clamped to ≥ 30 s, and the beacon is off by default.
* Messages are retried (`12 s, 24 s`, three attempts) and stop on `ack`/`rej`;
  duplicates are not stored twice but are acknowledged again, which is what the
  sender needs.
* No digipeating, no igating, no third-party traffic: the station transmits
  only its own traffic.
* The callsign is always the AX.25 source, so the station is identified.

## Out of scope, and why

| Feature | Why not |
|---|---|
| Digipeater (`WIDE1-1` fill-in) | Needs a receive-then-transmit scheduler with per-packet state and duplicate suppression across minutes, and turns a handheld into infrastructure. The C62 also has one receiver: it cannot hear while it repeats. |
| IGate | Needs an internet link. The C62 has no Wi-Fi or IP stack. |
| SmartBeaconing | Needs a GPS. The C62 has none (`CONFIG_GPS` does not exist for this board); the position is fixed or typed in. |
| GPS / moving position | No hardware. The position is in `settings_t`, edited from the APRS setup screen. |
| Telemetry, weather, objects, third-party | Not messaging, and each one is its own little format with its own tables. |
| Two parallel demodulators | What direwolf uses to recover both signals of a collision. Worth doing (it is the same state machine twice, ~600 bytes and 2 % of the CPU) but not needed for messaging on a clear channel. |
| FX.25 / IL2P forward error correction | Different framing; adds ~2 dB but needs both ends to use it. |
| Mic-E encoded position | Meant for voice-channel bursts and needs a radio that supports it; the compressed position already saves the bytes. |

## Merge with `c62/full`

The stack was written against `c62/all` and merged into `c62/full` (the UI with
the quick menu, the hints row, the channel list and the editor) as
`c62/full-aprs`. The patch applies to `c62/all` unchanged; the merge joins that
commit with `c62/full`.

| File | Change | Merge |
|---|---|---|
| `openrtx/include/core/settings.h` | one `#ifdef CONFIG_APRS` block of fields and its defaults | **conflict**, both sides appended at the end of the struct: `rxFineHz` kept first, the APRS block after it, still last and still inside its `#ifdef` |
| `openrtx/include/ui/ui_default.h` | six values in `enum uiScreen` | clean (appended before `LOW_BAT`) |
| `openrtx/src/ui/default/ui.c` | draw case, FSM dispatch, long-press entry, `aprs_service_init()` | **moved by hand**: the entry used to sit before the quick menu and now sits after it, before the macro grid, with the `txDisable` handling the channel list uses |
| `openrtx/src/ui/default/ui_main.c` | the APRS status line on the FM main screen | clean |
| `openrtx/src/rtx/OpMode_FM.cpp/.hpp` | the receive step, the transmit attempt and `aprsSendPacket()` | clean; the transmit attempt lost its settings gate (see above) |
| `platform/drivers/audio/audio_c62.c` | three lines in `audio_checkPathCompatibility()` | clean; a sink was compared with `SOURCE_MCU`, now `SINK_MCU` (same value, right type) |
| `platform/targets/c62/{CMakeLists.txt,Kconfig.board}` | sources and the `CONFIG_C62_APRS` option | clean |

Everything else is new files. The `settings.h` block must stay last in the
struct and inside its `#ifdef`, because the NVM record is a raw blob.

Two things the merge exposed and fixed:

* `aprs_service_init()` had no caller: the service was left zero-initialised,
  with `path` and `stream` at 0 instead of -1. It now runs from `ui_init()`.
* the setup screen was unreachable and the inbox only reachable as the way
  back from a reply, so the callsign, the position, the path and the beacon
  interval could not be set from the radio. The station list now has the inbox
  on P1 and the setup screen on P2, and the decode toggle moved into the setup
  screen, where it can close the tap at once.

## References

* APRS Protocol Reference 1.0.1 (APRS101.PDF, 2000): chapters 3, 8, 9, 12, 16.
* AX.25 Link Access Protocol for Amateur Packet Radio, v2.2 (1998): §2.2,
  §2.3, §3.5, §4.3, §6.1.2.
* Bell 202 / ITU-T V.23 tone plan: mark 1200 Hz, space 2200 Hz.
* direwolf 1.7 (`atest`), used as the reference decoder for the captures.
* `work/aprs/` (this tree's prototype): the host build of the same sources,
  the three test programs and the captures.
