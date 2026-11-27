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

/*
 * Version 7 -> 8.  The record on flash is a prefix of today's, so the migration
 * is a copy of the prefix plus the defaults of what was appended.  It is
 * written out for this one version on purpose: a later version that appends
 * another field repeats the exercise with its own size and its own default,
 * because "one byte shorter than today" stops being true as soon as anything
 * else is added.
 */
#define SETTINGS_V7 7u
#define SETTINGS_V7_PAYLOAD ((size_t)(sizeof(settings_t) - sizeof(uint8_t)))

static void settings_recordUpgradeV7(settings_t *s)
{
    /*
     * A version 7 record says nothing about the pad, so it takes to-day's
     * factory value: the PTT switch.  Handing the pad to the serial port on an
     * upgrade would leave an operator with a radio that does not transmit and
     * no cable in hand to find out why (docs/ACCESSORY.md).
     */
    s->pc_port = (uint8_t)PORT_MODE_NEVER;
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
    /*
     * Only the header is needed to tell whose record this is: the length check
     * comes after the version, because a record written by a build with
     * another set of optional blocks (CONFIG_APRS, CONFIG_KISS) has a
     * different size *and* a different version, and "another version" is the
     * answer the operator needs -- "corrupt" sends him looking for a fault.
     */
    if (buf == NULL || out == NULL || len < SETTINGS_RECORD_HDR)
        return SETTINGS_RECORD_ERR_CORRUPT;

    if (read_u32(buf) != SETTINGS_RECORD_MAGIC)
        return SETTINGS_RECORD_ERR_CORRUPT;

    /*
     * Version before CRC: an old record has to be reported as "written by
     * another firmware" so that the screen can say the settings were reset by
     * the upgrade, not as a random corruption.
     */
    uint16_t version = read_u16(buf + 6);

    if (version == SETTINGS_V7) {
        /*
         * The one version that is a prefix of this one: version 8 appended
         * settings_t.pc_port and nothing else, so a version 7 record is the
         * same payload one byte shorter, with its own CRC.  Loading the prefix
         * and leaving the appended field at its default keeps everything the
         * operator had -- callsign, channels' power, scan settings -- instead
         * of dropping the record as a whole.  A v7 record whose CRC does not
         * match over that length is still corrupt.
         */
        if (len < SETTINGS_RECORD_HDR + SETTINGS_V7_PAYLOAD)
            return SETTINGS_RECORD_ERR_CORRUPT;

        uint16_t oldCrc = crc_ccitt(buf + SETTINGS_RECORD_HDR,
                                    SETTINGS_V7_PAYLOAD);
        if (oldCrc != read_u16(buf + 4))
            return SETTINGS_RECORD_ERR_CORRUPT;

        memset(out, 0, sizeof(*out));
        memcpy(out, buf + SETTINGS_RECORD_HDR, SETTINGS_V7_PAYLOAD);
        settings_recordUpgradeV7(out);
        return 0;
    }

    if (version != (uint16_t)SETTINGS_VERSION)
        return SETTINGS_RECORD_ERR_VERSION;

    if (len < SETTINGS_RECORD_SIZE)
        return SETTINGS_RECORD_ERR_CORRUPT;

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
