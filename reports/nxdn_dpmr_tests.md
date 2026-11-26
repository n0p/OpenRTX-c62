# What NXDN and dPMR prove off the radio

Branch `t/nxdn`, base `4b8af88d`. Scope: the two narrow 4FSK modes at
2400 Bd, the rows NXDN and dPMR of `docs/TEST_PLAN.md` §1. Everything below runs
on the host, from `meson test -C build_linux --no-rebuild`; nothing here needs
the radio, an attenuator or a signal generator.

The four files of `tests/unit/` are the framework: no second one was started.
New cases are tagged `[timing]`, `[ran]`, `[fec]`, `[layout]`, `[status]` so a
single group can be run on its own, e.g. `./build_linux/nxdn_tx_test "[ran]"`.

## 1. Covered

| what | where | how it is shown to be broken when it breaks |
|---|---|---|
| **Symbol and timing recovery** | `nxdn_tx.cpp` `[timing]`, `dpmr_tx.cpp` `[timing]` | The call is built once as one continuous stream and fed through a resampler: first sample inside a symbol (0·2, 2·7, 7·35, 13·9, 19·5 samples into one), and a clock up to ±500 ppm off. NXDN decodes bit for bit at every point; dPMR bit for bit inside ±100 ppm and keeps the CCH CRC closing past that. NXDN also re-acquires the grid after a lost block (`nxdnPhy_reset()`, as the mode does on a failed read). Narrowing the timing search to two phases (NXDN) or one phase (dPMR) fails these and nothing else in the loopbacks. |
| **FEC: the modes' own block codes** | `nxdn_tx.cpp`, `nxdn_rx.cpp`, `dpmr_tx.cpp`, `dpmr_rx.cpp` `[fec]` | LICH parity over the four MSBs (a flipped data bit and the parity bit itself are refused); SACCH (CRC-6, punctured K=5) corrects three scattered errors and refuses half a block of noise and 494/500 random blocks; VCH has no code at all and one flipped dibit arrives as exactly one changed bit; dPMR Hamming (12,8) corrects one bit per block, two in one block are refused; CCH CRC-7 and header CRC-8 refuse 500/500 random blocks. |
| **RAN acceptance and rejection (NXDN)** | `nxdn_tx.cpp` `[ran]` | The policy was extracted from `OpMode_NXDN::onFrame()` into `nxdnRanFrame()`/`nxdnRanFollows()`/`nxdnRanTransmit()` in `NxdnDefs.c`, so the table is testable: everything passes before the first SACCH; a match is kept; a mismatch is dropped; a lost SACCH block is judged by the last RAN that decoded; 0, 62 and 63 are real RANs and only above 63 is "any"; a transmitter set to "any" sends 63. The second half is on the air: a stream built with RAN 21 reports 21 on every superframe and is dropped by a filter set to 22. |
| **CCH acceptance and rejection (dPMR)** | `dpmr_tx.cpp`, `dpmr_rx.cpp` | Every frame number, mode, version, format and the 18-bit SLD close through `dpmrFec_cchDecode()`; the CCH of a real burst is read at its offset; two bits in one Hamming block are refused. |
| **SACCH call fields: source > destination, type** | `nxdn_tx.cpp` `[layout]`, `[status]` | VCALL, TX_REL and DISC are assembled from the four 18-bit segments of the superframe and parsed back to type, source and destination (0x0123 > 0x4567, so a swap fails); the UI string comes from `nxdnFecMsgName()`. dPMR's equivalent is the header: called and own addresses (24 bits), header type and call information, read from the burst at their offsets and through the loopback. |
| **dPMR channel permutation, 18j+k → 4k+j** | `dpmr_rx.cpp` | Exhaustive, both directions: each of the 72 canonical bits through `dpmrFec_voiceFrameEncode()` has to leave at transmitted bit `4k+j`, and each transmitted bit through `dpmrFec_voiceFrame()` has to come back at `18j+k`. A one-index error in either map is caught. |
| **TX burst generation** | `nxdn_tx.cpp` `[layout]`, `dpmr_tx.cpp` `[layout]` | The symbols are read back field by field, not through the demodulator (which would agree with a transmitter that was wrong the same way): NXDN FSW/LICH/SACCH at 8/VCHs at 38, descrambled with the mode's mask; dPMR preamble pattern, P|FS1|HI0|CC|HI1, the FS2/channel-code alternation, CCH at 12, TCH at 48, END FS3 plus its two copies. Both loopbacks already proved the chain end to end, at ±1050 Hz and ±350 Hz of deviation. |
| **The state each mode publishes** | `nxdn_tx.cpp` `[status]`, `dpmr_tx.cpp` `[status]` | NXDN: a synthetic stream through the real demodulator into the real `NxdnStatus`, then the struct `ui_mode_state.c` reads — sync, RAN 42, rfType/usc/steal, EVM, frames/sacchOk/voiceFrames/ambeFrames, message {valid, VCALL, 0x0123, 0x4567}, `messages`; then what `nxdnStatus_reset()` (sync, RAN and the half-assembled superframe go, counters stay) and `nxdnStatus_clear()` (everything) leave. dPMR: the one flag it publishes, `dpmrPhy_synced()` behind `rtx_rxSquelchOpen()`, opens on a burst and closes 400 ms of silence later. |

