/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the D-STAR transmit chain: the bit generator (header,
 * frames, slow data, resync words) and the GMSK shaper, per second of
 * modulating waveform, on QEMU's mps2-an505 (Cortex M33).
 *
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -DMARK_FRAMES \
 *     -I$RTX/openrtx/include tests/platform/c62_dstar_tx_cost.c \
 *     $RTX/openrtx/src/protocols/DSTAR/{DStarTx,DStarFec}.c startup.o \
 *     -Wl,--gc-sections -T mps2.ld -specs=rdimon.specs -lrdimon -lm -o txcost.elf
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocols/DSTAR/DStarTx.h"

extern void initialise_monitor_handles(void);

#ifdef MARK_FRAMES
__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()
#else
#define MARK() do { } while (0)
#endif

static int16_t out[1440];
static volatile uint32_t sink;

int main(void)
{
    initialise_monitor_handles();

    struct dstarTxConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.hdr.flags[0] = DSTAR_FLAG1_REPEATER;
    strcpy(cfg.hdr.rpt1, "ED1ZAR B");
    strcpy(cfg.hdr.rpt2, "ED1ZAR G");
    strcpy(cfg.hdr.ur, "CQCQCQ");
    strcpy(cfg.hdr.my, "EA5TEST");
    strcpy(cfg.hdr.sfx, "C62");
    cfg.text = "COSTE DE TRANSMISION";
    cfg.repeatHeader = true;
    cfg.useNullVoice = true;

    dstarTx_begin(&cfg);

    /* 1 s = 50 blocks of 30 ms, one marker per second, five seconds */
    for (int sec = 0; sec < 5; sec++) {
        for (int b = 0; b < 50; b++) {
            dstarTx_samples(out, 1440, 24000);
            sink += (uint32_t)out[0] + (uint32_t)out[1439];
        }
        MARK();
    }

    printf("dstar tx cost done, sink %lu\n", (unsigned long)sink);
    return 0;
}
