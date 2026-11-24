# Transmit hang: M17 and DMR key the PA and never come back

**Branch** `c62/tx-hang`, from `master` `1958c7c3` — the build that was flashed
when this was reported.
**Symptom, on hardware**: in M17 and in DMR, pressing the PTT produces no
emission and the radio ends up hung — the PA stays keyed and nothing brings it
back except a power cycle.
**Not** the previous bug: the stream buffer overflow and the corrupted `txStage`
(which is why the rtx thread went to 9 KiB) are already fixed in `1958c7c3`, and
this happens with those fixes in.

---

## 1. Diagnosis

Three things have to be true at once for a keyed, silent, hung radio, and all
three were:

1. **Something in the transmit path blocks with no bound.** The M17 and DMR
   transmitters are the only ones that wait on a *buffer boundary* of an
   application stream: `outputStream_sync()` in `Modulator::sendBaseband()` and
   in `OpMode_DMR::txSlot()`, and `inputStream_getData()` in the M17 encoder
   thread. FM never waits on one, which is why FM was not reported broken.
2. **The wait can be entered while the lock that would end it is held.** In
   `platform/drivers/audio/csk6_codec.c` the worker ran

   ```c
   k_mutex_lock(&audioMutex, K_FOREVER);
   applyRoutes(...);   /* AudioRecord_start() / AudioTrack_start(): DSP calls */
   ```

   `AudioTrack_start()` (`AudioTrack.c`) changes the thread priority and calls
   into the DSP core through `IAudioTrack_start()`; `AudioRecord_start()` does
   the same on the capture side. While one of those is in progress the worker
   holds `audioMutex`, and **every** audio caller waits on it. The RTX thread is
   one of them: entering transmit it calls `audioPath_request()` →
   `audio_connect()` → `csk6codec_setRoute()`, which waits (`K_FOREVER`) for the
   worker to apply the route. So a DSP call that does not return leaves the RTX
   thread blocked, holding `boardMutex`, with the PA keyed, and the radio dead:
   the UI thread also runs `ui_updateFSM()` under `state_mutex` and waits for
   audio in the voice-prompt path, so the whole radio looks hung.
3. **Nothing unkeyed it.** The thread that owns the PTT state is the thread that
   is blocked, there was no deadline anywhere, and the AP watchdog was
   **disabled** (`&ap_wdt { status = "disabled"; }` in `c62.dts`, no
   `CONFIG_WATCHDOG`). So the "hang" was permanent by construction.

Two secondary unbounded waits in the same path, both reachable from the
transmit entry/exit and both capable of hanging the RTX thread on their own:

| Where | Wait | Reachable from |
|---|---|---|
| `audio_codec.c` `stopThread()` | `pthread_join()` — no timeout in this kernel | M17 `offState()` / `disable()`, i.e. every exit from transmit |
| `csk6_codec.c` `csk6codec_terminate()` | `k_thread_join(K_FOREVER)` | power off |

Note what this does **not** claim: without the radio in hand, which of the two
candidates (#2, the DSP call behind the audio lock, or simply a playback stream
that never starts) fires first cannot be told from the source alone. Both are
the same class of failure and both are now bounded and reported — see §5 for the
trace that distinguishes them on the radio (`wa=` and `pk=`).

## 2. What the fix is

**One lifecycle for every transmission**, `openrtx/include/rtx/TxGuard.h` +
`openrtx/src/rtx/TxGuard.c`: phases `starting → wait_first → running →
draining`, each with a budget (250 / 300 / 160 ms, plus a 300 s absolute cap),
written from the modes and checked from **another thread**. It is a pure
function of the time passed in, so `tests/unit/tx_guard.cpp` walks every state
and every way out.

**A supervisor that can unkey what the owner thread cannot.** `rtx_supervise()`
runs in the main thread every 5 ms. At the first overrun it calls
`radio_disableRtx()` (PA off first) and then `currMode->abortTx()`, which only
cancels the transmit streams with `audioStream_terminate()` — and cancelling a
stream is exactly what releases a thread blocked in `outputStream_sync()`. It
repeats every 20 ms until the mode leaves transmit, and the mode sets
`txBlocked` so a still-pressed PTT cannot re-key it in a loop. The audio paths
and the modulator are still released by the mode's own exit path, from the
thread that owns them.

**The audio lock is no longer held across DSP calls.** The worker computes the
desired route state under `audioMutex`, releases it, and only then starts or
stops the DSP streams. A DSP call that never returns now costs the audio instead
of the radio. The remaining waits got bounds: route apply 250 ms, codec thread
stop 300 ms, codec worker join 500 ms, and the stream table got a mutex so a
cancel from another thread is well defined.

**The watchdog.** `&ap_wdt` is enabled, `CONFIG_WATCHDOG=y`, armed for 2 s at the
end of `platform_init_csk6()` after the PA enable lines are driven low, and
refreshed only by the main thread. If the supervisor itself cannot unwind the
firmware, the reset leaves the PA off.

