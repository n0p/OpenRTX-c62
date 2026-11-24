/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file C4fmMod.c
 * @brief 4-level C4FM modulator, see C4fmMod.h.
 *
 * The pulse is generated from the closed form of the root raised cosine, not
 * from a table: it costs 81 sin/cos calls once per transmission and keeps a
 * 324-byte table out of flash.  The symbol history is a 16-slot ring (the
 * pulse spans nine symbols) indexed with a mask, and only one tap in ten meets
 * a symbol, so a symbol costs 90 multiply-accumulates: 432 kMAC/s at 4800
 * symbols/s, which is noise next to the vocoders.
 */

#include "protocols/C4FM/C4fmMod.h"

#include <math.h>
#include <string.h>

void c4fmMod_init(c4fmMod_t *mod, float gain, bool invert)
{
    const float pi = 3.14159265358979f;
    const float a = 0.2f;
    float sum = 0.0f;

    for (int i = 0; i < C4FM_MOD_TAPS; i++)
    {
        const float t = ((float)i - (C4FM_MOD_TAPS - 1) / 2.0f) / C4FM_MOD_SPS;
        float h;

        if (fabsf(t) < 1e-6f)
        {
            h = 1.0f - a + 4.0f * a / pi;
        }
        else if (fabsf(fabsf(t) - 1.0f / (4.0f * a)) < 1e-6f)
        {
            h = a / sqrtf(2.0f)
              * ((1.0f + 2.0f / pi) * sinf(pi / (4.0f * a))
                 + (1.0f - 2.0f / pi) * cosf(pi / (4.0f * a)));
        }
        else
        {
            h = (sinf(pi * t * (1.0f - a))
                 + 4.0f * a * t * cosf(pi * t * (1.0f + a)))
              / (pi * t * (1.0f - (4.0f * a * t) * (4.0f * a * t)));
        }

        mod->taps[i] = h;
        sum += h;
    }

    /* One symbol every C4FM_MOD_SPS samples: the DC gain of the filter is
     * C4FM_MOD_SPS / sum per unit symbol level, scaled to the deviation. */
    for (int i = 0; i < C4FM_MOD_TAPS; i++)
        mod->taps[i] *= gain * (float)C4FM_MOD_SPS / sum;

    memset(mod->hist, 0, sizeof(mod->hist));
    mod->histPos = 0;
    mod->gain = gain;
    mod->invert = invert;
}

void c4fmMod_dibits(c4fmMod_t *mod, const uint8_t *dibits, int n, int16_t *out)
{
    /* Dibit to level: 01 = +3, 00 = +1, 10 = -1, 11 = -3 */
    static const float sym[4] = { +1.0f, +3.0f, -1.0f, -3.0f };
    const int span = (C4FM_MOD_TAPS + C4FM_MOD_SPS - 1) / C4FM_MOD_SPS;

    for (int k = 0; k < n; k++)
    {
        mod->histPos = (mod->histPos + 1) & 15u;
        mod->hist[mod->histPos] = (dibits != NULL) ? sym[dibits[k] & 3u] : 0.0f;

        for (int p = 0; p < C4FM_MOD_SPS; p++)
        {
            /* Zero-stuffed symbols through the filter: one tap in ten meets
             * a symbol, so the inner loop walks the taps that matter. */
            float y = 0.0f;

            for (int j = 0; j < span; j++)
            {
                const int t = p + C4FM_MOD_SPS * j;

                if (t >= C4FM_MOD_TAPS)
                    break;
                y += mod->taps[t] * mod->hist[(mod->histPos - j) & 15u];
            }

            if (mod->invert)
                y = -y;
            if (y > 32767.0f)
                y = 32767.0f;
            if (y < -32768.0f)
                y = -32768.0f;
            out[k * C4FM_MOD_SPS + p] = (int16_t)y;
        }
    }
}
