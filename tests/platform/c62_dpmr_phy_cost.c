/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the dPMR receive PHY: bare metal on QEMU (mps2-an505,
 * Cortex M33), one frame marker per second of discriminator audio,
 * instructions counted by the TCG plugin of work/dsp-probe/common/insnframe.c.
 *
 * It runs only dpmrPhy_push(), so what it measures is the demodulator: the
 * 161 tap matched filter at 48 kHz decimated to 9.6 kHz, the sync search, the
 * slicer and the CCH/HI/END FEC -- no vocoder, no audio interface.
 *
 * tests/platform/run_dpmr_phy_cost.sh does the three commands below and
 * reports the per-second medians; it is registered in the Meson build and
 * skips (exit 77) on a machine without the cross toolchain or QEMU.
 *
 *   arm-none-eabi-gcc -c -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffp-contract=off -ffunction-sections \
 *     -fdata-sections -DMARK_FRAMES -DWAV_PATH='"/tmp/dpmr.dis"' \
 *     -I$RTX/openrtx/include tests/platform/c62_dpmr_phy_cost.c
 *   ... the same for openrtx/src/protocols/DPMR/{DpmrPhy,DpmrDefs,DpmrData}.c
 *   arm-none-eabi-g++ -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -Wl,--gc-sections -T $DSP/common/mps2.ld \
 *     -specs=rdimon.specs -o dpmrphy.elf $DSP/common/startup.o <objects> \
 *     -lrdimon -lm -lc
 *   INSNHIST_MARKER=$(arm-none-eabi-nm dpmrphy.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel dpmrphy.elf \
 *     -plugin file=$DSP/common/libinsnframe.so -d plugin
 *
 * The input is the discriminator output: S16LE at 48 kS/s, either raw (the
 * DSDcc .dis files) or a WAV, whose 44 byte header is skipped.  The buffer
 * holds 20 s, which is the window the figures of docs/RX_DPMR.md are taken
 * over; a longer file is read up to there, and the harness says so.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/DPMR/DpmrDefs.h"
#include "protocols/DPMR/DpmrPhy.h"

extern void initialise_monitor_handles(void);

#ifdef MARK_FRAMES
__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()
#else
#define MARK() do { } while (0)
#endif

#define MAX_SAMPLES (48000 * 20)

static int16_t audio[MAX_SAMPLES];
static unsigned frames;
static unsigned voice;
static unsigned headers;
static unsigned ends;
static volatile uint32_t sink;

static void onFrame(const struct dpmrFrame *f, void *user)
{
    (void)user;
    frames++;
    if (f->voice) {
        voice++;
        sink += f->ambe[0][0] + f->cch.fn;
    } else if (f->headerOk) {
        headers++;
        sink += f->header.ht;
    } else if (f->endOk) {
        ends++;
        sink += f->end.status;
    }
}

int main(int argc, char **argv)
{
    initialise_monitor_handles();

#ifdef WAV_PATH
    const char *path = WAV_PATH;
    (void)argc;
    (void)argv;
#else
    const char *path = (argc > 1) ? argv[1] : NULL;
#endif

    if (path == NULL) {
        printf("usage: c62_dpmr_phy_cost.elf capture.dis\n");
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return 1;
    }

    char magic[4];
    if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "RIFF", 4) == 0)
        fseek(f, 44, SEEK_SET);
    else
        fseek(f, 0, SEEK_SET);

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t n = fread(audio, sizeof(int16_t), MAX_SAMPLES, f);
    fclose(f);

    if (total > 0 && (size_t)total > MAX_SAMPLES * sizeof(int16_t))
        printf("reading the first %d s of %ld s: the rest is not measured\n",
               (int)(MAX_SAMPLES / 48000), total / (48000 * 2));

    dpmrPhy_init();

    const size_t block = 480;   /* 10 ms */
    int seconds = 0;

    for (size_t off = 0; off + block <= n; off += block) {
        dpmrPhy_push(&audio[off], block, onFrame, NULL);
        if (((off / block) % 100) == 99) {
            MARK();
            seconds++;
        }
    }

    struct dpmrPhyStats st;
    dpmrPhy_stats(&st);

    printf("phy cost: %.2f s audio, %d markers, %u frames (%u voice, %u headers,"
           " %u ends), cch %u/%u, sink %lu\n",
           (double)n / 48000.0, seconds, frames, voice, headers, ends,
           st.cchOk, st.cchOk + st.cchFail, (unsigned long)sink);

    /* A capture that stops decoding must fail the runner, not just print */
    return frames > 0 && st.cchOk > 0 ? 0 : 1;
}
