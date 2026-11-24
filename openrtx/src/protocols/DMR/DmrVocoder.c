/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrVocoder.c
 * @brief Weak default of the AMBE+2 encoder hook, see DmrVocoder.h.
 *
 * A platform that has an encoder (the HiFi4 service, an optimised port, the
 * host tools of work/dmr-tx) overrides the symbol; everybody else gets a
 * transmitter that refuses to key instead of one that sends silence.
 *
 * The default is an alias so that dmrVocoder_available() can tell "nobody
 * overrode me" from "an encoder is linked in" by comparing the addresses,
 * which is the only portable way in C.
 */

#include "protocols/DMR/DmrVocoder.h"

static int encodeUnavailable(const int16_t pcm[160], uint8_t frame[9]);

int dmrVocoder_encode(const int16_t pcm[160], uint8_t frame[9])
    __attribute__((weak, alias("encodeUnavailable")));

static int encodeUnavailable(const int16_t pcm[160], uint8_t frame[9])
{
    (void)pcm;
    (void)frame;
    return -1;
}

bool dmrVocoder_available(void)
{
    return (void *)dmrVocoder_encode != (void *)encodeUnavailable;
}
