# Per-mode receive and transmit statistics

Every operating mode used to keep its own diagnostics: the TETRA mode has a
`tetraDiag`, DMR fills a `dmrStatus`, M17 has its lock flag and the transmit
guard counts blocks for all of them. Answering the same question of two modes
meant reading two different things, and there was no number at all for the
one question that decides whether a mode is usable on this radio: **how much
of a 30 ms block does demodulating it cost**.

`core/mode_stats` is that one table, per mode, with the same fields in the
same order for all of them:

| | FM | M17 | DMR | TETRA |
|---|---|---|---|---|
| blocks | polls of the mode thread | baseband blocks | baseband blocks | baseband blocks |
| frames | – | frames demodulated | bursts decoded | speech frames |
| valid | – | LSF decoded | CRC of a data burst | TMO block CRC |
| bad CRC | – | LSF not decoded | CRC failed | speech frame BFI |
| lost | – | – | blocks the receiver never got | blocks the receiver never got |
| resyncs | – | lock acquisitions | PHY restarts | burst sync restarts |
| level | – (no samples in the CPU) | in | in | in |
| RSSI | peak | peak | peak | peak |
| quality | – | soft-bit confidence | burst EVM | phase error |
| timing | – | clock recovery point | fractional timing of the burst | timing loop drift |
| cost | per poll | per block | per block | per block |
| TX | keys, first block | keys, first block, starved, gaps | keys, first block, starved, gaps | – (RX only) |

## The option

`C62_MODE_STATS`, off by default, defines `CONFIG_C62_MODE_STATS`:

```
west build -b c62 -d build_stats . -- -DC62_MODE_STATS=ON \
    -DC62_REMOTE=ON -DC62_BASEBAND_DUMP=ON
```

With the option off the hooks expand to `((void)sizeof(x))`: no call, no
argument evaluation, no symbol. `tests/mode_stats_off.sh` compiles a probe that
calls all of them and checks that the object references nothing of the layer
and that the function is a single return instruction.

On the firmware the same thing was checked against `master`: the image built
with the option off has the same symbol table (no symbol added, removed or
resized), the same `.text`, `.data` and `.bss` (574 524, 4 612 and 576 632
bytes) and the same FLASH and RAM figures. The only bytes that differ are the
embedded git version string and the order of the 31 linker veneers, 8-byte
trampolines no function's address depends on.

With the option on, a hook is a handful of integer adds on the thread that
already has the data, plus two reads of the cycle counter per block. There is
no `printf` anywhere near the audio or the receive path: the only formatted
output is the reply to the remote control command, produced on the remote
thread from the counters.

## Reading it

`rx` prints one line per compiled mode, over the same cable `scripts/c62_bbdump.py`
already uses (see `platform/drivers/audio/c62_remote.c`). `rx 0` clears every
counter. Field order and names are stable:

```
rx <mode> blk=<n> frm=<n> ok=<n> crc=<n> lost=<n> rs=<n> lvl=<mean>/<peak>
          rssi=<dBm> q=<‰> fit=<‰>/<‰> cost=<min>/<mean>/<max>us p=<%>
          tx=<keys> fst=<ms> late=<n> gap=<n>
```

- `blk` blocks of baseband processed (or polls of the mode thread, FM).
- `frm` frames, bursts or speech frames taken out of them.
- `ok` / `crc` of those, how many passed their check and how many failed it.
- `lost` blocks that never arrived (a stream overrun).
- `rs` resynchronisations: the receiver had to acquire again.
- `lvl` mean of the per-block mean level and the peak, in samples of 32768.
- `rssi` the strongest reading seen, dBm; `-` if none was reported.
- `q` the mode's quality figure in thousandths, lower is better: DMR EVM
  against the symbol levels, TETRA RMS phase error over pi/4, M17 distance of
  the soft bits from full confidence. `-` where the mode has no such figure.
- `fit` the timing the demodulator settled on and the step of the search or
  loop that found it, both in thousandths of a symbol (`DMR` 125 per timing
  phase, `M17` 200 per sample, `TETRA` 125 per phase of its 8-phase loop).
- `cost` microseconds of CPU to demodulate one block, minimum, mean and
  maximum; `p` the mean as a percentage of the 30 ms the block lasts.
- `tx` transmissions keyed, `fst` the milliseconds from the PTT to the first
  block that reached the modulator (last transmission), `late` frames that
  went out with no audio behind them, `gap` blocks more than two frame periods
  after the previous one.

