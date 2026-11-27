# NXDN receive mode

NXDN (NXDN Forum TS 1-A/1-B, JVC Kenwood + Icom) receiver for the Retevis C62:
4FSK demodulation of the radio's discriminator output, frame sync, LICH, the
SACCH superframe with its layer 3 messages and voice through the AMBE+2
decoder. Transmit with `CONFIG_NXDN_TX`: `protocols/NXDN/NxdnTx.c` builds the
frames this receiver decodes (`docs/TX_NXDN.md`).

The chain was validated on two real off-air captures before anything was
integrated, against `dsd-neo` as the reference decoder. This document is the
specification, the evidence, the cost and the hooks the UI needs.

## Enabling it

```
west build -b c62 -d build_c62 . -- -DCONFIG_C62_NXDN_RX=y
```

`CONFIG_C62_NXDN_RX` (`platform/targets/c62/Kconfig.board`) selects
`CONFIG_C62_DMR_VOCODER` and adds the protocol sources, `OpMode_NXDN` and
`CONFIG_NXDN`: that is what turns `UI_MODE_NXDN` on, and with it the mode's
entry in the registry (`uiModes[]`, `ui/ui_modes.h`), its quick menu, its page
in Settings, its label in the header and its entry in the channel editor. With
the option off none of that is compiled in and the mode is not offered.

`OPMODE_NXDN = 5` is the only enum value this work adds (`rtx.h`); `rtx.cpp`
has the instance, the selector case and `opmode_canTransmit()` returning false
for it. `radio_C62.cpp` configures the BK4819 for it.

## 0.1. Lo que la UI enseña y lo que deja tocar

La UI vive en la rama `c62/ui-modes` y no tiene nada propio de NXDN más allá de
su tabla de filas: ver `docs/UI.md` §«Añadir un modo» para el procedimiento y
§«NXDN» para la tabla de filas.

| pieza | fuente |
|---|---|
| etiqueta `NXDN` en la cabecera | `chstore_modeName(OPMODE_NXDN)` |
| línea 2: `RAN12 VOX` / `NXDN SRCH` | `nxdn_getStatus()` (`ran`, `sync`, `voice`) |
| línea 1: `e3% f1234` | `nxdn_getStatus()` (`evm`, `frames`) |
| filas `RAN`, `BW`, `SYNC`, `CALL`, `TYPE` | `qm_items_nxdn[]`, menú rápido y Ajustes → NXDN |

`RAN` es un **filtro de recepción**, no un número de adorno: se guarda en
`settings.nxdn_ran` (0..63, `0xFF` = cualquiera, el valor por defecto) y
`OpMode_NXDN::onFrame()` descarta la voz y los mensajes de capa 3 de otro RAN.
El RAN viaja en los bits SR del SACCH y no en el LICH, así que el filtro solo
puede cerrarse cuando ya ha decodificado un superframe: durante los primeros
~320 ms de portadora pasa todo. `BW` elige 12,5 o 6,25 kHz y lo único que cambia
en el chip es el filtro de FI (3,3 ≈ 2,9 kHz para 6,25; 7,7 = 4,5 kHz, que es
con el que se tomaron todas las medidas de este documento, para 12,5).

Dos cosas de este apartado **no se han medido en el aire**: el ajuste de 6,25
kHz (solo recorta el filtro de FI, la selección de canal sigue siendo el filtro
adaptado del demodulador) y el filtro RAN (que por defecto está en «cualquiera»,
así que el camino medido es el de siempre). Las dos son de quitar y poner en el
menú, y el resto del modo no depende de ellas.

## 1. Air interface

### Symbol rate, modulation, pulse shape

| | NXDN48 | NXDN96 |
|---|---|---|
| bits/s | 4800 | 9600 |
| symbols/s | **2400** | 4800 |
| bits/symbol | 2 (4FSK) | 2 |
| frame | 192 symbols = 80 ms | 192 symbols = 40 ms |
| channel | 6.25 kHz (2 per 12.5 kHz) or 12.5 kHz | 12.5 kHz |
| deviation, outer/inner | ±350 / ±116.7 Hz (6.25 kHz), ±1050 / ±350 Hz (12.5 kHz) | ±1050 / ±350 Hz |

4FSK with a root-raised-cosine of roll-off 0.2 (TS 1-A 3.4/3.5; ETSI TS 102 658
clause 14.2.2.2 for the dPMR twin). The standard splits the pulse between the
two ends: the transmitter sends `H(f) P(f)` and the receiver takes `H(f) D(f)`,
with `H(f)` the root raised cosine, `P(f) = sinc(fT)` **the hold of a symbol for
its period** and `D(f) = 1/sinc(fT) = pi f T / sin(pi f T)` over
`|f| <= (1 + 0.2)/2T`. The two together are the Nyquist raised cosine `H(f)^2`,
zero ISI at the symbol instants. A receiver that stops at `H(f)` -- a plain RRC,
which is what this mode had until the filter below landed -- is the matched
filter of half the transmitted pulse: it leaves the pair at `RC(f)/sinc(fT)`,
which is not Nyquist, and misses the band-edge energy the hold puts there.
`dsd-neo`'s filter note (`src/dsp/dsd_filters.c`) is the clearest public
statement of this and is what both this receiver's filter and its sync detector
were written around.

