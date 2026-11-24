/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the DMR receive PHY: bare metal on QEMU (mps2-an505, Cortex
 * M33), one frame marker per second of discriminator audio, instructions
 * counted by the TCG plugin of work/dsp-probe/common/insnframe.c.
 *
 * It runs only dmrPhy_push(), so what it measures is the demodulator: the
 * matched filter, the scan and the FEC of the burst -- no vocoder, no audio
 * interface.
 *
 *   RTX=../OpenRTX-integ
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -DMARK_FRAMES \
 *     -DWAV_PATH='"/tmp/cost.wav"' -I$RTX/openrtx/include \
 *     tests/platform/c62_dmr_phy_cost.c \
 *     $RTX/openrtx/src/protocols/DMR/{DmrPhy,DmrDefs,DmrFec}.c \
 *     -ffunction-sections -Wl,--gc-sections -T work/dsp-probe/common/mps2.ld \
 *     -specs=rdimon.specs $(...) -lrdimon -lm -o dmrphy.elf
 *   INSNHIST_MARKER=$(arm-none-eabi-nm dmrphy.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel dmrphy.elf \
 *     -plugin file=work/dsp-probe/common/libinsnframe.so -d plugin
 *
 * The harness reads WAV_PATH through semihosting (newlib + rdimon).  The
 * second line of the histogram holds the running total at each marker, so the
 * difference between two consecutive markers is one second of audio.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/DMR/DmrPhy.h"

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
static unsigned bursts;
static unsigned voices;
static unsigned data;
static volatile uint32_t sink;

static void onBurst(const struct dmrBurst *b, void *user)
{
    (void)user;
    bursts++;
    if (b->voice) {
        voices++;
        sink += b->frame[0][0] + b->frameCount;
    } else {
        data++;
        sink += b->sync + b->crcOk;
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
        printf("usage: c62_dmr_phy_cost.elf capture.wav\n");
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

    dmrPhy_init();

    const size_t block = 480; /* 10 ms */
    size_t done = 0;
    int seconds = 0;

    for (size_t off = 0; off + block <= n; off += block) {
        dmrPhy_push(&audio[off], block, onBurst, NULL);
        if (((off / block) % 100) == 99) {
            MARK();
            seconds++;
        }
    }

    printf("phy cost: %.2f s audio, %d markers, %u bursts (%u voice, %u data), "
           "sink %lu\n", (double)n / 48000.0, seconds, bursts, voices, data,
           (unsigned long)sink);
    return 0;
}
