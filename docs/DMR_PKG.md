# DMR: the protocol package, on our vocoders

The C62's DMR mode is not this tree's own chain any more. The protocol layer
(demodulator, FEC, burst and slot state machine, transmitter, data, SMS, LRRP,
privacy, Tier III) is the package of `protocols/DMR/` described in
`docs/DMR_PKG_UPSTREAM.md`: about 12 100 lines of C from another C62 tree,
applied here with one interface to port (the settings records) and one
substitution (the vocoders). `docs/DMR_RX.md` and `docs/DMR_TX.md` describe the
chain it replaced, kept for the measurements and the reasoning.

This document is the map: what is the package, what is ours, what is gone, what
it costs and what it does not do yet.

```
CONFIG_C62_DMR_RX=y  the mode, the receiver, voice, Tier III
CONFIG_DMR_TX=y      the transmitter
C62_DMR_VOICE=ON     voice from mbelib instead of lib/dmr_vocoder (off)
DMR_ENC=ON           the encoder from OpenDMR instead of lib/dmr_encoder (off)
```

## The file map

Every file is in `openrtx/{include,src}/protocols/DMR/`.

| File | What it does | Whose |
|---|---|---|
| `dmr_demod.c` | 4FSK off the discriminator: RRC, sync search, levels, per-burst timing, burst framer | package |
| `dmr_fec.c`, `dmr_trellis.c` | CACH, Golay, BPTC(196,96), QR, Reed-Solomon, rate-3/4 trellis | package |
| `dmr_proto.c` | bursts, link control, EMB, CSBK, the two-slot state machine, the Tier III feed | package |
| `dmr_voice.c` | voice frames per slot, listen filter, call bookkeeping, privacy, the playback queue and the vocoder | package, **backend ours** |
| `DmrSpeech.c` | playback thread: audio path, 60 ms double buffer, statistics | package, adapted (our audio API, Spanish log) |
| `dmr_tx.c` | voice LC headers, superframes, terminator, PI header, 4FSK modulator | package |
| `dmr_enc.c` | the AMBE+2 encoder hook and the canonical-to-air interleave | **ours** |
| `dmr_data.c`, `dmr_sms.c`, `dmr_lrrp.c` | data calls, texts in three formats, LRRP positions | package |
| `dmr_ep.c`, `dmr_aes.c` + `aes/`, `dmr_hytera.c` | Motorola, AES/DES and Hytera privacy, receive and transmit | package |
| `dmr_track.c` | stations heard, talkgroups, call log | package |
| `dmr_glue.c` + `dmr_glue.h` | `c62_dmr` and `c62_t3`: the settings records the package reads; air delay and its pre-roll; the SMS format mapping | **ours** |
| `dmr_port.h` | platform shim: locks, `EXT_RAM_BSS_ATTR`, `DMR_FAST_BSS`, cycle counter | **ours** |
| `DmrDefs.c`, `DmrStatus.c` | burst type names, what the screen shows | package |
| `trunk/` | Tier III: channel hunt, camping, grants, registration (RadioFoundry, MIT) | package |
| `../../rtx/OpMode_DMR.cpp` | the mode: channels, PTT, audio streams, the log | ours (ported from `OpMode_DMR_RX.cpp`) |
| `../../../../platform/targets/c62/psram_dmr.ld` | `.psram_dmr`, the NOLOAD output section | **ours** |

`OpMode_DMR_RX.cpp` stays in the tree as the package's own mode, the reference
for what the UI hooks do; it is not compiled (it calls a transmit encoder
service this tree does not have).

## What was replaced

Deleted with the old chain: `DmrPhy.c` (streaming demodulator), `DmrFec.c`,
`DmrTx.c`/`DmrTxTables.c` (transmitter), `DmrVocoder.c`/`DmrVocoderEncoder.c`
(the weak encoder hook), `DmrTxFsm.c`, `DmrSms.c`/`DmrSmsApp.c` and their
headers `DmrDefs.h`, `DmrFec.h`, `DmrPhy.h`, `DmrSms.h`, `DmrTx.h`,
`DmrTxFsm.h`, `DmrVocoder.h`. The package's own `DmrSpeech`, `DmrStatus` and
`DmrDefs` replace them under the same names where the mode and the UI expect
them.

The only edit inside the package's files is the settings hook: `dmr_trunk.c`
includes `dmr_glue.h` instead of the other tree's `ui/C62Extras.h`, and reads
the same field names. Everything else in `protocols/DMR/` is as packaged.

## Transmit air delay

`c62_dmr.txAirDelayUs` is how long a slot written into the output stream takes
to leave the PA. Its default, 40 000 us, is the part this firmware can compute:

```
30 000 us   half of the output buffer: 2 x 1440 samples at 48 kHz
            (BUF_CIRC_DOUBLE).  A slot written now plays when the other half
            ends, one 30 ms slot from now.
10 000 us   one DSP transport frame (csk6_codec.c, CAPTURE_FRAME_MS = 10):
            the half is handed to the DSP at the next 480-sample boundary,
            0..10 ms.
---------
40 000 us   deterministic, one transport frame of uncertainty (-0 / +10 ms)
```

Not in that figure: the DSP's own output queue and the DAC, which this driver
cannot read back. The other tree's loopback measured 12.4 ms for the same
worker and image family, so the real number is expected around 50 ms: that is
what the persistent trim is for (`dmrGlue_setAirDelayUs()`, 0..60 000 us,
stored by hand in the memory). Through a repeater the trim also picks the slot:
`dmrGlue_preRollSamples()` emits the delay modulo one TDMA frame (2880 samples
at 48 kHz) of silence before the first burst, so the bursts land where the
repeater expects them instead of a fixed number of milliseconds late. Simplex
does not use the pre-roll. Re-measure the trim whenever the DSP image changes.

## Sizes

`west build -b c62` on this tree, `-Os` as the firmware uses, with
`-DCONFIG_C62_DMR_RX=y` and/or `-DCONFIG_DMR_TX=y`:

| build | FLASH | RAM | DTCM | PSRAMAP |
|---|---|---|---|---|
| no DMR | 464 100 B (44.26 %) | 174 288 B (66.49 %) | 0 | 379 064 B (20.66 %) |
| DMR RX | 551 080 B (52.56 %) | 186 928 B (71.31 %) | 12 640 B (77.15 %) | 427 888 B (23.32 %) |
| DMR RX + TX, our vocoders | **612 312 B (58.39 %)** | **200 000 B (76.29 %)** | **12 640 B (77.15 %)** | **427 888 B (23.32 %)** |
| DMR RX + TX, mbelib + OpenDMR | 670 384 B (63.93 %) | 194 240 B (74.10 %) | 11 688 B (71.34 %) | 451 552 B (24.61 %) |