The receive table in `NxdnPhy.c` is `H(f) D(f)` at 48 kHz, 20 samples per
symbol, four symbols each way (161 taps, the same count the plain RRC had, so
the cost did not move). `tools/nxdn_rx_pulse.py` evaluates it by quadrature from
the specification; the result agrees with `dsd-neo`'s independently generated
`nxdn_dpmr_rx_taps` to 5e-10 per tap, which is the cross-check that the two
readings of 3.4/3.5 are the same reading.

**This work implements NXDN48 (2400 symbols/s).** That is the mode used on
6.25 kHz channels and on most 12.5 kHz Kenwood NEXEDGE traffic. NXDN96 is the
same frame at twice the symbol rate: `NXDN_SPS` in `NxdnPhy.c` is the only
constant that would change (10 → 5), plus the frame timing, and it is not
verified here because no NXDN96 capture was decoded fully enough to prove it
(see §5).

### Frame

192 symbols, 384 bits. The LICH and everything after it is scrambled; the FSW is
not.

| symbols | field | bits |
|---|---|---|
| 0–9 | frame sync word (FSW), `CDF59` | 20 |
| 10–17 | LICH | 16 |
| 18–47 | SACCH | 60 |
| 48–191 | VCH 1 … VCH 4 (four voice channels of 20 ms) | 4 × 72 |

* **FSW** (TS 1-A 4.4.4, Table 4.4-2): 10 symbols `-3 +1 -3 +3 -3 -3 +3 +3 -1
  +3`, hex `CDF59` read two bits per symbol. Sum zero, so a carrier offset does
  not bias the correlation. There is one FSW for every channel type — no
  separate voice and data words as in DMR.
* **Preamble** (4.4.3): `5775FD`, a repeat of `+3 +3 -3 -3`; only the first
  frame of a transmission carries it.
* **Scrambler** (4.6): a 9-bit LFSR, X⁹ + X⁴ + 1, default seed `0b011100100`,
  one output bit per symbol, inverting the *sign* of the symbol. It runs over
  the 182 payload dibits of every frame, LICH included. Only the default seed is
  implemented; a non-default one comes from a layer 3 message and is not
  supported (it would show as a LICH parity failure).
* **LICH** (5.2.1, Table 5.2-1; 4.5.3, Figure 4.5-7): 7 control bits plus one
  even parity bit over the 4 MSBs, each coding bit sent as one dibit whose
  **sign** is the data bit (so an inverted discriminator inverts the LICH).
  Bits [6:5] RF channel type, [4:3] functional channel, [2:1] option (steal
  flag), [0] direction.

| RF type | meaning |
|---|---|
| `00` | RCCH (control channel) |
| `01` | RTCH (traffic, trunked) |
| `10` | RDCH (traffic, conventional / direct) |
| `11` | RTCH_C (composite control) |

