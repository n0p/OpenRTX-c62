# The DMR receiver: an automated stand-in for the radio

**Branch** `t/dmr`, from `4b8af88d`.
**Hole** `docs/TEST_PLAN.md`, section 1, and the paragraph after the table: no
digital mode had a *stand-in for its receiver* — nothing fed its receive path
with samples and checked the state it publishes. Everything that goes wrong
between the demodulator and the screen (a colour code that dances, a call
announced with the wrong identifier, a quality figure stuck at 100 %) only
showed on the radio, with a repeater in front of it.

**Test** `tests/unit/dmr_rx_status.cpp`, registered as `DMR RX Status Test`.

---

## 1. What the stand-in is

```
  our modulator (protocols/DMR/dmr_tx.c)          what a repeater adds
  ┌──────────────────────────────────────┐        ┌────────────────────────┐
  │ dmr_tx_header / _voice / _terminator │  +     │ 24-bit CACH (TACT: TC) │
  │ BPTC(196,96), RS(12,9), EMB, AMBE    │        │ BS-sourced sync word   │
  │ interleave, RRC 4FSK, 48 kHz, 10 Sa/s│        │ 30 ms slot, no guard   │
  └──────────────────────────────────────┘        └────────────────────────┘
                              ↓ 144 dibits = 1440 samples per 30 ms slot
        ┌───────────────────────────────────────────────────────────────┐
        │ dmr_demod_feed_disc()  ← 1440 samples, floats in chunks of 160 │
        │ dmr_voice_decode_next() (what DmrSpeech.c's thread does)       │
        │ dmrStatus_update()     → dmr_getStatus()                       │
        └───────────────────────────────────────────────────────────────┘
```

* The payload is the transmitter's: the same calls `dmr_tx_header()`,
  `dmr_tx_voice()` and `dmr_tx_terminator()` make, so BPTC, RS(12,9), the
  embedded LC and the AMBE+2 interleave are the ones the radio transmits.
* The repeater wrapping is the test's: the 12-dibit CACH with its Hamming(7,4)
  TACT (AT, TC, LCSS — the order `dmr_proto.c` decodes) and the BS voice/data
  sync word. A repeater re-transmits a mobile's payload with its own sync and
  CACH, which is exactly this.
* The call runs on one timeslot; the other carries a repeater's idle bursts
  (slot type 9), so the slot phase has to be learned and followed, not just
  read once.
* The blocks are the mode's: 1440 samples, converted to float in chunks of 160,
  `dmrStatus_update()` after each block — `OpMode_DMR::feed()`.

The whole receive chain is linked: `dmr_demod.c`, `dmr_fec.c`, `dmr_trellis.c`,
`dmr_proto.c`, `dmr_data.c`, `dmr_lrrp.c`, `dmr_track.c`, `dmr_voice.c`,
`dmr_sms.c`, `dmr_ep.c`/`dmr_aes.c`/`dmr_hytera.c`, the Tier III engine, and
`lib/dmr_vocoder` as `DMR_VOICE_OWN` (what the C62 builds). `DmrStatus.c` is the
real one: the emulator's stub (`platform/targets/linux/emulator/ui_mode_status.c`)
is deliberately **not** linked, or the test would be reading a fixture.

`-O2 -ffp-contract=off`, as the firmware builds the demodulator: its decisions
are margins and they move at `-O0`.

## 2. What is covered

| # | case | asserts |
|---|---|---|
| 1 | a repeater call on TS1, CC 1, 1234567 → TG 9 | every block: sync; colour code 1 on both slots; TS1 blocks: voice, ids valid, group, 1234567 > 9; TS2 blocks: not voice, no ids; CACH phase established (both slots seen ≥ 15 of 40 blocks); quality 1000; bursts, voice bursts, AMBE+2 frames and PCM samples all non-zero; demodulator locked, BS-sourced, outer-symbol level ≈ the transmitted deviation |
| 2 | the same call on TS2 | the CACH's TACT, not the order of the bursts, decides: TS2 blocks carry the call, TS1 blocks do not |
| 3 | 1 s of silence then 2 s of noise at full scale | never locks; sync false; colour code −1; timeslot 0; ids invalid; src = dst = 0 |
| 4 | a voice LC header whose two octets were changed *after* its RS parity | the call is not announced (no ids, no identifiers), while the burst is still heard (sync, colour code, timeslot); the same stream with the header intact **is** announced, so the case is about the checksum |
| 5 | one voice burst whose EMB word only decodes with a correction, onto another colour code | the published colour code stays 1; a *clean* burst announcing CC 5 **is** published (this receiver is a listener, not a colour-code filter) |
| 6 | a text message as packet data, one payload octet of its last block changed | `crcChecks` > 0 with `crcOk` = 0 when broken, and `crcOk` = `crcChecks` when intact |
| 7 | the embedded LC words of every fifth voice burst replaced by one their code cannot correct | quality falls below 1000 and stays above 0; the call, its colour code, its identifiers and its audio stay |
| 8 | the same call, ended with a terminator | the call's slot goes idle — not voice, `lastType` 9 — and the identifiers stay published until the next call, which is what `dmr_proto.h` promises |

1831 assertions, runtime ≈ 0.1 s.

## 3. What it found

