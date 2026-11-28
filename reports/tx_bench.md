# Five modes that put no carrier on the air: the host bench, and what it found

**Branch** `tx/bench`, worktree `/home/n0p/src/c62/work/txbench`, from `master`
`a2521344`. **No radio, no `/dev/ttyUSB*`**: everything below is measured on the
host. **Symptom, on hardware**: in D-STAR, dPMR, NXDN, YSF and P25 the PTT
produces no emission and the radio does not reboot; DMR and FM do transmit.

---

## 1. The bench

`tests/host/tx_bench/` (target `tx_bench`, `meson test -C build_linux "TX Bench
Host"`). It compiles the five modes for real — `OpMode_DSTAR/NXDN/DPMR/YSF/P25`
with the `CONFIG_*` the image is built with and the protocol sources
`platform/targets/c62/CMakeLists.txt` adds per mode — against the emulator's own
audio and radio drivers, and drives them the way the radio does:

```
rtx_init() -> rtx_configure(mode, 430 MHz, 500 mW, src/dst ids) -> rtx_task()
  ... until the driver reports RX
PTT released (the gate has to see one) -> PTT pressed and held
  ... until the mode reports TX, 2 s at most
a few more passes so the samples the mode renders can be counted -> release
```

It prints, per mode, `TX` (and the halves/samples/peak that left the mode) or
the call that refused, by name and return code, read from `rtx/TxTrace.h` — the
same record the radio keeps. FM rides along as the control that transmits.

## 2. What it says, per mode

Before the fixes, on this host, with everything else working:

| mode | result | where the PTT died |
|---|---|---|
| FM | TX | — (control) |
| NXDN | did not enter receive | `OpMode_NXDN::getID()` answered `OPMODE_NONE` → `switchMode()` destroyed the mode one statement after constructing it; `startTx()` was never called |
| dPMR | did not enter receive | same, `OpMode_DPMR` |
| D-STAR | did not enter receive | same, `OpMode_DSTAR` |
| YSF | TX with the receiver alive / "the PTT never looked" with the receiver dead | the keying check sat at the end of the receive pass, behind three `return`s (path not open, no block, the settle window) |
| P25 | same, and the 500 ms settle window ate the first press | same |

So the answer to "which call of `startTx()` fails" is, for all five, **none**:
`startTx()` was never reached. Three modes were destroyed by the mode engine and
two never looked at the PTT because the receive path returned first. That is the
"a previous condition is false" branch of the question, and no amount of
instrumentation *inside* `startTx()` would have shown it.

## 3. The mutation that validates the bench

The bench is not allowed to be green for the wrong reason. Three injections, in
`platform/targets/linux/emulator/txbench.h`:

| injection | what it breaks | what the bench demands |
|---|---|---|
| `txbench_failStreamStart(-EIO)` | the codec refuses the playback stream | every mode that streams names `audioStream_start(OUT …)` |
| `txbench_refuseKeying(true)` | the PA does not come up | every mode, FM included, names `radio_enableTx(): the PA did not key` |
| `txbench_failReceiveStream(true)` | the receiver's stream never starts | every mode **still keys** |

The first two are the "can it tell red from green" check; the third is a
regression test for the YSF/P25 bug itself. The run before the fix, verbatim:

```
  YSF      PTT keys with the receiver dead                no: the PTT never looked
  P25      PTT keys with the receiver dead                no: the PTT never looked
  NXDN     radio_enableTx(): the PA did not key           (nothing)
  dPMR     radio_enableTx(): the PA did not key           (nothing)
```

and the two fixes, each reverted in the worktree and rebuilt, are caught again:

| fix reverted | bench output |
|---|---|
| `getID()` on the three modes | `NXDN did not enter receive`, `dPMR did not enter receive`, `D-STAR did not enter receive` — `(mode NXDN did not come up: published mode=0 status=0 driver=1)` |
| the YSF/P25 reorder | `YSF … no: the PTT never looked`, `P25 … no: the PTT never looked`, exit 1 |

## 4. The causes and the fixes

