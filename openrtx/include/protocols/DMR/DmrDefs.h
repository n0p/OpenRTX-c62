/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_DEFS_H
#define OPENRTX_DMR_DEFS_H

/**
 * @file DmrDefs.h
 * @brief Burst data types and their names, for the UI.
 *
 * Everything else this header used to carry (geometry, sync words, the voice
 * interleaver, the slot type Golay code) is now the package's
 * (protocols/DMR/dmr_fec.h, dmr_proto.h, dmr_voice.c); what is left is the
 * one table the screen needs to print what the last burst was.  Values from
 * ETSI TS 102 361-1 table 6.6, as dmr_proto classifies them.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Burst data types (TS 102 361-1 table 6.6): slot type of a data burst. */
enum dmrDataType {
    DMR_DT_PI_HEADER = 0,
    DMR_DT_VOICE_LC = 1,
    DMR_DT_TERMINATOR_LC = 2,
    DMR_DT_CSBK = 3,
    DMR_DT_MBC_HEADER = 4,
    DMR_DT_MBC_CONT = 5,
    DMR_DT_DATA_HEADER = 6,
    DMR_DT_RATE_12 = 7,
    DMR_DT_RATE_34 = 8,
    DMR_DT_IDLE = 9,
    DMR_DT_RATE_1 = 10,
    DMR_DT_UNIFIED_12 = 11,
    DMR_DT_UNIFIED_34 = 12,
};

/** @return a short name of a burst data type, for the screen. */
const char *dmr_data_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_DEFS_H */