| USC | meaning, on a traffic channel |
|---|---|
| `00` | SACCH, non-superframe (a call's first and last frame) |
| `01` | UDCH (user data) |
| `10` | SACCH, superframe (voice frames) |
| `11` | SACCH superframe with an idle FACCH1 |

Steal flag `11` = no steal (all four VCH are voice), `01` = VCH1/VCH2 are
FACCH1, `10` = VCH3/VCH4, `00` = both halves are FACCH1. On the capture the
LICH decodes to `0x57` = RDCH outbound, SACCH superframe, no steal, which is
exactly the four-VCH voice frame this receiver expects.

### Superframe

A voice call's first and last frames are `SACCH non-superframe`; everything in
between is a superframe of **four frames** (TS 1-A 4.1.1, Figure 4.1-1; TS 1-B
4.1.1). The SACCH is the only channel with the superframe structure: each frame
carries one 18-bit segment of a 72-bit layer 3 message, and the segment's
position is announced in the 2-bit SF field at the head of the SR byte.

At 9600 bit/s with the half-rate vocoder the frame alternates voice and FACCH1
(TS 1-B 4.1.2.1), which is why the steal flag exists.

## 2. Channel coding

### Voice channel: none of ours

**A VCH is 72 bits of AMBE+2 at 3600 bit/s, and those 72 bits already are one
complete AMBE+2 3600x2450 frame** — 12 + 12 + 25 information bits protected by
Golay(24,12) and Golay(23,12) inside the vocoder. TS 1-A 4.5.2 says it
outright: *"Channel coding on VCH is described in another section"*, and that
section is the vocoder specification, not the air interface. So:

* there is **no outer FEC, no interleaving and no CRC on NXDN voice**, unlike
  DMR where the 72 bits are spread over the burst;
* the 36 dibits of a VCH sit contiguously, in order, one channel per 20 ms;
* the 72 bits go to `ambe_fr[4][24]` through **the same schedule DMR uses**
  (`dmr_rw/dmr_rx/dmr_ry/dmr_rz` in `protocols/DMR/DmrDefs.c`, which is
  `dsd_ambe_2450_dibit_map` in `dsd-neo`).

### Does the C62's AMBE+2 decoder serve NXDN as it is?

**Yes, unchanged — and that is the point, not a shortcut.** `lib/dmr_vocoder`
takes nine bytes in the canonical `A(24) | B(23) | C(25)` order and decodes
them. Those nine bytes are the DMR voice frame *after* its burst de-interleave,
and they are produced from the on-air 72 bits by one fixed permutation. NXDN
transmits the same 72 bits with the same permutation and no burst interleave,
so the only difference is *where the 36 dibits come from*:

| | DMR | NXDN |
|---|---|---|
| 72-bit frame | 1/3 of the burst payload, interleaved over the 30 ms slot | one VCH, contiguous |
| bit order into the vocoder | `protocols/DMR/DmrFec.c` (before the package: now the tables are private to `NxdnFec.c`) | `nxdnFecVchToAmbe()`, same tables |
| vocoder | `dmr_vocoder_decode()` | `dmr_vocoder_decode()`, same code |

There is no second copy of the vocoder in the image: `CONFIG_C62_NXDN_RX`
selects `CONFIG_C62_DMR_VOCODER` and both modes call the same function.

This is *not* the 3600 bit/s "EFR" vocoder of TS 1-B 4.1.2: that one is 144
bits per 20 ms, used at 9600 bit/s, and it is a different AMBE+2 rate that the
C62 does not have.

### LICH

8 coding bits + 1 even parity bit over the 4 MSBs, each bit sent as one dibit
with the data in the sign (half of the dibit is wasted). No FEC, no
interleaving. The parity is the receiver's first bit-level check on a frame and
it is what settles the *polarity* of the discriminator: the two inner symbols
of the FSW are the ones a real channel distorts, so the negated sync word can
match as well as the true one, and the parity picks the reading (`decode_frame`
tries both).

### SACCH

60 bits per frame, one punctured convolutional block (TS 1-A 4.5.2.1,
Figure 4.5-4):

```
18 bits layer 3 + 8 bits SR                       26
   + 6-bit CRC  (X^6 + X^5 + X^2 + X + 1)          32
   + 4 zero tail bits                              36
   -> rate 1/2, K=5, G1 = 1+D^3+D^4, G2 = 1+D+D^2+D^4   72
   -> punctured 111111 / 110110                    60
   -> interleaved, five rows of 12 read column-wise
```

The decoder de-interleaves, un-punctures (the two erased G2 bits of each group
of six become erasures), runs a hard-decision Viterbi over the 16-state trellis,
and checks the CRC-6 in a register that starts all ones — the reference
decoders' convention, and the one a transmitter has to match.

**There is no Golay code in the SACCH**, and no Reed-Solomon anywhere in NXDN
voice. Golay appears in NXDN only inside the AMBE+2 frame, which is the
vocoder's business.

### FACCH1 / FACCH2 / UDCH

Not implemented. They carry the layer 3 messages that do not fit in the SACCH
(call setup on a control channel, short data, paging), and each has its own
12-bit CRC, puncturing matrix and inter-frame interleaving. See §4.

## 3. Messages

The SACCH carries a 72-bit layer 3 message per superframe. What this firmware
decodes:

| type | name | fields decoded |
|---|---|---|
| `0x01` | VCALL | call type (3 bits), source unit ID (16), destination / talkgroup (16) |
| `0x07` | TX_REL_EX | source, destination |
| `0x08` | TX_REL | source, destination |
| `0x11` | DISC | source, destination |
| `0x35` | ALIAS | recognised, not interpreted (a text alias in 4 segments) |

The RAN is not a field of the message: it rides in bits 2..7 of the 8-bit SR
byte of every SACCH block, and is shown from there.

Layout of a VCALL, from the first bit of the 72 (bit indices in the message):
`0–7` message type, `8–15` call control option, `16–18` call type, `19–23`
voice call option, `24–39` source ID, `40–55` destination, `56–57` cipher type,
`58–63` key ID.

### Are there short data / text messages?

**Yes, NXDN has them, and this receiver does not decode them.** NXDN defines a
Short Data Call (TS 1-C §12, `SDCALL`, "Broadcast" and "Unit to Unit") and a
status call, both carried in **FACCH1** (144 bits, 12-bit CRC, its own
puncturing matrix, interleaved across two frames), and a data call in
**UDCH/FACCH2** (348 bits, 16-bit CRC). The text itself is up to 255 characters
in the data-call PDU. Nothing in the two captures used here carried one — their
SACCH messages are VCALL, TX_REL and ALIAS — so implementing the FACCH1 path
would have been code without a bit-level reference to check it against, and
this is a receive path for voice. It is the next useful piece of work, and the
first thing it needs is a capture of a real short message.

## 4. The chain, as it runs on the radio

```
48 kHz discriminator (BK4819 audio bypass, 12.5 kHz channel, REG_43 = 0x7e48)
  H(f) D(f) matched filter of TS 1-A 3.4/3.5, 161 taps at 48 kHz, decimated
       by two (10 samples/symbol at 24 kHz)
  pre-gate over 11 instants across one whole symbol period
  sync search on the 10-symbol FSW, signs only, both polarities; a free-running
       acquisition needs a near-perfect field, a tracked frame does not
  per-frame fractional timing: 16 phases of a 4-point Lagrange interpolation
       over +-2.5 samples, metric = sync correlation + 0.35 * payload EVM
  levels: median DC, then 4-means (outer, inner = outer/3) with a drift term
  slice to dibits, descramble (XOR with the mask, plus the polarity)
  LICH -> parity -> polarity and RF/functional/steal fields
  SACCH -> per-bit reliabilities (max-log LLR over the four levels) ->
       de-interleave, de-puncture, soft Viterbi, CRC-6, hard fallback
  VCH 1..4 -> nxdnFecVchToAmbe() -> 9-byte AMBE+2 frames
  voice -> NxdnSpeech queue -> playback thread -> dmr_vocoder_decode() -> 8 kHz
```

Everything is on the AP. `-DCONFIG_C62_NXDN_RX=y` costs 6 828 B of flash and
15 000 B of RAM over the DMR+APRS build, plus the 644 B of the transmit pulse
table in `Fsk4Tx.c` (§6).

The demodulator started as the DMR one, and that was not a coincidence: NXDN48
is 2400 symbols/s with RRC 0.2, which is the same symbol rate and the same pulse
family DMR's chain was built and measured for (81 taps at 24 kHz, ten per
symbol, eight symbols of span). What is new is the frame -- one sync word
instead of four slot words, no CACH, and 192 symbols per frame instead of 144
per slot -- and, since this work, the half of the pulse the receiver was
missing: DMR has no symbol hold and a plain RRC is right there, NXDN does and a
plain RRC is 1.5 dB of matched-filter gain and a closed eye left on the table.