| commit | what it fixes |
|---|---|
| `9c0cf31b` rtx: NXDN, dPMR and D-STAR never told the engine who they were | `getID()` in the three mode headers. `switchMode()` compares the constructed mode's id with the requested one and destroys a mode that answers `OPMODE_NONE`: these three inherited that default, so selecting NXDN, dPMR or D-STAR destroyed the mode one statement after `new (block) OpMode_XXX()` — no receiver, no PTT, nothing in the log, in **any** build. |
| `0c4d29a1` ysf, p25: the PTT is looked at before the receiver, not after it | The keying check moves from the end of the receive pass to just after `startRx`/`newCfg`, as OpMode_DMR, OpMode_NXDN and OpMode_DPMR already do (and OpMode_DSTAR effectively does, because its RX case breaks instead of returning). A press can no longer be swallowed by a path that is not open, a missing block or the settle window, and a dead receiver no longer takes the transmitter with it. |
| `fd8bdcf3` nxdn, dpmr: a key-up the PA refused is not a transmission | Both modes called `radio_enableTx()` and returned `true` without ever asking `radio_getStatus()`: a refusal (the gate, the driver's barrier, 0 mW, no calibration) produced TX on the screen and a supervisor told an over had started, with the PA off. They now check, like DMR/YSF/P25/D-STAR. |

Supporting commits, no behaviour change on the radio:

| commit | what it adds |
|---|---|
| `d5f32ecf` rtx: a trace of the last startTx() | `openrtx/include/rtx/TxTrace.h`, `openrtx/src/rtx/TxTrace.c` — the record: mode, calls and their return codes, the first refusal, a self-checked precondition, "keyed". Portable C, ~20 B of .bss, written by the RTX thread only. |
| `aefc2126` rtx: the six modes report the steps of startTx() | the calls themselves, in D-STAR, NXDN, dPMR, YSF, P25 and FM. Same returns, same branches. |
| `1bdc463b` linux: the emulator gets the transmitter, the microphone and the receiver | `platform/drivers/audio/audio_linux_rtx.c` + `emulator/txbench.h`. |
| `7d802c8a` tests: a host bench for the five modes… | `tests/host/tx_bench/` and its meson target. |
| `docs/TX_SAFETY.md` | the new section: what to read when a PTT does nothing. |

## 5. Why nobody saw it before

`meson.build` compiles the five modes' *screens* and not the modes ("the mode
itself on the radio"), and the Linux platform declared three audio endpoints
with no driver behind them:

| endpoint | what the host had |
|---|---|
| `SINK_RTX` | `{ NULL, 0, 0, SINK_RTX }` — no driver, so `audioStream_start()` answered `-ENODEV` one call below anything a mode decides |
| `SOURCE_MIC` | `{ NULL, 0, 0, SOURCE_MIC }` — same |
| `SOURCE_RTX` | a capture file (`/tmp/baseband.raw`) that is normally not there, so the receive stream never started either |

That is the whole reason FM looked like it could transmit (it keys the PA with no
stream of its own) and every digital mode looked like it could not. Both halves
are now honest, and the three endpoints are what makes the bench able to see a
mode that keys and renders nothing (`peak == 0`) as the failure it is.

## 6. Suite

`meson test -C build_linux` — **115/115** (the 114 of `master` in a workspace
with `libtetra`, plus `TX Bench Host`). DMR's code is untouched; FM gained the
trace calls and nothing else.

```
$ ./build_linux/tx_bench
mode     result                                         keyed  blocks  peak
------------------------------------------------------------------------------
FM       TX                                             yes    0       0
NXDN     TX                                             yes    24      14161
dPMR     TX                                             yes    25      17438
D-STAR   TX                                             yes    25      3617
YSF      TX                                             yes    23      15339
P25      TX                                             yes    25      29147
...
PASS
```

## 7. What is not verified

- **The radio.** None of this was reproduced on hardware: no access, and the
  task forbids touching it. What the three fixes do on the air is the next step;
  the trace is now in the image, so the serial log of a PTT will say which call
  refused if one still does.
- **Which condition was true for YSF and P25 on the radio.** The bench proves
  the *shape* of the failure (any receive-side return swallows the PTT) and the
  fix removes the dependency; whether what the receiver was missing on the radio
  was the codec route, the baseband stream or the settle window is not something
  the host can decide.
- **DMR in the bench.** The DMR mode drags the whole DMR package (demodulator,
  vocoder, trunk, SMS, messaging) into the target; FM is the control and the
  three injections are what prove the bench can tell a working transmission from
  a broken one. Worth doing when the package's host links are cheap.
