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

/*
 * The pulse NXDN and dPMR transmit, P(f) = H(f) sinc(f T): a root raised
 * cosine of roll-off 0.2 times the sinc of a symbol held for its period (TS
 * 1-A 3.4/3.5 and ETSI TS 102 658 14.2.2.2 -- "H(f) P(f)" out, "H(f) D(f)"
 * in).  The hold is what a symbol-rate modulator does with each symbol before
 * the pulse shaper, and it is the reason both receivers use H(f)/sinc(fT):
 * together the pair is the Nyquist raised cosine, H(f)^2.
 *
 * 161 taps at 20 samples per symbol, four symbols of span each way, DC gain
 * one, from tools/nxdn_rx_pulse.py --tx.  Only valid at 20 samples per
 * symbol, which is exactly FSK4_HOLD_SPS.
 */
static const float hold_pulse[FSK4_TX_MAX_TAPS] = {
    6.180322894e-04f, 4.775605687e-04f, 3.113963622e-04f, 1.231387129e-04f,
    -8.267286204e-05f, -3.006465814e-04f, -5.246583679e-04f, -7.480014815e-04f,
    -9.635627124e-04f, -1.164021070e-03f, -1.342064021e-03f, -1.490615567e-03f,
    -1.603069818e-03f, -1.673523255e-03f, -1.696998588e-03f, -1.669653046e-03f,
    -1.588964037e-03f, -1.453885463e-03f, -1.264968540e-03f, -1.024441695e-03f,
    -7.362450743e-04f, -4.060162885e-04f, -4.102529401e-05f, 3.499423570e-04f,
    7.567532022e-04f, 1.168142329e-03f, 1.571974968e-03f, 1.955548157e-03f,
    2.305926044e-03f, 2.610301224e-03f, 2.856373398e-03f, 3.032735836e-03f,
    3.129259483e-03f, 3.137464187e-03f, 3.050866430e-03f, 2.865293153e-03f,
    2.579151735e-03f, 2.193646957e-03f, 1.712936820e-03f, 1.144220392e-03f,
    4.977523585e-04f, -2.132193280e-04f, -9.725944500e-04f, -1.761639957e-03f,
    -2.559285599e-03f, -3.342491492e-03f, -4.086681390e-03f, -4.766233557e-03f,
    -5.355019415e-03f, -5.826978638e-03f, -6.156718104e-03f, -6.320121118e-03f,
    -6.294952695e-03f, -6.061446339e-03f, -5.602857829e-03f, -4.905971915e-03f,
    -3.961548602e-03f, -2.764696811e-03f, -1.315164671e-03f, 3.824625811e-04f,
    2.318664104e-03f, 4.478989159e-03f, 6.844169460e-03f, 9.390338109e-03f,
    1.208935259e-02f, 1.490921675e-02f, 1.781459435e-02f, 2.076740443e-02f,
    2.372748675e-02f, 2.665332379e-02f, 2.950280411e-02f, 3.223401102e-02f,
    3.480601946e-02f, 3.717968382e-02f, 3.931839934e-02f, 4.118882023e-02f,
    4.276151856e-02f, 4.401156907e-02f, 4.491904687e-02f, 4.546942672e-02f,
    4.565387483e-02f, 4.546942672e-02f, 4.491904687e-02f, 4.401156907e-02f,
    4.276151856e-02f, 4.118882023e-02f, 3.931839934e-02f, 3.717968382e-02f,
    3.480601946e-02f, 3.223401102e-02f, 2.950280411e-02f, 2.665332379e-02f,
    2.372748675e-02f, 2.076740443e-02f, 1.781459435e-02f, 1.490921675e-02f,
    1.208935259e-02f, 9.390338109e-03f, 6.844169460e-03f, 4.478989159e-03f,
    2.318664104e-03f, 3.824625811e-04f, -1.315164671e-03f, -2.764696811e-03f,
    -3.961548602e-03f, -4.905971915e-03f, -5.602857829e-03f, -6.061446339e-03f,
    -6.294952695e-03f, -6.320121118e-03f, -6.156718104e-03f, -5.826978638e-03f,
    -5.355019415e-03f, -4.766233557e-03f, -4.086681390e-03f, -3.342491492e-03f,
    -2.559285599e-03f, -1.761639957e-03f, -9.725944500e-04f, -2.132193280e-04f,
    4.977523585e-04f, 1.144220392e-03f, 1.712936820e-03f, 2.193646957e-03f,
    2.579151735e-03f, 2.865293153e-03f, 3.050866430e-03f, 3.137464187e-03f,
    3.129259483e-03f, 3.032735836e-03f, 2.856373398e-03f, 2.610301224e-03f,
    2.305926044e-03f, 1.955548157e-03f, 1.571974968e-03f, 1.168142329e-03f,
    7.567532022e-04f, 3.499423570e-04f, -4.102529401e-05f, -4.060162885e-04f,
    -7.362450743e-04f, -1.024441695e-03f, -1.264968540e-03f, -1.453885463e-03f,
    -1.588964037e-03f, -1.669653046e-03f, -1.696998588e-03f, -1.673523255e-03f,
    -1.603069818e-03f, -1.490615567e-03f, -1.342064021e-03f, -1.164021070e-03f,
    -9.635627124e-04f, -7.480014815e-04f, -5.246583679e-04f, -3.006465814e-04f,
    -8.267286204e-05f, 1.231387129e-04f, 3.113963622e-04f, 4.775605687e-04f,
    6.180322894e-04f
};

void fsk4Mod_init(fsk4_mod_t *mod, int sps, float gain, bool invert, bool hold)
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

    if (hold && (sps == FSK4_HOLD_SPS)) {
        /*
         * H(f) sinc(fT), the pulse the standard sends: the receiver's H(f)
         * D(f) then completes the Nyquist raised cosine.  Already normalised
         * to a DC gain of one, so the scale is the same as the RRC's below.
         */
        for (int i = 0; i < mod->tapsN; i++)
            mod->taps[i] = hold_pulse[i] * gain * (float)sps;

        memset(mod->hist, 0, sizeof(mod->hist));
        mod->histPos = 0;
        mod->gain = gain;
        mod->invert = invert;

        return;
    }

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
