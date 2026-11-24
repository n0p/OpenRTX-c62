/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_enc.c
 * @brief The AMBE+2 encoder hook, see dmr_enc.h.
 */

#include <string.h>
#include "protocols/DMR/dmr_enc.h"

#ifdef CONFIG_DMR_ENC_OPENDMR
/* ---- reference backend: OpenDMR (off by default) ------------------------- */
#include "opendmr.h"

static opendmr_encoder_t *s_enc;

static bool backend_available(void)
{
    /* Created once, kept: the encoder is stateful across frames of a call */
    if (s_enc == NULL)
        s_enc = opendmr_encoder_create();
    return s_enc != NULL;
}

static void backend_reset(void)
{
    if (s_enc != NULL)
        opendmr_encoder_reset(s_enc);
}

static bool backend_encode(const int16_t pcm[160], uint8_t ambe[9])
{
    if (s_enc == NULL)
        return false;
    return opendmr_encode(s_enc, pcm, ambe);
}
#else
/* ---- default backend: ours, lib/dmr_encoder ------------------------------ */
#include "dmr_encoder.h"

static bool backend_available(void)
{
    return true;
}

static void backend_reset(void)
{
    dmr_encoder_reset();
}

static bool backend_encode(const int16_t pcm[160], uint8_t ambe[9])
{
    return dmr_encoder_encode(pcm, ambe) == 0;
}
#endif /* CONFIG_DMR_ENC_OPENDMR */

/*
 * Both encoders hand out the 72 bits in the canonical (DVSI) order:
 * A = Golay(24,12) of the first 12 data bits, B = scrambled Golay(23,12) of
 * the next 12, C = the last 25, MSB first, 9 bytes.  dmr_tx_voice() does not
 * take that: it places the 9 bytes as they go out on the air, which is the
 * ETSI TS 102 361-1 four-block interleave of the same 72 bits (four blocks of
 * 18, taken one bit from each in turn; 36 dibits per voice frame).  The
 * receiver undoes it with the rW/rX/rY/rZ schedule (dmr_voice.c, from DSD).
 *
 * So the adapter interleaves here, once, for either backend: canonical bit c
 * goes to air bit 4*(c mod 18) + c/18.
 */
static void canonical_to_air(const uint8_t in[9], uint8_t out[9])
{
    memset(out, 0, 9);
    for (int c = 0; c < 72; c++) {
        const uint8_t bit = (uint8_t)((in[c >> 3] >> (7 - (c & 7))) & 1u);
        const int a = 4 * (c % 18) + c / 18;

        out[a >> 3] |= (uint8_t)(bit << (7 - (a & 7)));
    }
}

bool dmrEnc_available(void)
{
    return backend_available();
}

void dmrEnc_reset(void)
{
    backend_reset();
}

bool dmrEnc_encode(const int16_t pcm[160], uint8_t ambe[9])
{
    uint8_t canonical[9];

    if (!backend_encode(pcm, canonical))
        return false;
    canonical_to_air(canonical, ambe);
    return true;
}