**The abort reason survives the exit**, so the trace says why the over was cut
(`to-start`, `to-first`, `to-drain`, `to-max`) and not why the mode then left.

## 3. Instrumentation: where it stops

The transmit lifecycle is now visible in two places, no analyser required.

Per over, in the log (one line, printed on the way out so it does not sit in the
keying path):

```
TX exit 0:enter 8:open 31:first 61:first 91:first ... 4008:finish 4094:exit |
        TX n=3 ab=0 ph=idle rs=ptt blk=133 slip=0 lat=31 tail=86 | pk=2731
TX ABORT to-first: 0:enter 8:open 312:abort | TX n=4 ab=1 ph=abort rs=to-first ...
TX ABORT to-drain: 0:enter 6:open 29:first ... 5001:finish 5170:abort | ...
```

* the trace is `ms-from-the-PTT:event` for `enter open openfail first finish
  exit abort keyed enc late`;
* `rs=` is the reason, `blk=` the blocks handed to the sink, `slip=` the blocks
  that arrived later than two slots (cadence), `lat=` the PTT-to-first-block
  time of the last over, `tail=` the release-to-receive time;
* `pk=` is the peak of the baseband the codec actually played to the BK4819
  (peak hold 100 ms). **`pk=0` on a keyed over means the mode is handing the
  transmitter silence** — that is the "does not emit" case, and it is a
  different failure from "the stream never started".

Over the remote control (`-DC62_REMOTE=ON -DC62_BASEBAND_DUMP=ON`, see
`scripts/c62_remote.py`), live during an over:

```
tx  ->  t= 0:enter 8:open 90:first ... | TX n=1 ab=0 ph=run rs=none blk=7
        slip=0 lat=90 tail=0 | pk=2731 wa=2
```

`wa=` is the age of the audio worker in milliseconds. A large `wa` **is** the
wedge: the worker is inside a DSP call and no stream is being served.
`ptt 0|1` forces the PTT (the C62_REMOTE build has no PTT button), so a whole
over can be driven and measured from the host, and `s` now carries `txp=`.

## 4. Verification on the host

Everything below is real output, not a description.

**The hang, reproduced.** `tests/host/tx_lifecycle_host/` builds the real
`audio_stream.c` and the real `TxGuard.c` against a stub sink whose `sync()`
never returns — a consumer that does not start:

```
scenario                     numbers (ms, measured from the PTT)
A stalled, no supervisor     RTX thread never returned after 3 s
  ok: reproduces the hang -- the PA is still keyed
B stalled, supervised        first=0  tail=0  keyed=304  ret=0  keyednow=no reason=to-first
  ok: PA off 304 ms after the PTT (budget 300), loop back 0 ms later, reason to-first
C healthy, 1.5 s of slots    first=30 tail=0  keyed=1504 ret=0  keyednow=no reason=ptt
  ok: first block 30 ms (budget 300), out of TX 0 ms after the release (budget 200)

all scenarios behaved as expected
```

Scenario A is the bug (a stalled sink and no supervisor leaves the PA keyed for
ever), B is the fix (the PA is off 304 ms after the PTT, inside the 300 ms
first-block budget plus one 5 ms tick, and the blocked caller returns
immediately), C is the healthy path with the two numbers the radio has to meet.
`run.sh` exits non-zero if any of them drifts.

**The lifecycle itself**, `tests/unit/tx_guard.cpp`, 9 cases / 53 assertions:
a healthy over (first block 40 ms, back to receive 70 ms after the release), a
sink that never consumes (aborts at `TXG_FIRST_MS` + one tick, reason `to-first`,
request repeated until the exit, reason preserved through it), streams that never
open (`to-start`), a tail that never ends (out of transmit ≤ 200 ms after the
release, `to-drain`), a PTT held for five minutes (`to-max`), late blocks counted
as cadence slips, and the trace format.

**DMR cadence and the encoder**, `tests/unit/dmr_tx_fsm.cpp` (new case): ten
seconds of held PTT with the microphone at 50 frames/s and the slot rules of
`txSlot()`. Result: 166 bursts of 3 frames, one every other slot, 498 frames on
the air for 499 produced — 1× speed, no burst doubled, no frame repeated or
skipped, queue ≤ 12, at most one starved burst, first voice burst at slot 2
(60 ms after the PTT, after the LC header that announces the call).

Full suite: `meson test -C build_linux` → 26/26 OK.

**Both layouts, C62, `-DCONFIG_C62_APRS=y`:**

| layout | flash | RAM |
|---|---|---|
| `-DCONFIG_C62_DMR_RX=y` | 513 516 B (48.97 %) | 195 936 B (74.74 %) |
| `+ -DCONFIG_DMR_TX=y` | 574 424 B (54.78 %) | 208 888 B (79.68 %) |

The receive-only layout grew 5 392 B of flash and 264 B of RAM over `1958c7c3`
(`508 124` / `195 672`): the guard, the supervisor, the watchdog driver and the
trace.

