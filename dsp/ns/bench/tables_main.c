/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Dumps the tables lib/rnnoise builds at init time (check_init() in
 * denoise.c and the kiss_fft state) so the same probe can be run on the M33
 * and on the HiFi4 and the two compared.
 *
 * These tables come out of double-precision sin()/cos() in the C library, and
 * the two targets do not share a libm.  If the tables differ, every sample of
 * every frame differs, so this is the first thing to check before blaming the
 * per-frame arithmetic of the port.
 *
 * `common` is a file-scope object in denoise.c (not static), so its layout can
 * be mirrored here without touching the library.
 */

#include <stdint.h>
#include <stdio.h>

#include "rnnoise.h"
#include "kiss_fft.h"

#ifdef NS_RDIMON
extern void initialise_monitor_handles(void);
#endif

#define FRAME_SIZE 480
#define WINDOW_SIZE (2 * FRAME_SIZE)
#define NB_BANDS 22
#define FREQ_SIZE (FRAME_SIZE + 1)

struct ns_common_mirror {
    int init;
    kiss_fft_state *kfft;
    float half_window[FRAME_SIZE];
    float dct_table[NB_BANDS * NB_BANDS];
};

extern struct ns_common_mirror common;

static void put1(const char *tag, float v)
{
    union { float f; uint32_t u; } x;
    x.f = v;
    printf("%s %08x\n", tag, x.u);
}

int main(void)
{
#ifdef NS_RDIMON
    initialise_monitor_handles();
#endif
    static unsigned char state[64 * 1024] __attribute__((aligned(8)));
    static float in[FRAME_SIZE], out[FRAME_SIZE];
    DenoiseState *st = (DenoiseState *)state;
    rnnoise_init(st, NULL);
    for (int i = 0; i < FRAME_SIZE; i++)
        in[i] = (float)((i * 37) % 1000) - 500.0f;
    rnnoise_process_frame(st, out, in);   /* forces check_init() */

    printf("init %d\n", common.init);
    for (int i = 0; i < FRAME_SIZE; i++)
        put1("hw", common.half_window[i]);
    for (int i = 0; i < NB_BANDS * NB_BANDS; i++)
        put1("dct", common.dct_table[i]);

    const kiss_fft_state *k = common.kfft;
    printf("nfft %d shift %d\n", k->nfft, k->shift);
    put1("scale", (float)k->scale);
    for (int i = 0; i < 2 * MAXFACTORS; i++)
        printf("fac %d\n", k->factors[i]);
    for (int i = 0; i < k->nfft; i++)
        printf("brv %d\n", k->bitrev[i]);
    for (int i = 0; i < k->nfft; i++) {
        put1("twr", k->twiddles[i].r);
        put1("twi", k->twiddles[i].i);
    }
    return 0;
}
