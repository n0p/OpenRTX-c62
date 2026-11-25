/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/settings.h"

/*
 * Names only.  Everything else about settings_t is data, validated inline in
 * settings.h so that a menu can check a value without a call.
 */

/*
 * Scan steps, Hz.  The index is stored in flash (settings_t.scan_step), so the
 * order is part of the format: append only.
 *
 * 8.33 kHz is 25000/3 Hz, which no integer frequency can express: 8330 Hz is
 * the usual rounding and the error (4 Hz) is below the radio's own reference
 * accuracy.
 */
const uint32_t scan_steps[SETTINGS_SCAN_STEPS] = {
    5000U, 6250U, 8330U, 10000U, 12500U, 25000U
};

const char *settings_scanStepName(uint8_t idx)
{
    static const char *const names[SETTINGS_SCAN_STEPS] = {
        "5", "6.25", "8.33", "10", "12.5", "25"
    };

    return (idx < SETTINGS_SCAN_STEPS) ? names[idx] : "----";
}

const char *settings_portModeName(uint8_t mode)
{
    switch (mode) {
        case PORT_MODE_ALWAYS: return "Always";
        case PORT_MODE_NEVER:  return "Never";
        default:               return "Auto";
    }
}

const char *settings_dmrDestName(uint8_t type)
{
    switch (type)
    {
        case DMR_DEST_GROUP: return "group";
        case DMR_DEST_USER:  return "user";
        case DMR_DEST_NONE:  return "none";
        default:             return "----";
    }
}

const char *settings_smsFormatName(uint8_t format)
{
    switch (format)
    {
        case DMR_SMS_FMT_AUTO:      return "auto";
        case DMR_SMS_FMT_DD_UTF16BE:return "DD UTF-16BE";
        case DMR_SMS_FMT_DD_UTF8:   return "DD UTF-8";
        case DMR_SMS_FMT_IP_5016:   return "IP 5016";
        case DMR_SMS_FMT_IP_HC:     return "IP-HC";
        case DMR_SMS_FMT_ANYTONE:   return "AnyTone";
        case DMR_SMS_FMT_TMS:       return "TMS";
        case DMR_SMS_FMT_HYTERA:    return "Hytera";
        default:                    return "----";
    }
}

const char *settings_dmrTimeslotName(uint8_t timeslot)
{
    return (timeslot == 2) ? "TS2" : "TS1";
}
