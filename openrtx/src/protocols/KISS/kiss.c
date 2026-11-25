/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/KISS/kiss.h"
#include <string.h>

static const char *cmdNames[] =
{
    "DATA", "TXDELAY", "PERSIST", "SLOTTIME", "TXTAIL", "FULLDUP",
    "HARDWARE"
};

void kiss_dec_init(kiss_dec_t *d, bool simple)
{
    memset(d, 0, sizeof(*d));
    d->simple = simple;
}

void kiss_dec_reset(kiss_dec_t *d)
{
    d->inFrame  = false;
    d->escape   = false;
    d->first    = true;
    d->overflow = false;
    d->len      = 0;
}

const char *kiss_cmd_name(uint8_t cmd)
{
    if (cmd <= KISS_CMD_HARDWARE)
        return cmdNames[cmd];
    if (cmd == KISS_CMD_END_KISS)
        return "END-KISS";
    if (cmd == KISS_CMD_RETURN)
        return "RETURN";
    return "?";
}

bool kiss_cmd_isParam(uint8_t cmd)
{
    return (cmd >= KISS_CMD_TXDELAY) && (cmd <= KISS_CMD_HARDWARE);
}

/*
 * Store one payload byte. Overlong messages are not truncated silently: the
 * whole message is dropped at the closing FEND and counted, because a
 * truncated AX.25 frame is a corrupt frame and would key the transmitter on
 * garbage.
 */
static inline void put(kiss_dec_t *d, uint8_t b)
{
    if (d->len < KISS_MAX_PAYLOAD)
        d->msg.data[d->len++] = b;
    else
        d->overflow = true;
}

kiss_ev_t kiss_dec_byte(kiss_dec_t *d, uint8_t b)
{
    d->bytes++;

    if (b == KISS_FEND)
    {
        bool complete = d->inFrame && (!d->first);

        if (complete && d->overflow)
        {
            d->bad++;
            complete = false;
        }

        if (complete)
        {
            /* The command byte went to msg.port/msg.cmd, so the payload is
             * exactly what put() collected. */
            d->msg.len = d->len;
            d->frames++;
        }

        /* Start the next message. */
        d->inFrame  = true;
        d->escape   = false;
        d->first    = !d->simple;
        d->overflow = false;
        d->len      = 0;

        if (!complete)
            return KISS_EV_NONE;

        if (d->simple)
        {
            d->msg.cmd  = KISS_CMD_DATA;
            d->msg.port = 0;
            return KISS_EV_DATA;
        }

        switch (d->msg.cmd)
        {
            case KISS_CMD_DATA:
                return KISS_EV_DATA;

            case KISS_CMD_RETURN:
            case KISS_CMD_END_KISS:
                return KISS_EV_RETURN;

            case KISS_CMD_TXDELAY:
            case KISS_CMD_PERSIST:
            case KISS_CMD_SLOTTIME:
            case KISS_CMD_TXTAIL:
            case KISS_CMD_FULLDUP:
            case KISS_CMD_HARDWARE:
                return KISS_EV_PARAM;

            default:
                return KISS_EV_UNKNOWN;
        }
    }

    if (!d->inFrame)
        return KISS_EV_NONE;

    if ((!d->simple) && (b == KISS_FESC))
    {
        d->escape = true;
        return KISS_EV_NONE;
    }

    if (d->escape)
    {
        d->escape = false;
        if (b == KISS_TFEND)
            b = KISS_FEND;
        else if (b == KISS_TFESC)
            b = KISS_FESC;
        else
            d->bad++;   /* undefined escape: keep the byte as it came */
    }

    if (d->first)
    {
        d->msg.port = (uint8_t)(b >> 4);
        d->msg.cmd  = (uint8_t)(b & 0x0Fu);
        d->first    = false;
        return KISS_EV_NONE;
    }

    put(d, b);
    return KISS_EV_NONE;
}

/* ------------------------------------------------------------- encoder --- */

static size_t closeFrame(uint8_t *out, size_t cap, size_t n)
{
    if (n + 1 > cap)
        return 0;
    out[n++] = KISS_FEND;
    return n;
}

static size_t putEscaped(uint8_t *out, size_t cap, size_t n, uint8_t b)
{
    if (b == KISS_FEND)
    {
        if (n + 2 > cap)
            return 0;
        out[n++] = KISS_FESC;
        out[n++] = KISS_TFEND;
    }
    else if (b == KISS_FESC)
    {
        if (n + 2 > cap)
            return 0;
        out[n++] = KISS_FESC;
        out[n++] = KISS_TFESC;
    }
    else
    {
        if (n + 1 > cap)
            return 0;
        out[n++] = b;
    }
    return n;
}

size_t kiss_enc_data(uint8_t *out, size_t cap, uint8_t port,
                     const uint8_t *payload, size_t len, bool simple)
{
    if (cap < 3)
        return 0;

    size_t n = 0;
    out[n++] = KISS_FEND;

    if (!simple)
    {
        out[n++] = (uint8_t)(((port & 0x0Fu) << 4) | KISS_CMD_DATA);
    }

    for (size_t i = 0; i < len; i++)
    {
        if (simple)
        {
            /* KISS simple has no escape: a 0xC0 in the payload would end the
             * frame, so such a message cannot be represented at all. */
            if (payload[i] == KISS_FEND)
                return 0;
            if (n + 1 > cap)
                return 0;
            out[n++] = payload[i];
        }
        else
        {
            n = putEscaped(out, cap, n, payload[i]);
            if (n == 0)
                return 0;
        }
    }

    return closeFrame(out, cap, n);
}

size_t kiss_enc_param(uint8_t *out, size_t cap, uint8_t cmd, uint8_t value)
{
    return kiss_enc_cmd(out, cap, 0, cmd, &value, 1);
}

size_t kiss_enc_cmd(uint8_t *out, size_t cap, uint8_t port, uint8_t cmd,
                    const uint8_t *payload, size_t len)
{
    if (cap < 4)
        return 0;

    size_t n = 0;
    out[n++] = KISS_FEND;
    /* 0xFF (RETURN) is a whole byte, not port 15 command 15 */
    out[n++] = (cmd > 0x0Fu) ? cmd
                             : (uint8_t)(((port & 0x0Fu) << 4) | cmd);

    for (size_t i = 0; i < len; i++)
    {
        n = putEscaped(out, cap, n, payload[i]);
        if (n == 0)
            return 0;
    }

    return closeFrame(out, cap, n);
}

size_t kiss_enc_cheese(uint8_t *out, size_t cap, size_t n)
{
    if (n > cap)
        n = cap;
    memset(out, KISS_FEND, n);
    return n;
}
