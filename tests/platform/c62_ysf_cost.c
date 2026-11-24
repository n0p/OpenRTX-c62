/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the System Fusion receive chain: bare metal on QEMU
 * (mps2-an505, Cortex-M33), one frame marker per second of discriminator
 * audio, instructions counted by the TCG plugin of work/dsp-probe.
 *
 * It runs ysfPhy_push() with the frame decoder as the callback, so what it
 * measures is PHY + FICH + data channel + voice unpacking: everything the RTX
 * thread does per frame except the vocoder, which runs in the playback thread
 * and is already costed at 113 370 instructions per 20 ms frame.
 *
 * Build (from the repo root, RTX = this worktree):
 *
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -DMARK_FRAMES \
 *     -DWAV_PATH='"/tmp/cost.wav"' -Iopenrtx/include -Ilib/dmr_vocoder \
 *     -DCONFIG_YSF tests/platform/c62_ysf_cost.c \
 *     openrtx/src/protocols/YSF/YsfPhy.c openrtx/src/protocols/YSF/YsfFrame.c \
 *     lib/dmr_vocoder/dmr_decoder.c lib/dmr_vocoder/dmr_fft.c \
 *     -Wl,--gc-sections -T ../dsp-probe/common/mps2.ld \
 *     -specs=rdimon.specs -lrdimon -lm -o ysf.elf
 *
 *   INSNHIST_MARKER=$(arm-none-eabi-nm ysf.elf | awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel ysf.elf \
 *     -plugin file=../dsp-probe/common/libinsnhist.so -d plugin
 *
 * The WAV is 48 kHz mono 16-bit; fseek(44) skips the header.
 * The harness needs the vocoder only for dmr_golay24_correct(), which lives in
 * dmr_decoder.c next to the syndrome table.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/YSF/YsfFrame.h"
#include "protocols/YSF/YsfPhy.h"

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
static unsigned fichOk;
static unsigned voice;
static volatile uint32_t sink;

static void onFrame(const struct ysfFrame *f, void *user)
{
    (void)user;

    frames++;

    const struct ysfDecoded *d = ysfFrame_decode(f);

    if (d == NULL)
        return;

    if (d->fichOk)
        fichOk++;
    voice += d->voiceCount;
    sink += f->dibits[0] + f->evm + d->voiceCount;
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
        printf("usage: c62_ysf_cost.elf capture.wav\n");
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return 1;
    }
    fseek(f, 44, SEEK_SET);
    size_t n = fread(audio, sizeof(int16_t), MAX_SAMPLES, f);
    fclose(f);

    ysfPhy_init();
    ysfFrame_reset();

    /* The audio stream hands out 4800-sample halves: one frame each */
    const size_t block = 4800;
    size_t done = 0;
    int seconds = 0;

    for (size_t off = 0; off + block <= n; off += block) {
        ysfPhy_push(&audio[off], block, onFrame, NULL);
        if (((off / block) % 10) == 9) {
            MARK();
            seconds++;
        }
        done = off + block;
    }

    printf("ysf cost: %.2f s audio, %d markers, %u frames (%u FICH ok), "
           "%u voice slots, sink %lu\n", (double)done / 48000.0, seconds,
           frames, fichOk, voice, (unsigned long)sink);
    return 0;
}
