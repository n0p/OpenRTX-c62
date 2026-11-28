/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/noise_suppress.h"
#include "rnnoise.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(noise_suppress, LOG_LEVEL_INF);

/*
 * RNNoise v0.1.1 (BSD-3-Clause, see lib/rnnoise) on the 48 kHz analog receive
 * path. One DenoiseState is 62 KiB with the local "no 35 KiB stack" patch
 * applied, which is why it lives in PSRAM: it must not touch the internal RAM
 * budget (see docs/NOISE.md for the measured flash/RAM figures).
 *
 * The state is a single static block so that enabling the stage cannot fail at
 * run time for lack of heap; it is only initialised when the user turns the
 * feature on, and rnnoise_init() zeroes it then.
 */
static struct {
    bool enabled;                   ///< Setting is on
    bool ready;                     ///< DenoiseState initialised
    DenoiseState *state;            ///< RNNoise state, inside `buf`
    size_t stateSize;               ///< Bytes rnnoise_get_size() asked for
    float in[NOISE_SUPPRESS_BLOCK]; ///< int16 -> float conversion
    float out[NOISE_SUPPRESS_BLOCK];
    unsigned char buf[64 * 1024] __attribute__((aligned(8))); ///< DenoiseState
} ns __attribute__((section(".psram_section")));

/* Measured on this library build: 1 360 819 instructions per 10 ms frame of
 * the v0.1.1 network at -O2 -fsingle-precision-constant on the Cortex-M33
 * (median of 19 frames, QEMU mps2-an505 with the TCG instruction plugin;
 * work/nr-probe, docs/NOISE.md "Cost after the float patch").  That is 45 % of
 * a 300 MHz core in instructions and 54 % in estimated cycles, 5.4 ms of CPU
 * for 10 ms of audio. */
#define NS_INSTR_PER_FRAME 1360819

bool noiseSuppress_enabled(void)
{
    return ns.enabled && ns.ready;
}

bool noiseSuppress_setEnabled(bool enabled)
{
    if (!enabled) {
        ns.enabled = false;
        return false;
    }

    if (ns.ready == false) {
        const int size = rnnoise_get_size();
        ns.stateSize = (size_t)size;
        if (size <= 0 || (size_t)size > sizeof(ns.buf)) {
            LOG_ERR("RNNoise state %d B does not fit %u B", size,
                    (unsigned)sizeof(ns.buf));
            ns.enabled = false;
            return false;
        }
        ns.state = (DenoiseState *)ns.buf;
        if (rnnoise_init(ns.state, NULL) != 0) {
            LOG_ERR("RNNoise init failed");
            ns.enabled = false;
            return false;
        }
        ns.ready = true;
        LOG_INF("RNNoise enabled, %u B state, %d instr/frame",
                (unsigned)ns.stateSize, NS_INSTR_PER_FRAME);
    }

    ns.enabled = true;
    return true;
}

void noiseSuppress_init(void)
{
    /* The settings decide; nothing is allocated until the feature is used. */
    ns.enabled = false;
    ns.ready = false;
}

bool noiseSuppress_process(int16_t *block, size_t count)
{
    if ((ns.enabled == false) || (ns.ready == false))
        return false;

    if (count != NOISE_SUPPRESS_BLOCK)
        return false;

    for (size_t i = 0; i < NOISE_SUPPRESS_BLOCK; i++)
        ns.in[i] = (float)block[i];

    rnnoise_process_frame(ns.state, ns.out, ns.in);

    for (size_t i = 0; i < NOISE_SUPPRESS_BLOCK; i++) {
        float v = ns.out[i];
        if (v > 32767.0f)
            v = 32767.0f;
        if (v < -32768.0f)
            v = -32768.0f;
        block[i] = (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }

    return true;
}

int32_t noiseSuppress_costEstimate(void)
{
    return NS_INSTR_PER_FRAME;
}
