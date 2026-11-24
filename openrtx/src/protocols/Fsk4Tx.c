/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file Fsk4Tx.c
 * @brief The 4FSK baseband of the 2400 symbols/s modes, see Fsk4Tx.h.
 */

#include <math.h>
#include <string.h>

#include "protocols/Fsk4Tx.h"

void fsk4Mod_init(fsk4_mod_t *mod, int sps, float gain, bool invert)
{
    const float pi = 3.14159265358979f;
    const float a = 0.2f;
    float sum = 0.0f;

    if (sps < 2)
        sps = 2;

    mod->sps = sps;
    mod->tapsN = 8 * sps + 1;   /* four symbols of span each way */
    if (mod->tapsN > FSK4_TX_MAX_TAPS)
        mod->tapsN = FSK4_TX_MAX_TAPS;

    /* Root raised cosine, alpha 0.2, the receiver's matched filter */
    for (int i = 0; i < mod->tapsN; i++) {
        const float t = ((float)i - (float)(mod->tapsN - 1) / 2.0f)
                      / (float)sps;
        float h;

        if (fabsf(t) < 1e-6f) {
            h = 1.0f - a + 4.0f * a / pi;
        } else if (fabsf(fabsf(t) - 1.0f / (4.0f * a)) < 1e-6f) {
            h = a / sqrtf(2.0f)
              * ((1.0f + 2.0f / pi) * sinf(pi / (4.0f * a))
                 + (1.0f - 2.0f / pi) * cosf(pi / (4.0f * a)));
        } else {
            h = (sinf(pi * t * (1.0f - a))
                 + 4.0f * a * t * cosf(pi * t * (1.0f + a)))
              / (pi * t * (1.0f - (4.0f * a * t) * (4.0f * a * t)));
        }
        mod->taps[i] = h;
        sum += h;
    }

    /*
     * One symbol every sps samples, so the DC gain of the polyphase bank is
     * sum / sps per symbol value: scale to the requested gain, which is the
     * deviation of a run of +1.
     */
    for (int i = 0; i < mod->tapsN; i++)
        mod->taps[i] *= gain * (float)sps / sum;

    memset(mod->hist, 0, sizeof(mod->hist));
    mod->histPos = 0;
    mod->gain = gain;
    mod->invert = invert;
}

void fsk4Mod_symbols(fsk4_mod_t *mod, const float *sym, int n, int16_t *out)
{
    const int sps = mod->sps;
    const int span = (mod->tapsN + sps - 1) / sps;

    for (int k = 0; k < n; k++) {
        mod->histPos = (mod->histPos + 1) & 15u;
        mod->hist[mod->histPos] = sym ? sym[k] : 0.0f;

        for (int p = 0; p < sps; p++) {
            /* Zero stuffed symbols through the filter: only one tap in sps
             * meets a symbol */
            float y = 0.0f;

            for (int j = 0; j < span; j++) {
                const int t = p + sps * j;

                if (t >= mod->tapsN)
                    break;
                y += mod->taps[t] * mod->hist[(mod->histPos - j) & 15u];
            }

            if (mod->invert)
                y = -y;
            if (y > 32767.0f)
                y = 32767.0f;
            if (y < -32768.0f)
                y = -32768.0f;
            out[k * sps + p] = (int16_t)y;
        }
    }
}

void fsk4Mod_dibits(fsk4_mod_t *mod, const uint8_t *dibits, const float map[4],
                    int n, int16_t *out)
{
    const int sps = mod->sps;
    const int span = (mod->tapsN + sps - 1) / sps;

    for (int k = 0; k < n; k++) {
        mod->histPos = (mod->histPos + 1) & 15u;
        mod->hist[mod->histPos] = dibits ? map[dibits[k] & 3u] : 0.0f;

        for (int p = 0; p < sps; p++) {
            float y = 0.0f;

            for (int j = 0; j < span; j++) {
                const int t = p + sps * j;

                if (t >= mod->tapsN)
                    break;
                y += mod->taps[t] * mod->hist[(mod->histPos - j) & 15u];
            }

            if (mod->invert)
                y = -y;
            if (y > 32767.0f)
                y = 32767.0f;
            if (y < -32768.0f)
                y = -32768.0f;
            out[k * sps + p] = (int16_t)y;
        }
    }
}

void fsk4Mod_levels(fsk4_mod_t *mod, const int8_t *levels, int n, int16_t *out)
{
    const int sps = mod->sps;
    const int span = (mod->tapsN + sps - 1) / sps;

    for (int k = 0; k < n; k++) {
        mod->histPos = (mod->histPos + 1) & 15u;
        mod->hist[mod->histPos] = levels ? (float)levels[k] : 0.0f;

        for (int p = 0; p < sps; p++) {
            float y = 0.0f;

            for (int j = 0; j < span; j++) {
                const int t = p + sps * j;

                if (t >= mod->tapsN)
                    break;
                y += mod->taps[t] * mod->hist[(mod->histPos - j) & 15u];
            }

            if (mod->invert)
                y = -y;
            if (y > 32767.0f)
                y = 32767.0f;
            if (y < -32768.0f)
                y = -32768.0f;
            out[k * sps + p] = (int16_t)y;
        }
    }
}
