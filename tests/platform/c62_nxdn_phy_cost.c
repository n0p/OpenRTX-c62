/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the NXDN receive PHY: bare metal on QEMU (mps2-an505,
 * Cortex M33), one frame marker per second of discriminator audio,
 * instructions counted by the TCG plugin of work/dsp-probe/common/insnframe.c.
 *
 * It runs only nxdnPhy_push(), so what it measures is the demodulator: the
 * matched filter, the scan, the timing search, the four-level fit, the
 * descrambler, the SACCH Viterbi and the voice-frame mapping -- no vocoder, no
 * audio interface.
 *
 *   RTX=../OpenRTX-integ
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -DMARK_FRAMES \
 *     -DWAV_PATH='"/tmp/cost.wav"' -I$RTX/openrtx/include \
 *     tests/platform/c62_nxdn_phy_cost.c \
 *     $RTX/openrtx/src/protocols/NXDN/{NxdnPhy,NxdnDefs,NxdnFec}.c \
 *     $RTX/openrtx/src/protocols/DMR/DmrDefs.c \
 *     -Wl,--gc-sections -T work/dsp-probe/common/mps2.ld \
 *     -specs=rdimon.specs -lrdimon -lm -o nxdnphy.elf
 *   INSNHIST_MARKER=$(arm-none-eabi-nm nxdnphy.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel nxdnphy.elf \
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

#include "protocols/NXDN/NxdnPhy.h"

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
static unsigned voices;
static unsigned ambe;
static unsigned sacch;
static volatile uint32_t sink;

static void onFrame(const struct nxdnFrame *f, void *user)
{
    (void)user;
    frames++;
    if (f->frameCount > 0)
        voices++;
    ambe += f->frameCount;
    if (f->sacchOk)
        sacch++;
    sink += f->frame[0][0] + f->frameCount + f->lich.raw;
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
        printf("usage: c62_nxdn_phy_cost.elf capture.wav\n");
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

    nxdnPhy_init();

    const size_t block = 480; /* 10 ms */
    int seconds = 0;

    for (size_t off = 0; off + block <= n; off += block) {
        nxdnPhy_push(&audio[off], block, onFrame, NULL);
        if (((off / block) % 100) == 99) {
            MARK();
            seconds++;
        }
    }

    printf("phy cost: %.2f s audio, %d markers, %u frames (%u voice), "
           "%u AMBE frames, %u SACCH ok, sink %lu\n",
           (double)n / 48000.0, seconds, frames, voices, ambe, sacch,
           (unsigned long)sink);
    return 0;
}
