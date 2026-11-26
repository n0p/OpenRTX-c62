# RNNoise evaluation, reproducible

The numbers in `docs/NOISE.md` come from these two pieces. Both run on a Linux
host; neither needs the radio.

`rn_demo.c` is the stage exactly as the firmware uses it: 48 kHz mono `s16` raw
in, raw out, 480-sample frames, `lib/rnnoise` as built for the target.

```
gcc -O2 -I../../lib/rnnoise/include -I../../lib/rnnoise/src -o rn_demo rn_demo.c \
    ../../lib/rnnoise/src/{denoise,kiss_fft,pitch,celt_lpc,rnn,rnn_data}.c -lm
```

`evaluate.py` builds the material and scores the stage:

* **speech**: LibriSpeech `dev-clean`, six utterances, 48 kHz, −20 dBFS
  (downloaded with the Hugging Face datasets-server API, see the script header)
* **engine noise**: `freesound` idle-engine recording (CC0), looped with 30 ms
  crossfades
* **airband noise**: synthesised (250–3400 Hz band-limited, pink tilt, Poisson
  static crashes at 3/s) — the script says so, and it should be replaced by a
  real recording when one is available
* **metrics**: PESQ-WB (`pesq`), STOI (`pystoi`), segmental SNR, noise floor on
  speech pauses, speech-level change, per-frame gain dispersion (pumping), and
  the input/output delay by cross-correlation

```
python3 -m venv venv && venv/bin/pip install pesq pystoi numpy scipy
venv/bin/python evaluate.py            # writes quality.json
```

`quality.json` next to this file is the run quoted in `docs/NOISE.md`
(2026‑11‑24), so a change in the library or in the stage can be diffed against
it instead of re-deriving the whole table.

The per-frame instruction counts come from `work/nr-probe/measure.py` (QEMU
`mps2-an505` + the TCG counter plugin): it builds the same library for
`-mcpu=cortex-m33 -mfpu=fpv5-sp-d16`, runs 20 frames after 10 warm-up frames and
reports min/median/max instructions per frame and an estimated cycle count.