The last row is the package as it came (`-DC62_DMR_VOICE=ON -DDMR_ENC=ON`). Our
vocoders save **58 072 B of flash** and cost **5 760 B of RAM** in the transmit
build: the encoder's analysis state (5.5 KB of `.bss`, touched every frame) has
no PSRAM home, while OpenDMR's tables were placed in `.psram_dmr` and mbelib's
decoder state came to 3.5 KB of DTCM. DTCM grows by 952 B (8 540 B of decoder
state and a 4 KB speech stack instead of mbelib's 3 496 B and 8 KB). All four
fit; the TX build leaves 62 KB of SRAM and 3.7 KB of DTCM.

RAM is measured by the image's own link report (`Memory region ... Used
Size`), not by a formula: the figures move with `git describe` and with the
Kconfig set.  Since the vocoder scratch moved out of DTCM (`docs/DMR_VOCODER.md`,
"Where the state lives"), the DMR rows gain 4 052 B in PSRAM and lose 4 048 B of
DTCM; the totals above predate that split.

## The vocoders

The package's pair is mbelib for the decoder and OpenDMR's C++ encoder. Neither
fits the M33, measured on QEMU `mps2-an505` with the TCG instruction counter
(300 MHz, CPI 1.2-1.6, the same convention as `docs/DMR_TX.md`):

| vocoder | instructions per 20 ms frame | time per frame | share of one core |
|---|---|---|---|
| OpenDMR encoder (as the firmware links it) | 4 097 626 | 16.4 - 21.9 ms | 82 - 109 % |
| mbelib decoder | 9 360 000 (median) | 37 - 50 ms | 187 - 250 % |
| **`lib/dmr_encoder` (ours)** | **1 423 868** | **5.7 - 7.6 ms** | **28 - 38 %** |
| **`lib/dmr_vocoder` (ours)** | **113 402** | **0.45 - 0.60 ms** | **2.3 - 3.0 %** |

So the substitution is not a preference: with the package's pair a single
20 ms frame of the decoder is already twice the budget, and the encoder plus
the transmitter overruns it. Ours are integer (encoder) and float-single
(decoder), bit-exact with their references, and were already in the tree —
this branch had taken them out of the DMR path when the package went in.

What the backend switch does (`dmr_voice.c`):

- **Clear voice** (DMR, NXDN): the 36 dibits are de-interleaved into mbelib's
  four planes, packed as the canonical 9-byte frame and decoded with
  `dmr_vocoder_decode_frame()`. One frame costs 113 k instructions.
- **Privacy** (Motorola Basic/Enhanced, Hytera Basic/Enhanced, Anytone, TYT,
  AES/DES): `dmr_vocoder_unpack()` runs the Golay FEC and hands back the 49
  data bits, the keystream goes on them as before, and
  `dmr_vocoder_decode_data()` synthesises. The two entry points are the only
  thing added to `lib/dmr_vocoder` for this; they are mbelib's
  `mbe_eccAmbe3600x2450*` and `mbe_processAmbe2450Dataf()` split in two.
- **System Fusion** (49 data bits, FEC already removed) goes straight to
  `dmr_vocoder_decode_data()`, the same 3600x2450 model.
- **D-STAR (3600x2400) and P25 IMBE (7200x4400)** are not in our decoder:
  with our backend those frames are played as silence. mbelib is the only
  backend that decodes them, and it is one build option away
  (`-DC62_DMR_VOICE=ON`). Nothing in this tree feeds them today (the DMR mode
  is the only caller), so the default build loses nothing.

The encoder hook (`dmr_enc.c`) is the same shape: `lib/dmr_encoder` by
default, `opendmr_encode()` with `-DDMR_ENC=ON`, and one thing for both.

A note on levels: on the same capture our decoder is about 1.9x mbelib's RMS
(it is calibrated against the OpenDMR decoder, 8x gain and a soft clip at 95 %,
not against mbelib's 1/4096 scale), so `dmr_voice.c` does not apply mbelib's
`OUT_SCALE`. Peaks stay under full scale (max 0.84 FS on the sigidwiki capture,
0.016 % of samples over the 0.7 limiter knee, none clipped), and the decoded
frames correlate 0.92 with mbelib's on the same 756 frames.

### The interleave both encoders were missing

Both libraries hand out the 72 bits in the canonical (DVSI) order: A, the
Golay(24,12) word, then B, the scrambled Golay(23,12), then the 25 unprotected
bits. `dmr_tx_voice()` does not take that: it writes the 9 bytes into the
burst's 36 dibits as they go out, which is ETSI TS 102 361-1's four-block
interleave of the same 72 bits (canonical bit `c` at air bit
`4*(c mod 18) + c/18`). The receiver undoes it with the DSD schedule in
`dmr_voice.c`. Before this adapter, the OpenDMR wrapper handed the canonical
frames straight to `dmr_tx_voice()`: over 1000 frames of speech the loop
encoder -> burst -> the receiver's frame slices -> de-interleave gives back
**0 differing frames** with the interleave and **999 of 1000** without it.
`dmr_enc.c` now interleaves once, for either backend.

## `.psram_dmr`

The package keeps its large, rarely touched state out of the 256 KB of SRAM:
the demodulator's sample ring, the call log, the voice burst queue, the slot
state machine, the data reassembly buffers and the Tier III engine
(`EXT_RAM_BSS_ATTR`, `dmr_port.h`). Together they are 48.8 KB, which SRAM does
not have.

They live in an output section of their own, `platform/targets/c62/psram_dmr.ld`,
`NOLOAD`, inside `PSRAMAP`. Not in `.psram_section`, which already exists for
the CSK6: that region also holds the CSK6 system heap, so it cannot be zeroed
wholesale. As a section with exact bounds, `platform_init_csk6()` memsets it
once at boot (`psram_dmr_clear()`), so it reads as zeros before any module's
reset; every module still rearms its own state in its reset function.

The risks, in the order they can bite:

- **Speed.** PSRAM is slower than SRAM. Nothing in the section is touched
  inside a sample or symbol loop — once per burst, or once per 20 ms frame —
  and the voice burst queue only receives and hands over pointers. The
  vocoder state is *not* here for that reason: it is in DTCM (`DMR_FAST_BSS`),
  as mbelib's was, because with it in PSRAM the vocoder was barely real time.
- **No initialisers.** `NOLOAD` means neither the loader nor the C runtime
  writes it: anything with a non-zero initialiser must not land there, and a
  boot that skips `platform_init_csk6()` sees whatever the PSRAM controller
  left. The boot clear is the contract; keep it when the init sequence changes.
- **Growth is invisible to the RAM figure.** PSRAMAP is reported separately
  (23-25 % today); a package update that adds a big buffer costs PSRAM, not
  the 76 % of SRAM above, and it will not show up where one looks first.
- **OpenDMR's tables when it is selected.** Its decoder's DCT cosine cache
  and three seeds are marked `MBE_THREAD_LOCAL`, which
  `scripts/c62/fetch_opendmr.sh` makes overridable; with `-DDMR_ENC=ON` they
  are placed in `.psram_dmr` too (they are built on the first decode and only
  read afterwards). The default build does not link OpenDMR at all.

## Limits

- **Tier III against the captured system does not camp.** The CSBKs of the
  capture on hand carry **FID 0x06** (the manufacturer PDU set), not the ETSI
  set the engine parses; only one `C_ALOHA` in it is ETSI (FID 0). With a
  synthetic ETSI Aloha the engine camps: state 2, syscode 0x1236, TSCC
  438 MHz, colour code 1. So the engine works and the capture is the wrong
  dialect. **There is no capture of a real ETSI Aloha on this system**: the
  Tier III path is verified against a synthesised PDU and the standard, not
  against the air. Vendor CSBKs are a separate decoder, not written.
- **Voice is AMBE+2 3600x2450 only** (DMR, NXDN, YSF). D-STAR and P25 IMBE
  need the mbelib build. See "The vocoders".
- **The transmit voice order is verified in the loop, not over the air.** The
  1000-frame loop above proves encoder, interleave, burst and receiver agree;
  the first on-air check of the transmit path with the interleave is still to
  be done (the receiver side of the same code decodes real captures).
- **Air delay**: the 40 ms default is the computed part; the trim that brings
  it to the ~50 ms the DSP adds is a bench figure per DSP image.
- `meson test` covers what does not need the radio: 32 tests (DMR glue, the LC
  header, SMS formats, settings, M17, UI). The vocoders and the demodulator
  have their own host and QEMU harnesses, below.

## The `lib/*` directories

- `lib/dmr_vocoder` — the decoder, GPL-2.0+/GPL-3.0+ (see
  `docs/DMR_VOCODER.md`). Was out of the DMR path while the package's mbelib
  was in; it is the default backend again. State 4 492 B in DTCM plus a
  4 052 B per-frame scratch in PSRAM (it was 8 540 B, all in DTCM; see
  `docs/DMR_VOCODER.md`, "Where the state lives").
- `lib/dmr_encoder` — the encoder, GPL-3.0+ (see `docs/DMR_TX.md`). Same
  history; the default backend for `DMR_TX`. State 5.5 KB of `.bss`.
- `lib/opendmr` (submodule, `n0p/OpenDMR`) — no longer built by default. Kept
  as the reference the two libraries are checked against and as the
  `-DDMR_ENC=ON` backend. Its decoder half is not linked: the receiver uses
  `lib/dmr_vocoder`, and `--gc-sections` drops it.
- mbelib is not a directory of this tree: `scripts/c62/fetch_mbelib.sh` puts
  it in `<west>/modules/lib/mbelib` and only `-DC62_DMR_VOICE=ON` builds it.

## Reproducing the numbers

```
# the receive chain on a real capture, ours vs the package's mbelib
tests/host/dmr_pkg_host/build.sh own            # build_host/dmr_pkg_host
tests/host/dmr_pkg_host/build.sh <mbelib dir>   # a second binary, same input
build_host/dmr_pkg_host capture.wav -o out.wav  # 756 frames of sigidwiki's

# decoder cost, from the objects the firmware builds
FIRMWARE_BUILD=build_dmrpkg lib/dmr_vocoder/tests/qemu/run_qemu.sh

# encoder alone, and the whole transmit path per 20 ms frame
LIB=lib/dmr_encoder OPT=O2 work/dmr-tx/enc-c/measure.sh
tests/host/dmr_enc_qemu/run_own.sh speech8k.pcm 60

# the whole receiver per second of 48 kHz discriminator audio
tests/host/dmr_pkg_qemu/run_rx.sh capture.wav 15
```

The receiver, everything included (demodulator, FEC, protocol, Tier III,
vocoder), is **37.1 M instructions per second of audio: 14.8 % of a 300 MHz
M33 at CPI 1.2, 19.8 % at CPI 1.6**, of which the vocoder is 5.1 M (2.0 %).
With mbelib the same stream would need 455 M instructions/s (182 %): the
capture decodes and plays, it does not decode in real time.

## Licences

The package carries its own: `trunk/LICENSE.radiofoundry` (MIT) and
`aes/LICENSE.dmrgo` + `aes/UNLICENSE.tinyaes` stay with their files. The glue
(`dmr_glue.*`, `dmr_enc.*`, `dmr_port.h`, `psram_dmr.ld`) is
GPL-3.0-or-later, as the rest of the tree. `lib/dmr_vocoder` mixes
GPL-3.0-or-later (its API) with GPL-2.0-or-later (the decoder core);
`lib/dmr_encoder` is GPL-3.0-or-later with GPL-2.0-or-later tables. mbelib and
OpenDMR are fetched, not shipped.