A real line, as the formatter produces it on the host build (`tests/unit/mode_stats.cpp`
links the same `mode_stats.c`; the figures below come from a harness that feeds
it a plausible 60 s of TETRA, the format and the rounding are the radio's):

```
rx fm blk=1000 frm=0 ok=0 crc=0 lost=0 rs=0 lvl=0/0 rssi=-85 q=- fit=-/- cost=40/40/40us p=0.1% tx=0 fst=- late=0 gap=0
rx m17 blk=650 frm=304 ok=300 crc=0 lost=0 rs=1 lvl=438/900 rssi=-91 q=180 fit=400/200 cost=380/380/380us p=1.2% tx=2 fst=6ms late=1 gap=0
rx dmr blk=0 frm=0 ok=0 crc=0 lost=0 rs=0 lvl=0/0 rssi=- q=- fit=-/- cost=-/-/-us p=- tx=0 fst=- late=0 gap=0
rx tetra blk=1980 frm=660 ok=638 crc=22 lost=0 rs=7 lvl=1319/1440 rssi=-74 q=215 fit=250/125 cost=1100/1100/1100us p=3.6% tx=0 fst=- late=0 gap=0
```

Each line is one control packet, so it arrives between the sample packets of
the dump and `c62_bbdump.py` prints it as `radio: rx m17 ...`.

## Cost

Where the hooks are, and what each of them adds:

| hook | called from | per |
|---|---|---|
| `modeStats_blockStart` / `modeStats_block` | `Demodulator::update` (M17), `OpMode_DMR::rxState`, `OpMode_TETRA::update`, `OpMode_FM::update` | block |
| `modeStats_frames` / `valid` / `badCrc` / `lost` / `resync` | where the mode already decides it (DMR burst callback, TETRA CRC and speech counters, M17 frame decode, PHY resets) | frame |
| `modeStats_rssi` / `error` / `timing` | next to the figures the mode already computes | block or frame |
| `modeStats_txKey` / `txBlock` / `txStarve` / `txEnd` | next to the `txg_*` calls of the transmit guard, in M17, DMR and FM | transmission |

Measured on the C62 image, 300 MHz Cortex-M33, `C62_DMR_RX` + `C62_APRS` +
`DMR_TX`:

| build | FLASH | RAM | PSRAM |
|---|---|---|---|
| release, no remote control (master and this branch with the option off) | 579 140 B | 209 640 B | 379 064 B |
| + `C62_REMOTE` + `C62_BASEBAND_DUMP`, option off | 593 088 B | 216 296 B | 1 427 640 B |
| + `C62_REMOTE` + `C62_BASEBAND_DUMP`, option **on** | 595 580 B | 217 104 B | 1 427 640 B |

So the layer itself is **2 492 bytes of flash and 808 bytes of RAM**:

- 1 524 B of `mode_stats.c` (of which 376 B the formatter), 48 B the cycle
  counter, 16 B the mode names, 4 B the transmit cadence table, 16 B the two
  accessors added to the DMR PHY and the M17 demodulator, 48 B of format
  strings: **1 656 B** of new code and constants.
- **666 B** of hooks inlined in the modes: 160 B in the TETRA update, 194 B in
  M17 (receive and transmit), 192 B in DMR (burst callback, receive, the three
  transmit stages), 90 B in FM, 22 B in the M17 demodulator, 8 B in the TETRA
  resync.
- 80 B the `rx` command in the remote control and 90 B of assorted string
  literals.
- RAM: 544 B of `.bss` (`stats`, 136 B per mode), 250 B the line buffer of the
  remote command, 12 B the TETRA deltas, the rest alignment.

The 30 ms budget is what the percentage is relative to: at 300 MHz a block is
9 000 000 cycles, and the counter wraps every 14.3 s, so the subtraction is
32-bit and the wrap is never in the way (there is a test for it).

## Limits, on purpose

- A mode reports what it already knows. DMR does not count lost blocks (its
  PHY only reports the bursts it decoded) and FM has no level or quality
  figure: its audio never enters the CPU, it goes from the discriminator to the
  speaker inside the DSP. Those cells print a dash rather than a number
  invented by the layer.
- The counters are per mode and never overlap: nothing here is shared between
  two threads except the read of a line by the remote control, which can only
  be off by one block.
- If the build has no cycle counter (`DWT_CTRL.NOCYCCNT`), the cost columns
  read zero: a zero is honest, a number from another clock would not be.
- A line is one control packet of at most 250 bytes, which holds every counter
  at its maximum. Should a nonsense quality or cost figure ever overflow it,
  the mode prints `rx <mode> overflow` instead of nothing.