Two things were learned the hard way while bringing the capture up and are worth
keeping:

* **The scan's guard has to be in the ring's units.** The ring counts 24 kHz
  samples and the scan counts symbols; comparing them directly lets the scan run
  off the end of the history, and `mf_at()` then returns zeros. The symptom is a
  receiver that finds nothing while every correlation on the same data, taken
  offline, scores 1.0.
* **Match the FSW on signs, not on levels.** Requiring the exact `±3/±1`
  pattern of the sync field rejects real signals: the channel distorts exactly
  the two inner symbols of the field. With signs only the detector finds every
  frame of both captures.

## 5. Captures used, and what they prove

Both are the sigidwiki NXDN examples (`sigidwiki.com/wiki/NXDN`, "I/Q Raw
Recording", `NXDN_IQ.zip`): 48 kHz complex int16, 11.8 s and 15.3 s. They were
converted to the 48 kHz real discriminator the radio hands the mode by
`angle(z[n]·conj(z[n-1]))`, which is what an FM demodulator produces — so the
demodulator works on the same kind of signal in both cases.

The reference is `dsd-neo` v2.13.0 (nightly AppImage) on the same converted
files, with `-Z` for its per-frame trace.

| | NXDN48 capture | NXDN96 capture |
|---|---|---|
| dsd-neo reference | 452 AMBE frames, VCALL Src=901 Dst=0, RAN 01, alias text | 712 AMBE frames, NXDN96 |
| **this chain** | **123 frames, 0 LICH errors, 118 with voice, 466 AMBE frames** | syncs but no voice decoded |
| SACCH | 54 / 123 blocks pass CRC-6, 4 VCALL messages assembled | — |
| polarity | 121 of 123 frames decode inverted (the capture's I/Q convention) | — |

What that pins down:

* **Frame layout and scrambling.** The LICH decodes with **zero parity failures
  on all 123 frames**, to `0x57` = RDCH / SACCH superframe / no steal, the same
  value `dsd-neo` prints. A wrong FSW offset, a wrong scrambler mask or a wrong
  LICH bit order cannot produce that.
* **SACCH, bit for bit.** The assembled VCALL is
  `[01][00][20][03][85][00][00][00][00]` with **Src=901, Dst=0** — identical to
  what `dsd-neo` decodes from the same capture. That covers the de-interleave,
  the un-puncturing, the Viterbi, the CRC-6, the SF position and the layer 3
  layout. Getting any of them wrong leaves the CRC at noise level (0/123); the
  first working combination was found by brute force over the four plausible
  conventions, and only one of them lifts the CRC from 2/123 to 54/123.
* **Voice bit mapping.** Of the 62 frames this chain reports with **zero Golay
  corrections**, 27 are bit-identical, as 49-bit AMBE payloads, to a frame
  `dsd-neo` printed. The remaining 35 differ in the 25 unprotected C bits — no
  FEC protects them, so a single slicer error changes the word while the Golay
  blocks still report zero corrections.
* **Continuity.** 466 AMBE frames decoded over 10.35 s of PCM (the capture
  contains silences; the harness pads the timeline with them).

### What is *not* proven

* **NXDN96 does not decode.** The 15.3 s capture syncs the frame grid but no
  voice: `NXDN_SPS` is 10 in this build. Wrong symbol rate, so this is expected,
  not a defect — but it means the 12.5 kHz / 9600 bit/s combination is
  unverified.
* **6.25 vs 12.5 kHz channel spacing.** Both captures are 12.5 kHz-class signals
  (outer deviation ≈ ±1 kHz). NXDN48 in a 6.25 kHz channel has a third of that
  deviation; the level estimation is automatic, so it should work, but no such
  capture was available and it has not been tried.
* **Nothing has been received on the radio itself.** Every number above comes
  from files, through the same C code the firmware runs. What the BK4819 of a
  real C62 hands over — level, bandwidth, continuity — is what
  `scripts/c62_bbdump.py` captures (`docs/DMR_RX.md`, *Capturing what the radio
  hears*); the NXDN mode takes the same path, so the same tool works unchanged.
* **The capture itself is not in the tree.** The 11.8 s NXDN48 file the numbers
  above come from is not; what is committed is the 1.5 s slice
  `tests/unit/assets/nxdn_air_frame.wav`, which is the clean stretch. The
  sensitivity numbers of §5.1 are measured on that slice and on noise added to
  it, which is why they can be reproduced without the original.

## 5.1 Sensitivity: frames, LICH and SACCH against Eb/N0

The capture decodes, on a clean signal, with either front end: the receiver's
weakness was never the clean case. It was that the flat RRC and the hard
decisions gave up much earlier than the reference on a noisy one, and the first
thing this work did was make that measurable.

`tests/platform/c62_nxdn_bench.c` pushes the 1.5 s asset through
`nxdnPhy_push()` with white Gaussian noise added at a chosen Eb/N0:

```
Eb/N0 = Ps * Fs / (4 * sigma^2 * Rs)   ->   sigma^2 = 5 * Ps / Eb/N0    (48 kHz)
```

with `Ps` the mean square of the file (DC removed), `Rs` = 2400 symbols/s and
two bits per symbol. The noise comes from a fixed xorshift64\* stream, so a
(Eb/N0, seed) pair is the same file on every machine, and `--write-dir` writes
those files so the reference decoder can be run on exactly the same input
(`tools/nxdn_ref_compare.py`). 24 seeds per point, mean per point, out of the
18 frames the slice carries. The clean row is the "no regression" check as
well: on the committed slice both front ends produce 18 frames, no LICH error,
18 SACCH blocks and the same VCALL -- `[01][00][20][03][85]...`, source 901,
destination 0, byte for byte what `dsd-neo` decodes from it -- so the table
below is a change in how far into the noise the chain works, not in what it
produces.

| Eb/N0 | flat RRC, hard |  |  | `H(f) D(f)`, hard |  |  | `H(f) D(f)`, soft + fallback |  |  |
|---|---|---|---|---|---|---|---|---|---|
| | frames | LICH | SACCH | frames | LICH | SACCH | frames | LICH | SACCH |
| clean | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 |
| 18 dB | 17.0 | 15.9 | 14.9 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 |
| 16 dB | 17.2 | 16.6 | 15.2 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 | 18.0 |
| 14 dB | 16.0 | 14.4 | 12.7 | 18.0 | 18.0 | 17.9 | 18.0 | 18.0 | 18.0 |
| 12 dB | 14.8 | 12.2 | 10.3 | 18.0 | 18.0 | 17.9 | 18.0 | 18.0 | 18.0 |
| 10 dB | 15.0 | 12.8 | 10.5 | 17.6 | 17.4 | 16.9 | 17.6 | 17.4 | 17.4 |
| 8 dB | 10.3 | 6.2 | 3.9 | 18.0 | 18.0 | 16.5 | 18.0 | 18.0 | 18.0 |
| 6 dB | 11.6 | 8.7 | 4.8 | 17.8 | 17.5 | 13.4 | 17.8 | 17.5 | 17.5 |
| 4 dB | 12.1 | 9.9 | 3.1 | 16.8 | 16.0 | 7.7 | 16.8 | 16.0 | 12.3 |
| 2 dB | 9.7 | 7.4 | 1.2 | 15.6 | 14.8 | 3.0 | 15.6 | 14.8 | 6.8 |
| 0 dB | 11.0 | 9.5 | 0.5 | 14.0 | 12.6 | 0.5 | 14.0 | 12.6 | 1.8 |
| −2 dB | 7.4 | 5.8 | 0.1 | 9.2 | 7.5 | 0.1 | 9.2 | 7.5 | 0.5 |

Three things this table says, in order of size:

1. **The matched filter is worth far more than its 1.5 dB.** The flat RRC does
   not mostly lose *bits*: it loses the **frame grid**. Its eye is soft and its
   inner levels sit pulled towards the outer ones, so a marginal 9-of-10 match
   somewhere in the payload wins the scan, the grid latches a few symbols early
   and every frame after it decodes with a dead LICH. That is the 8 dB row: 6.2
   frames of 18 with a good LICH, where `H(f) D(f)` holds 18 of 18. The
   matched-filter gain on top of that shows in the SACCH count and in the EVM
   (0.140 → 0.063 on the clean capture, 0.189 → 0.172 at 8 dB).
2. **The acquisition gates had to change with it.** Because the new eye makes
   that marginal match *more* marginal (one fewer sign error at the false
   position), a free-running acquisition now needs a sync score of 0.92 instead
   of 0.72 -- a tracked frame still accepts 0.72 -- and a grid whose frames fail
   their LICH four times in a row is dropped instead of being kept for ever.
   Without those two, the better filter decodes *fewer* frames than the worse
   one: it is the same failure, just reached sooner.
3. **Soft decisions are worth about a decibel at the floor.** They change
   nothing at 8 dB and above, where the block is already right, and they double
   the SACCH blocks recovered at 2 dB (3.0 → 6.8 of 18). The gain is in the
   region where a voice call is breaking up anyway, which is exactly where a
   SACCH block still matters: it carries the VCALL, the RAN and the release.

### Against the reference decoder, on the same files

`tools/nxdn_ref_compare.py` runs `dsd-neo` v2.13.0 (56cc137, built from source
in the `dmr-rx` workbench, `ref/dsd-neo`) over the very WAVs the bench wrote, and
counts its own trace: syncs, `LICH ERR` lines, SACCH segments it did not flag
`CRC ERR`, and AMBE frames. Four seeds per point, the same four this chain's row is
measured on:

| Eb/N0 | 16 dB | 12 dB | 8 dB | 4 dB | 0 dB |
|---|---|---|---|---|---|
| dsd-neo syncs | 17.5 | 16.5 | 16.2 | 17.2 | 16.2 |
| dsd-neo LICH errors | 3.8 | 2.2 | 2.8 | 1.8 | 4.0 |
| dsd-neo SACCH (CRC ok) | 13.8 | 13.0 | 7.2 | 2.0 | 0.0 |
| **this chain, SACCH (CRC ok)** | **18.0** | **18.0** | **18.0** | **10.2** | **1.8** |

On the clean asset the reference decodes 18 frames, 16 SACCH segments and 60
AMBE frames with 2 LICH errors; this chain decodes 18, 18 and 72 with 0. On the
noisy variants it is ahead at every point measured.

That is not a claim that this receiver is better than `dsd-neo` in general, and
two things have to be said about it:

* **The AMBE counts are not comparable.** `dsd-neo` only synthesises voice --
  and only logs an AMBE frame -- once a CRC-valid frame has confirmed the call
  (`nxdn_confirm_is_confirmed()`, `nxdn_frame.c`), so its AMBE count measures
  its call state machine as much as its demodulator: it reports 0 AMBE frames at
  0 dB where it still decodes SACCH segments. This chain hands everything it
  decodes to the vocoder and lets the audio be bad. The comparable number is the
  SACCH block whose CRC-6 passes.
* **The file is the clean stretch.** On the full 11.8 s capture of §5 the
  reference decodes 452 AMBE frames where this chain decodes 466 but only 54 of
  123 SACCH blocks, and the reason given there -- a plain RRC and hard
  decisions -- is what this work removed. Re-running that comparison needs the
  original capture, which is not in the tree; the numbers above are what can be
  reproduced.

Reproduce it:

```sh
cmake -S lib/dmr_vocoder -B build_dmr_vocoder && cmake --build build_dmr_vocoder
cc -O2 -I openrtx/include -I lib/dmr_vocoder tests/platform/c62_nxdn_host.c \
   openrtx/src/protocols/NXDN/{NxdnDefs,NxdnFec,NxdnPhy}.c \
   openrtx/src/protocols/DMR/DmrDefs.c build_dmr_vocoder/libdmr_vocoder.a \
   -lm -o c62_nxdn_host

# whole capture: PCM out, one CSV row per frame, AMBE frames, SACCH messages
./c62_nxdn_host -o out.wav -c frames.csv -a frames.txt -m msgs.txt nxdn48_disc.wav
```

`NXDN_DUMP=1 ./c62_nxdn_host ...` also writes the 60 raw SACCH bits of every
block to `/tmp/sacch_bits.txt` and the Golay error count of every decoded frame
to `/tmp/frames_err.txt`; those are what the two brute-force checks in §5 were
run on.

The sensitivity numbers of §5.1 have their own harness, built by meson as
`c62_nxdn_bench` (it is a host program, not a Catch2 test) and by hand with:

```sh
cc -O2 -ffp-contract=off -I openrtx/include tests/platform/c62_nxdn_bench.c \
   openrtx/src/protocols/NXDN/{NxdnDefs,NxdnFec,NxdnPhy}.c -lm -o nxdn_bench

# the table of §5.1: 24 noise seeds at each point, CSV for the per-run rows
./nxdn_bench --seeds 24 --snr 18,16,14,12,10,8,6,4,2,0,-2 \
              --csv bench.csv tests/unit/assets/nxdn_air_frame.wav

# the same captures, kept, for the reference decoder
./nxdn_bench --no-clean --seeds 4 --snr 16,12,8,4,0 --write-dir /tmp/wavs \
              tests/unit/assets/nxdn_air_frame.wav
tools/nxdn_ref_compare.py /tmp/wavs --dsd-neo /path/to/dsd-neo
```

`./nxdn_bench --dump ...` prints one line per frame (symbol, sync errors,
score, EVM, LICH and SACCH), which is how the false-sync lock of §5.1 was found.
`tests/platform/c62_nxdn_gen.c` synthesises a call with this tree's own
transmitter (preamble plus N frames, the AMBE payload a counter) for a capture
whose contents are known -- that is what the locked 12 s cost run below uses.
`meson test` runs `nxdn_rx_test`, which is the same measurement cut down to
three assertions: the clean capture unchanged, the grid held at 12 and 8 dB, and
the reliability path recovering more SACCH blocks than the hard one at 2 dB.

## 6. Cost

### Instructions (QEMU `mps2-an505`, Cortex-M33, no DSP extension)

`tests/platform/c62_nxdn_phy_cost.c` pushes 12 s of discriminator audio through
`nxdnPhy_push()` and drops a frame marker every second
(`qemu-mcu-instruction-profiling`, plugin `work/dsp-probe/common/insnframe.c`).
The demodulator only: no vocoder, no audio interface.
`tests/platform/run_nxdn_phy_cost.sh` builds it for the M33 and runs it under
QEMU; `meson test` runs it on the asset and skips (77) where the toolchain or
QEMU is missing. Set `NXDN_INSNFRAME` to the plugin to get instructions.

Two 12 s inputs, because the two regimes cost very differently, and the same
input on both sides of the change:

* **locked** -- `c62_nxdn_gen` framed a call with this tree's transmitter, so
  all 149 frames decode and the grid is never lost;
* **re-acquiring** -- the 1.5 s asset repeated eight times, which puts a frame
  discontinuity every second and makes the scan work for a living.

| 12 s input | flat RRC, hard | `H(f) D(f)` + soft | delta |
|---|---|---|---|
| locked (149 frames) | 34.8 M/s | **34.9 M/s** | **+0.3 %** |
| re-acquiring (81 frames) | 29.5 M/s | **30.2 M/s** | +2.4 % |
| noise only (searching, no frames) | 24.5 M/s | 26.2 M/s | +7 % |

Per second of audio and as a share of the 300 MHz core: 34.9 M/s is 11.6 %.
**The absolute figures do not match the 13.8 – 14.1 M/s this section used to
quote**, and neither does the old chain's 34.8 M/s on the same input, so that
row was measured on something this tree no longer has — most likely with the
81-tap DMR table the comment in `NxdnPhy.c` describes as the filter before the
161-tap one (the matched filter alone is 3.86 M MAC/s, about 10 M instructions
per second of audio). What is measured here, on the same input and the same
toolchain as both builds, is the ratio, and that is what the CPU budget cares
about: **the two improvements together cost +0.3 % where frames decode.**

In frames: the locked capture decodes 149 frames in 12.08 s, so **2.82 M
instructions per frame before and 2.83 M after** -- the budget for this work was
"no more than twice the cost per improvement", and it came in at 1.003x.

Where the money goes: the matched filter is the same 161 taps it was, the
reliabilities are one small loop over 60 symbols per frame, and the extra
Viterbi pass only happens on blocks whose CRC fails. The acquisition change
costs more, but only when the grid is lost -- a stronger sync gate and, after
four dead frames, a free-running scan over up to 192 symbol positions.

`-Os` instead of `-O2` (what the Zephyr image uses) was measured this time:
27.4 M/s before and 28.1 M/s after on the re-acquiring input, i.e. the same
+2.6 % at 8 % lower cost.

For scale, the DMR PHY is 20.2 M/s in the same conditions
(`docs/DMR_RX.md`), and the vocoder is the same code and the same cost as DMR's:
751 k instructions per 20 ms frame, 15-20 % of the core
(`docs/DMR_VOCODER.md`).

### Flash and RAM

Measured with the Zephyr build (`scripts/c62/build.sh`), same options on both
sides (`-DCONFIG_C62_DMR_RX=y -DCONFIG_C62_APRS=y`):

| build | FLASH | RAM |
|---|---|---|
| master, DMR RX + APRS | 518 144 B (49.41 %) | 196 688 B (75.03 %) |
| **+ NXDN RX** | **524 972 B (50.07 %)** | **211 688 B (80.75 %)** |
| difference | **+6 828 B** | **+15 000 B** |

`zephyr.bin` is the figure quoted; the ELF's FLASH region reads the same. The
118 B of slack against the 518 104 B quoted elsewhere is the `git describe`
string the image embeds, which depends on the commit it is built from.

The tables of this work cost nothing here: the receive pulse replaced the RRC
table tap for tap (161 floats, no change) and the soft decisions add no state.
The one addition is the transmit pulse in `Fsk4Tx.c`, **644 B of flash** (161
floats of `.rodata`) that only an NXDN image links — the mode is receive-only in
this fork, so no image that does not transmit NXDN pays for it.

Where the RAM goes:

| object | bytes | note |
|---|---|---|
| PHY ring | 8 192 | 2 048 floats of matched-filter output at 24 kHz; a frame is 1 920 samples, so this is the minimum that lets a whole frame be decoded in one go |
| baseband block | 3 840 | 2 × 960 samples of 48 kHz input, 20 ms |
| frame window | 768 | 192 floats of symbol values |
| speech queue and PCM | ~1 500 | 32 AMBE frames, 2 × 160 samples of PCM, thread stack 8 KiB (the DMR value) |
| everything else | ~700 | status, handler, audio path |

It is 80.75 % of the 256 KB region, i.e. **44 616 B still free**, which is the
number that matters (see the RAM rule in the fork's notes). The ring is the one
large item and it is the price of decoding a whole frame in one call: sampling
each symbol into the frame window as it arrives instead would let the ring drop
to 1 024 floats (4 KiB) and bring the total to ~76 %, at the cost of ~40 lines
in `scan()` and `decode_frame()`. It was left alone because the whole-frame
decode is what the captures above validated.

## 7. What the screen should show

Nothing in `openrtx/src/ui/**` was touched. `nxdn_getStatus()` fills this, and
the UI should read it the way it reads `dmr_getStatus()`:

```c
struct nxdnStatus st;
nxdn_getStatus(&st);
st.sync            // a frame decoded in the last 300 ms
st.voice           // a voice frame in the last second
st.ran             // -1 until a SACCH block decodes
st.rfType, st.usc, st.steal   // LICH fields, for a debug line
st.inverted        // the discriminator is inverted on this signal
st.evm             // EVM of the last frame, per mille
st.message.valid   // a layer 3 message was assembled
st.message.type    // 0x01 VCALL, ... (nxdnFecMsgName())
st.message.source, st.message.dest
st.frames, st.voiceFrames, st.ambeFrames, st.lichErrors
st.sacchChecks, st.sacchOk, st.messages
```

Suggested layout, matching the DMR and TETRA screens (line 2 is the identity
line they all use):

* line 2: `RAN 01` once a SACCH block decodes, `NXDN searching` before that,
  and `NXDN inv` when `inverted` is set (worth showing: it means the signal is
  being received on the wrong sideband of the discriminator and the audio is
  still correct, which is not obvious otherwise).
* line 1: `VCALL 901>0` while a call's message is current, or `Voice e12%` with
  the EVM, or the message type of the last assembled one.
* The green LED follows `nxdnPhy_synced()`, the same as DMR.
* The quick menu has nothing NXDN-specific to add: this is a receive-only mode
  with one frequency.

The squelch is `nxdnPhy_synced()`: audio opens while a frame has decoded in the
last 300 ms, and the playback thread releases the speaker after a second of
silence, so the tail of a transmission is not cut.

## 8. Files

```
openrtx/include/protocols/NXDN/
    NxdnDefs.h    constants, frame layout, FSW, scrambler mask, LICH fields
    NxdnFec.h     descrambler, LICH parity, SACCH block (hard and soft), VCH
                  -> AMBE, L3 parse
    NxdnPhy.h     streaming demodulator interface
    NxdnSpeech.h  voice queue and playback thread
    NxdnStatus.h  what the UI shows
openrtx/src/protocols/NXDN/
    NxdnDefs.c  NxdnFec.c  NxdnPhy.c  NxdnSpeech.c  NxdnStatus.c
openrtx/include/protocols/Fsk4Tx.h   the transmit pulse, RRC or RRC x sinc
openrtx/src/protocols/Fsk4Tx.c
openrtx/include/rtx/OpMode_NXDN.hpp
openrtx/src/rtx/OpMode_NXDN.cpp
tests/platform/c62_nxdn_host.c        host harness (PHY + vocoder + PCM out)
tests/platform/c62_nxdn_bench.c       sensitivity bench: Eb/N0 sweep, noise WAVs
tests/platform/c62_nxdn_gen.c         synthetic call from this tree's TX
tests/platform/c62_nxdn_phy_cost.c    bare-metal cost harness (QEMU mps2-an505)
tests/platform/run_nxdn_phy_cost.sh   builds and runs it, skips without the tools
tests/unit/nxdn_rx.cpp                the three receive assertions (meson test)
tools/nxdn_rx_pulse.py                the H D and H sinc tables, by quadrature
tools/nxdn_ref_compare.py             dsd-neo over the same WAVs
```

`DmrDefs.c` is compiled into the NXDN build as well: it holds the AMBE+2 2450
dibit schedule that both modes share.

## 9. Sources

* NXDN Forum, **TS 1-A v1.3** (Common Air Interface), Nov 2011 — frame and
  superframe structure (4.1.2, 4.3, 4.4), FSW and preamble (4.4.3–4.4.4),
  channel coding (4.5), scrambler (4.6), LICH (5.2.1).
  <https://www.qsl.net/kb9mwr/projects/dv/nxdn/NXDN-TS-1-A_v0103.pdf>
* NXDN Forum, **TS 1-B v1.3** (Basic Operation) — superframe structure, VCH
  allocation at 4800/9600 bit/s and the EHR/EFR vocoder split (4.1.1, 4.1.2).
  <https://www.qsl.net/kb9mwr/projects/dv/nxdn/NXDN-TS-1-B_v0103.pdf>
* NXDN Forum, **TS 1-C v1.3** (Call Control) — Short Data Call (§12),
  VCALL/VCALL_ASSGN/TX_REL/DISC procedures.
  <https://www.qsl.net/kb9mwr/projects/dv/nxdn/NXDN-TS-1-C_v0103.pdf>
* NXDN Forum, **TS 1-E v1.1** (Common Air Interface, Type-D) — the Icom IDAS
  variant, RTCH2/SCCH framing.
* NXDN Forum white paper, *NXDN vs DMR*, for the deviation and channel figures.
* `dsd-neo` (GPL-3.0), <https://github.com/arancormonk/dsd-neo> — the public
  reference implementation this was checked against: `src/dsp/dsd_filters.c`
  (the inverse-sinc receive pulse), `src/protocol/nxdn/nxdn_{voice,frame,
  deperm,convolution,lfsr,descramble}.c` (VCH mapping, SACCH block, scrambler
  seed, layer 3 layout), `nxdn_const.h` (the interleaver permutations). No code
  was copied: the tables here were re-derived from the specification and the
  results compared bit for bit.
* `dsd`/DSD-FME, OP25 (`rx_sync.cc`, `nxdn` encoder) and SDRTrunk's
  `layer2/LICH.java`, which independently use the same VCH→AMBE+2 schedule,
  the same FSW and the same LICH tables.
* Sigidwiki, *NXDN*, for the captures and the published symbol/deviation
  table: <https://www.sigidwiki.com/wiki/NXDN>
