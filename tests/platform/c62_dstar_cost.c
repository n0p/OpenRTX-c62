/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the D-STAR receive path: bare metal on QEMU (mps2-an505,
 * Cortex M33), one frame marker per 100 demodulated DV frames, instructions
 * counted by the TCG plugin of work/dsp-probe/common/insnframe.c.
 *
 * It runs the demodulator, the header Viterbi and the slow-data channel (the
 * message and position reassembly), i.e. everything the rtx thread does per
 * frame except the vocoder.  The input is a transmission made by the host
 * harness (tests/host/dstar/dstar_host tx), so the demodulator is locked and
 * decoding from the first frame: this is the steady-state cost, which is what
 * matters for the RAM/CPU budget.
 *
 *   RTX=../rx-dstar
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -DMARK_FRAMES \
 *     -DWAV_PATH='"/tmp/dstar.raw"' -I$RTX/openrtx/include \
 *     tests/platform/c62_dstar_cost.c \
 *     $RTX/openrtx/src/protocols/DSTAR/{DStarPhy,DStarFec,DStarSlowData}.c \
 *     -Wl,--gc-sections -T work/dsp-probe/common/mps2.ld -specs=rdimon.specs \
 *     -lrdimon -lm -o dstarcost.elf
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/DSTAR/DStarPhy.h"
#include "protocols/DSTAR/DStarSlowData.h"

extern void initialise_monitor_handles(void);

#ifdef MARK_FRAMES
__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()
#else
#define MARK() do { } while (0)
#endif

#define MAX_SAMPLES (48000 * 12)

static int16_t audio[MAX_SAMPLES];
static struct dstarSlow slow;
static unsigned headers;
static unsigned frames;
static unsigned resyncs;
static unsigned texts;
static unsigned sentences;
static volatile uint32_t sink;

static void onHeader(const struct dstarHeader *h, int syncErrors, void *user)
{
    (void)user;
    headers++;
    sink += (uint32_t)h->my[0] + (uint32_t)syncErrors;
}

static void onFrame(const struct dstarPhyFrame *f, void *user)
{
    (void)user;
    frames++;
    if (f->sync) {
        resyncs++;
        return;
    }
    dstarSlow_push(&slow, f->slow);
    const char *t = dstarSlow_text(&slow);
    if (t != NULL)
        texts++;
    const char *s = dstarSlow_sentence(&slow);
    if (s != NULL) {
        struct dstarFix fix;
        if (dstarFix_parse(s, &fix))
            sentences++;
    }
    sink += f->voice[0] + f->slow[0];
}

int main(void)
{
    initialise_monitor_handles();

    FILE *f = fopen(WAV_PATH, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", WAV_PATH);
        return 1;
    }
    size_t n = fread(audio, sizeof(int16_t), MAX_SAMPLES, f);
    fclose(f);

    dstarPhy_init();
    dstarSlow_reset(&slow);

    /* 30 ms blocks, like the audio thread: 50 frames per marker */
    const size_t block = 1440;
    int markers = 0;
    unsigned frameAtMark = 0;

    for (size_t off = 0; off + block <= n; off += block) {
        dstarPhy_push(&audio[off], block, onHeader, onFrame, NULL, NULL);
        if ((frames - frameAtMark) >= 50) {
            MARK();
            frameAtMark = frames;
            markers++;
        }
    }

    printf("dstar cost: %.2f s audio, %d markers, %u headers, %u frames, "
           "%u resyncs, %u texts, %u sentences, sink %lu\n",
           (double)n / 48000.0, markers, headers, frames, resyncs, texts,
           sentences, (unsigned long)sink);
    return 0;
}
