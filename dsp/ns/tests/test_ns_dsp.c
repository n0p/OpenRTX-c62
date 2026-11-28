/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * End-to-end check of the DSP service body: the 8 test frames go through
 * ns_dsp_process() as int16, exactly as the LSF audio service will hand them
 * over, and the 480 int16 of every output frame are printed as hex.  Compared
 * against the M33 harness dump converted to int16 (tools/cmp_int16.py), this
 * also covers the float -> int16 rounding, which the frame harness does not.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ns_dsp.h"
#include "../bench/pcm_frames.h"

#ifdef NS_RDIMON
extern void initialise_monitor_handles(void);
#endif

static unsigned char state[64 * 1024] __attribute__((aligned(8)));

int main(void)
{
#ifdef NS_RDIMON
    initialise_monitor_handles();
#endif
    if (ns_dsp_state_bytes() > (int)sizeof(state)) {
        printf("state too small\n");
        return 1;
    }
    if (ns_dsp_init(state, sizeof(state)) != 0) {
        printf("init failed\n");
        return 1;
    }

    /* Same sequence as the frame harness (bench/bench_main.c): two warm-up
     * frames, then eight measured ones, so the two dumps line up. */
    for (int k = 0; k < NS_TEST_FRAMES + 2; k++) {
        int16_t block[NS_DSP_FRAME];
        memcpy(block, ns_test_pcm[k % NS_TEST_FRAMES], sizeof(block));
        if (ns_dsp_process(state, block) != 0) {
            printf("process failed at %d\n", k);
            return 1;
        }
        if (k < 2)
            continue;
        printf("I%02d", k - 2);
        for (int i = 0; i < NS_DSP_FRAME; i++)
            printf(" %04x", (unsigned)(uint16_t)block[i]);
        printf("\n");
    }
    return 0;
}
