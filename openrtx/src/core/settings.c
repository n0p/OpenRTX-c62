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
