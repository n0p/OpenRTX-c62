# C62 DMR: protocol layer, as it stands

This is the DMR code of the `c62/tetra`-derived tree, packaged on its own so it
can be applied to another branch. It is the protocol layer plus the operating
mode; everything that is specific to one radio's UI, non-volatile memory or
baseband driver is **not** here, because that part is yours.

## What is in the package

```
openrtx/include/protocols/DMR/   headers, including trunk/
openrtx/src/protocols/DMR/       the protocol layer, including aes/ and trunk/
openrtx/include/rtx/OpMode_DMR_RX.hpp
openrtx/src/rtx/OpMode_DMR_RX.cpp
scripts/c62/fetch_mbelib.sh      AMBE+2 decoder, fetched and patched
scripts/c62/fetch_opendmr.sh     AMBE+2 encoder, the same way
```

About 15 700 lines in `protocols/DMR` and 2 400 in the operating mode.

### The layers

| File | What it does |
|---|---|
| `dmr_demod.c` | 4FSK demodulator off the discriminator: sync search, symbol timing, soft values |
| `dmr_fec.c`, `dmr_trellis.c` | BPTC(196,96), Golay, QR, Reed-Solomon, the 3/4 rate trellis |
| `dmr_proto.c` | Bursts, link control, EMB, CSBK, the slot state machine |
| `dmr_voice.c`, `DmrSpeech.c` | Voice frames, the playback queue and the listen filter |
| `dmr_tx.c` | Transmission: bursts, voice superframes, repeater slots, simplex |
| `dmr_data.c`, `dmr_sms.c`, `dmr_lrrp.c` | Data calls, text messages in three formats, LRRP positions |
| `dmr_ep.c` | Motorola Basic and Enhanced Privacy (ARC4, 40-bit), receive and transmit |
| `dmr_aes.c` + `aes/` | AES-128/256 and DES, shared with the P25 mode |
| `dmr_hytera.c` | Hytera Basic and Enhanced Privacy |
| `dmr_track.c` | Stations heard, talkgroups, call tracking |
| `trunk/` | Tier III trunking, ported from RadioFoundry (see `LICENSE.radiofoundry`) |

## What it needs from the platform, and is not here

- **The vocoder.** `fetch_mbelib.sh` brings the AMBE+2 decoder and
  `fetch_opendmr.sh` the encoder; both patch what they fetch. The build only
  turns the encoder on when OpenDMR is present.
- **The baseband path.** The demodulator wants the discriminator flat and wide:
  on the BK4819 that is the 12.5 kHz mode with the RF filter open. It takes
  48 kHz samples; `dmr_demod.h` says what it expects.
- **Non-volatile records and settings.** Keys, talkgroup lists, the listen
  filter and the transmission settings live in the radio's own nvmem and CPS
  layer here, which is C62 specific. The protocol layer only reads them through
  its own setters, so wiring them up is a per-tree job.
- **The UI.** Call cards, the talkgroup page and the message inbox are not in
  the package.

## Two things worth knowing before you apply it

1. **`DmrSpeech.h` is shared.** Every digital mode's operating mode in this tree
   includes it (DMR, NXDN, dPMR, P25, D-STAR, YSF): it is the speech and
   playback layer, not a DMR-only header. Expect it to collide with whatever
   plays voice in your branch.
2. **`dmr_aes.h` is used by P25** (`p25_rx.cpp`, `p25_tx.cpp`) for its own
   AES-256. If you take the DMR encryption, those come with it.

## Licences

`trunk/` carries `LICENSE.radiofoundry`, and `aes/` carries `LICENSE.dmrgo` and
`UNLICENSE.tinyaes`. They stay with their files.
