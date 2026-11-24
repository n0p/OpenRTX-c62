/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrDefs.c
 * @brief Burst data type names, see DmrDefs.h.
 */

#include "protocols/DMR/DmrDefs.h"

const char *dmr_data_type_name(uint8_t type)
{
    static const char *const names[] = {
        "PI hdr",   "VoiceLC", "Term", "CSBK", "MBC hdr", "MBC",
        "Data hdr", "1/2",     "3/4",  "Idle", "Rate 1",  "Unified",
    };

    if (type >= (sizeof(names) / sizeof(names[0])))
        return "?";

    return names[type];
}
