/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Public interface of the RNNoise stage on the DSP.
 *
 * A 10 ms frame at 48 kHz is 480 int16 samples; the caller owns the state
 * buffer (ns_dsp_state_bytes() of them, 8-byte aligned) and calls
 * ns_dsp_process() in place on every frame of the receive path.
 */

#ifndef NS_DSP_H
#define NS_DSP_H

#include <stdint.h>

#define NS_DSP_FRAME 480            /* samples per 10 ms frame, 48 kHz */

/** Bytes of state the caller must reserve, 8-byte aligned. */
int ns_dsp_state_bytes(void);

/** Initialise a state buffer of `bytes` bytes.  0 on success. */
int ns_dsp_init(void *state, unsigned bytes);

/** Denoise one frame in place.  `block` holds NS_DSP_FRAME samples. */
int ns_dsp_process(void *state, int16_t *block);

#endif /* NS_DSP_H */