**Fixed in this branch.** A colour code that only came out of an error
correction used to be published. Golay(20,8) (slot type) and QR(16,7) (embedded
LC) always land on *some* word, so a burst with an error in it decodes onto a
different colour code — the screen showed CC 1, CC 5, CC 1 while the audio was
perfectly fine. `dmr_proto.c` now publishes a corrected field's colour code only
when it agrees with the one already heard (`cc_publishable()`); with the guard
taken out the test fails on `5 == 1`.

**Found, not fixed — the call's identifiers flicker on a busy repeater.**
`DmrStatus.c` takes ids, colour code, activity and timeslot from *the slot of
the last burst*. With a call on TS1 and anything at all on TS2, `idsValid`,
`src` and `dst` are therefore only published on every other 30 ms block: the
main screen's `1234567>9` row is there, gone, there, gone. The stand-in shows it
directly (the ids assertions have to be restricted to the call's own timeslot)
and it is why the quality case first failed on `idsValid`. Not touched here
because the fix is a change of what the screen shows: the identifiers should
come from the last decoded link control, not from the last burst. That is a
decision for the owner, and the test will hold it down either way.

**Found, not fixed — two dead fields.** `dmrStatus.accessType` and `.lateEntry`
are declared in `DmrStatus.h` ("CACH access type of the last burst", "CACH late
entry flag") and never written by `DmrStatus.c`, and nothing reads them. Either
they get filled from the CACH TACT (which `dmr_proto.c` already decodes: AT is
bit 6 of the TACT word) and from the LC service options, or they should go.

**Found, not fixed — the mode named by the test plan is dead code.**
`OpMode_DMR_RX.cpp` is not compiled (`docs/DMR_PKG.md`: superseded by
`OpMode_DMR.cpp`, which is the one `CONFIG_C62_DMR_RX` builds). It also never
calls `dmrStatus_update()` at all — if it were ever revived, the screen would
stay empty. The live receive path is `OpMode_DMR::feed()`, and the stand-in
mirrors that one.

## 4. What fails when the behaviour breaks

The tests were checked against mutants of the production code (each one built
separately, never committed):

| mutation | result |
|---|---|
| `dmr_rs_12_9_decode()` returns true without checking | case 4 fails: `src 1234439 != 1234567` — a call announced with a corrupted identifier, which is what the checksum exists to prevent |
| `cc_publishable()` taken out of `dmr_proto.c` | case 5 fails: `5 == 1`, the dancing colour code |
| the CACH sliced one symbol off in `dmr_demod.c` | six of the seven cases fail (timeslot, colour code, ids) |
| `linkQuality()` in `DmrStatus.c` hardcoded to 1000 | case 7 fails: a figure that is not a measurement |

## 5. What is not covered, and why

* **`OpMode_DMR::update()`/`feed()` themselves.** They need the Zephyr kernel,
  the audio streams, the radio driver and the settings records. The stand-in
  calls the same C functions in the same order, with the same block size, but a
  change *inside* `feed()` (dropping `dmrStatus_update()`, as
  `OpMode_DMR_RX.cpp` does) would not be seen here. That belongs to the
  simulator (`openrtx_c62_sim`) or to a compiled-out shim of the mode.
* **The BK4819 receive path: the `REG_7E` high-pass cleared for digital modes
  and `REG_43` = `0x7e48` in 12.5 kHz channel mode (`C62_DMR_12K5`).** The
  baseband here starts at the discriminator, so a high-pass in the chip is
  invisible to it. It is testable without a radio: `BK4819.c` needs only a
  mocked SPI bus and a register shadow, and then "bypass means
  `REG_7E<2:0>` = 0" is an assertion. Not done in this branch; it is the next
  cheapest hole to close.
* **The 7 KiB voice stack.** The floor is a `_Static_assert` in `DmrSpeech.c`,
  which the firmware build checks; `DmrSpeech.c` cannot be compiled on the host
  (Zephyr kernel, audio API).
* **MS-sourced (simplex / hotspot) streams.** Only repeater framing is
  generated: no CACH, no timeslots, bursts 60 ms apart, and the colour code
  comes from the embedded LC alone. Worth its own case; the generator already
  has the pieces (`dmr_tx.c` makes MS bursts natively).
* **The vocoder's audio.** The test counts the frames that reach it and the
  samples that come out; whether they are the right samples is compared against
  captures by `unit/imbe_*` and `tests/host/dmr_pkg_host`.
* **Encrypted calls, talker alias, GPS/LRRP payloads, SMS text, Tier III
  states.** Outside the published status; some are covered by
  `unit/dmr_glue`, `unit/dmr_sms_format`, `tests/host/dmr_pkg_host` (its
  `t3_probe`).
* **Anything RF**: sensitivity, BER at a given Eb/N0, deviation, occupied
  bandwidth. Bench (`tools/bench/dmr_baseband.py`).

## 6. How to run it

```sh
meson setup build_linux -Dcpp_args=-fpermissive
ninja -C build_linux dmr_rx_status_test
meson test -C build_linux --no-rebuild 'DMR RX Status Test'
```

The meson target is the supported way: it is the only place where the source
list lives, and it is the same list that is linked for the firmware minus the
mode and the drivers. Passing the test binary a test name (`dmr_rx_status
"silence" --reporter compact`) runs one case, which is what the mutation runs in
section 4 did.
