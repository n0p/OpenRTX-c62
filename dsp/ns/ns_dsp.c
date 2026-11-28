/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RNNoise noise-reduction stage as the DSP service will call it.
 *
 * This is the body of the LSF audio service that dsp-openfw will register next
 * to playback and capture: 480 int16 samples in, the same 480 samples out,
 * in place, one 10 ms frame at 48 kHz.  It owns no memory of its own: the
 * caller passes a state buffer (the DenoiseState, 62 312 B with the C62
 * patches), which is why the service can allocate it once and never fail at
 * run time.
 *
 * The int16 -> float -> int16 conversion is the one the M33 firmware does in
 * openrtx/src/core/noise_suppress.c, rounding half away from zero, so a frame
 * that goes through this function and a frame that goes through the firmware
 * path end up with the same bytes.
 */

#include <stdint.h>
#include <stddef.h>

#include "rnnoise.h"
#include "ns_dsp.h"

int ns_dsp_state_bytes(void)
{
    return rnnoise_get_size();
}

int ns_dsp_init(void *state, unsigned bytes)
{
    if (state == NULL || bytes < (unsigned)rnnoise_get_size())
        return -1;
    if (rnnoise_get_frame_size() != NS_DSP_FRAME)
        return -1;
    return rnnoise_init((DenoiseState *)state, NULL);
}

int ns_dsp_process(void *state, int16_t *block)
{
    float in[NS_DSP_FRAME];
    float out[NS_DSP_FRAME];
    int i;

    if (state == NULL || block == NULL)
        return -1;

    for (i = 0; i < NS_DSP_FRAME; i++)
        in[i] = (float)block[i];

    rnnoise_process_frame((DenoiseState *)state, out, in);

    for (i = 0; i < NS_DSP_FRAME; i++) {
        float v = out[i];
        if (v > 32767.0f)
            v = 32767.0f;
        if (v < -32768.0f)
            v = -32768.0f;
        block[i] = (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }
    return 0;
}