## 2. Fixed on the way

1. **dPMR: an uncorrectable Hamming block failed the CRC, not the field.**
   `dpmrFec_hammingDecode()` writes nothing when a block cannot be corrected, so
   `hammingDecodeBlocks()` left those eight bytes as they were in the caller's
   stack — uninitialised in the CCH, HI and END decoders. The CRC was then the
   only guard, and the stale bytes can close it (and reading them is undefined
   behaviour). The block is now zeroed and the failure passed back, so
   `crcOk` is false on its own merits. The new header-rejection case fails
   without this fix and passes with it; the real 7 s capture decodes to the same
   numbers (42 frames, 39 CCH).
2. **NXDN: the RAN filter and the SACCH superframe assembly moved out of the
   mode**, into `NxdnDefs.c` and `NxdnStatus.c`. They were two expressions and
   twenty lines inside `OpMode_NXDN::onFrame()`, which the host build does not
   instantiate (`CONFIG_NXDN`), so nothing could reach them. The mode is now
   three calls over tested code. Behaviour is unchanged; the part-to-slot
   mapping, the publishing point and the reset semantics are now pinned by tests.
3. `docs/TEST_PLAN.md` §1 rows updated, and the paragraph on the missing
   receiver double records what now exists.

## 3. Not coverable without the radio, and why

- **Voice from the air to the speaker.** `nxdnSpeech_push()`/`dpmrSpeech_push()`
  and the AMBE+2 playout queue need the audio streams and the codec worker; the
  host build has no `OpMode_*` instance at all. What can be shown off the radio
  is what is shown: the four AMBE+2 frames of every frame, bit for bit, at the
  vocoder's input (loopback and layout tests).
- **`OpMode_*::update()` itself**: the PTT/keying path, the settle window after a
  retune, the `sleepFor()` pacing and the LED. `OpMode_NXDN.cpp` and
  `OpMode_DPMR.cpp` are compile-checked by hand against the same sources
  (`g++ -fsyntax-only` with the firmware's defines) but not executed; putting
  them in the host build would drag the vocoder, the audio driver and the RTX
  thread into every test, which `meson.build` deliberately avoids.
- **dPMR publishes no call state at all.** The mode decodes the header's
  addresses, the CCH's mode and address slices, the channel code and the END
  status, and keeps them in `OpMode_DPMR` accessors that nothing reads: the
  main screen shows `dPMR RX`/`SRCH` only (`reports/mode_status.md`, and
  `ui_mode_state.c` says so in a comment). Making that provable means giving
  dPMR a status module like `NxdnStatus` — a product change, not a test.
  **Closed later**: `DpmrStatus.c` publishes it and `unit/dpmr_status` asserts
  both the struct and the texts the main screen draws (branch `t/dpmrstatus`).
- **RF behaviour**: deviation, occupied bandwidth, sensitivity, BER against a
  real signal, and the transmitter's spectral mask after the PA. `docs/TX_NXDN.md`
  and `docs/RX_DPMR.md` are where those numbers live; they need the bench.
  (What the shipped captures give is already asserted in the suite: NXDN's
  18-frame asset in `nxdn_rx.cpp`, and dPMR's 7 s asset with floors on frames,
  CCH, voice, header and both addresses in `dPMR Host Harness`. The 30 s DSDcc
  capture behind `DPMR_CAPTURE` is the optional deeper check and stays out of
  the repository.)
- **A real second clock**: the timing tests resample in software (exact, and
  repeatable). Two crystals a few ppm apart, and the audio path's own drift, are
  a bench measurement.
