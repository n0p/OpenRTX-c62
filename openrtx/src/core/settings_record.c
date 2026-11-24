/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/settings_record.h"
#include "core/crc.h"
#include <string.h>

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int settings_recordPack(uint8_t *buf, size_t cap, const settings_t *s)
{
    if (buf == NULL || s == NULL || cap < SETTINGS_RECORD_SIZE)
        return -1;

    write_u32(buf, SETTINGS_RECORD_MAGIC);
    write_u16(buf + 6, (uint16_t)SETTINGS_VERSION);
    memcpy(buf + SETTINGS_RECORD_HDR, s, sizeof(settings_t));
    write_u16(buf + 4, crc_ccitt(buf + SETTINGS_RECORD_HDR, sizeof(settings_t)));

    return (int)SETTINGS_RECORD_SIZE;
}

uint16_t settings_recordVersion(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < SETTINGS_RECORD_HDR)
        return 0xFFFF;

    return read_u16(buf + 6);
}

int settings_recordRead(const uint8_t *buf, size_t len, settings_t *out)
{
    if (buf == NULL || out == NULL || len < SETTINGS_RECORD_SIZE)
        return SETTINGS_RECORD_ERR_CORRUPT;

    if (read_u32(buf) != SETTINGS_RECORD_MAGIC)
        return SETTINGS_RECORD_ERR_CORRUPT;

    /*
     * Version before CRC: an old record has to be reported as "written by
     * another firmware" so that the screen can say the settings were reset by
     * the upgrade, not as a random corruption.
     */
    if (read_u16(buf + 6) != (uint16_t)SETTINGS_VERSION)
        return SETTINGS_RECORD_ERR_VERSION;

    uint16_t crc = crc_ccitt(buf + SETTINGS_RECORD_HDR, sizeof(settings_t));
    if (crc != read_u16(buf + 4))
        return SETTINGS_RECORD_ERR_CORRUPT;

    memcpy(out, buf + SETTINGS_RECORD_HDR, sizeof(settings_t));
    return 0;
}

int settings_recordLoad(const uint8_t *buf, size_t len, settings_t *out,
                        uint8_t *cause)
{
    /* A blank flash reads all 0xFF: magic mismatch, and that is not a fault */
    bool blank = true;

    if (buf != NULL) {
        for (size_t i = 0; (i < SETTINGS_RECORD_HDR) && (i < len); i++) {
            if (buf[i] != 0xFF) {
                blank = false;
                break;
            }
        }
    }

    if (len < SETTINGS_RECORD_SIZE)
        blank = true;

    int ret = settings_recordRead(buf, len, out);

    if (cause != NULL) {
        if (ret == 0)
            *cause = SETTINGS_NO_RESET;
        else if (blank)
            *cause = SETTINGS_RESET_BLANK;
        else if (ret == SETTINGS_RECORD_ERR_VERSION)
            *cause = SETTINGS_RESET_VERSION;
        else
            *cause = SETTINGS_RESET_CORRUPT;
    }

    return ret;
}