**Stack, `-fstack-usage`, same build.** Deepest RTX-thread chain (one DMR voice
slot): `update` 32 + `txSlot` 48 + `dmrTx_slot` 680 + `dmr_encoder_encode` 16 +
`dmr_enc_frame` 112 + `enc_ambe` 2288 + `dmr_enc_analysis` 232 + `pitch_est`
2112 = **5 520 B** (7 216 B if `e_p` 1 696 nests under it) of the 9 216 B stack
— 2 to 3.7 KiB of margin. Codec worker: `audioWorker` 136 + `playChannel` 56 of
2 048 B. M17 encoder thread: `encodeFunc` 96 + `codec2_encode_3200` 1 464 of
16 384 B. Nothing overflows, and `CONFIG_HW_STACK_PROTECTION` stays on as the
backstop.

**Heap.** The kernel heap is 40 960 B; the M17 transmit path allocates 9 216
(rtx stack) + 2 048 (UI) + 16 384 (codec2 thread) + 7 680 (modulator buffer, 2 ×
1 920 samples of 16 bit) + 640 (encoder ADC buffer) = **35 968 B, 87.8 %**, and
DMR needs 9 216 + 2 048 = 11 264 B of it. The change adds no allocation. The one case that
does lose the 16 KiB is a codec thread that does not come back within 300 ms: it
is left detached and its stack is not recovered — a bounded price, and the mode
then refuses to key (reason `start`) instead of hanging.

## 5. What to test on the radio

Flash both images; the interesting one is
`-DCONFIG_C62_DMR_RX=y -DCONFIG_DMR_TX=y -DCONFIG_C62_APRS=y`.

1. **Boot**: the log must say `C62: watchdog armed, 2 s`. If it says
   `watchdog unavailable`, the WDT node or `CONFIG_WATCHDOG` did not make it into
   the image and the last line of defence is missing.
2. **M17, PTT 2 s**: expect `TX exit ... rs=ptt ... pk=` non-zero and
   `lat=` (PTT to first block) under 300 ms. The preamble is 160 ms of it, so a
   number around 170–200 ms is normal. A `pk=0` here means the modulator is
   feeding silence — a different bug from a hang.
3. **DMR, PTT 2 s**: same, and `blk=` must be about 66 for 2 s (one slot per
   30 ms). `slip=` must be 0; a run of slips is the slot clock being missed.
4. **Release the PTT and watch the red LED**: it must go out and receive must
   come back inside 200 ms (`tail=` < 200 in the trace). The terminator plus tail
   slot is 60 ms of air, so 80–120 ms is expected.
5. **The hang scenarios, on purpose**: run with the remote control build, and
   with the radio in DMR, send `ptt 1` and then poll `tx` every 200 ms. Expect
   `ph=run` with growing `blk=`; if it goes to `ph=abort`/`rs=to-first` with a
   large `wa=`, the codec worker is the one wedging and the trace now says so.
   `ptt 0` must always end the over.
6. **The watchdog**: with the remote build, hold `ptt 1` in a mode that cannot
   key (DMR with `CONFIG_DMR_TX_ENC=n`), then a second `ptt 1` on top of an over
   that was already cut — the radio must never stay keyed for more than ~2.5 s
   without a reset, and a reset must come up in receive.
7. **M17 and the codec thread**: start and stop several overs back to back. Each
   must log a `TX exit` and the codec must restart (`codec_startEncode` returns
   true). If a `start` reason appears after a few overs, the encoder thread was
   left behind and its 16 KiB were not recovered — report it, the trace will show
   it.

## 6. What is still open

* **The root cause is narrowed, not proven.** The lock-across-DSP-calls is a real
  defect and the most likely one, but a playback stream that simply never starts
  on this DSP would look the same. Both are fixed or bounded; the trace
  distinguishes them on the radio (`wa=` large → the DSP call; `wa=` small with
  `rs=to-first` → the DSP started but is not consuming).
* **`pk=` is a peak-hold, not a level meter**: it answers "is anything going
  out", not "is the deviation right". The deviation is still uncalibrated
  (`REG_40 = 0x476`), unchanged by this work.
* **The bounded waits are a floor, not a cure for a wedged DSP.** If
  `AudioTrack_start()` genuinely never returns, that stream is lost until the
  next reset; the radio keeps receiving, but audio that needs the playback path
  (FM transmit passthrough, prompts) will not work. The watchdog will not fire,
  because the main thread is fine. That is deliberate: audio-only failure is
  better than a keyed PA.
* **Miosix/ARM targets are not buildable here** (toolchain), so the guard is
  compiled and tested on the Linux target and the C62 firmware only. It has no
  OS dependency and no platform-specific code.
* **A stale `build_c62` can fail at the Kconfig step** (`CONFIG_DMR_SMS` and
  `CONFIG_MESSAGING` are app CMake options that end up in
  `extra_kconfig_options.conf` on the second configure pass, and this Zephyr
  aborts on undefined Kconfig symbols). Always build the C62 image from a fresh
  build directory.
