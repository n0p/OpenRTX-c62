/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/KISS/packet_modem.h"
#include "protocols/APRS/ax25.h"
#include <string.h>
#include <strings.h>

/*
 * The registry. `maxPayload` is what the mode can carry in one KISS frame:
 * the AX.25 ceiling (7 * (2 + 8) + 2 + 256 = 328 without the FCS) for the two
 * AFSK modes, the 9 octets of the SACCH layer 3 message for NXDN (a short data
 * call would be longer, but it travels in FACCH1 and is not decoded), and one
 * DMR data packet (32 blocks of 24 octets, dmr_data.h) for DMRIP, cut at what
 * one KISS message can hold.
 */
static const pkt_modem_info_t modems[PKT_MODEM_COUNT] =
{
    {
        .name       = "AFSK1200",
        .label      = "1200",
        .baud       = 1200,
        .caps       = PKT_CAP_TX | PKT_CAP_RX,
        .maxPayload = 328,
        .framing    = "AX.25 / HDLC / NRZI, Bell 202 AFSK mark 1200 Hz "
                      "space 2200 Hz, 1200 bit/s"
    },
    {
        .name       = "AFSK9600",
        .label      = "9600",
        .baud       = 9600,
        .caps       = PKT_CAP_TX | PKT_CAP_RX,
        .maxPayload = 328,
        .framing    = "AX.25 / HDLC, G3RUH scrambler 1+x^12+x^17, baseband "
                      "NRZ 9600 bit/s, 5 samples per bit at 48 kHz"
    },
    {
        .name       = "NXDN",
        .label      = "NXDN",
        .baud       = 0,
        .caps       = PKT_CAP_RX,
        .maxPayload = 9,
        .framing    = "SACCH layer 3 message of an NXDN superframe, 9 octets, "
                      "receive only"
    },
    {
        .name       = "DMRIP",
        .label      = "DMRIP",
        .baud       = 0,
        .caps       = PKT_CAP_RX,
        .maxPayload = 256,
        .framing    = "IPv4/UDP datagram of a DMR packet data call "
                      "(dmr_data.c), receive only"
    }
};

const pkt_modem_info_t *pkt_modem_info(pkt_modem_t mode)
{
    if ((unsigned)mode >= PKT_MODEM_COUNT)
        return &modems[PKT_MODEM_AFSK1200];
    return &modems[mode];
}

const char *pkt_modem_name(pkt_modem_t mode)
{
    if ((unsigned)mode >= PKT_MODEM_COUNT)
        return "?";
    return modems[mode].name;
}

int pkt_modem_parse(const char *name)
{
    if (name == NULL)
        return -1;

    for (int i = 0; i < (int)PKT_MODEM_COUNT; i++)
    {
        if (strcasecmp(name, modems[i].name) == 0)
            return i;
    }

    /* The rate alone is what an operator types */
    if ((strcmp(name, "1200") == 0) || (strcmp(name, "afsk1200") == 0))
        return PKT_MODEM_AFSK1200;
    if ((strcmp(name, "9600") == 0) || (strcmp(name, "g3ruh") == 0))
        return PKT_MODEM_AFSK9600;
    if ((strcmp(name, "dmr") == 0) || (strcmp(name, "ip") == 0))
        return PKT_MODEM_DMRIP;

    return -1;
}

uint32_t pkt_modem_spb(pkt_modem_t mode, uint32_t fs)
{
    switch (mode)
    {
        case PKT_MODEM_AFSK1200:
            return fs / (uint32_t)AFSK_BAUD;

        case PKT_MODEM_AFSK9600:
            if ((fs % G3RUH_BAUD) != 0)
                return 0;
            return fs / G3RUH_BAUD;

        default:
            return 0;
    }
}

/* --------------------------------------------------------------- engine --- */

bool pkt_engine_init(pkt_engine_t *e, pkt_modem_t mode, uint32_t fs)
{
    memset(e, 0, sizeof(*e));
    e->mode = mode;
    e->fs   = fs;

    switch (mode)
    {
        case PKT_MODEM_AFSK1200:
            afsk_mod_init(&e->afsk, fs, 0.5f);
            afsk_mod_setGain(&e->afsk, 0.8f);
            if (!afsk_demod_init(&e->afskDem, fs))
                return false;
            break;

        case PKT_MODEM_AFSK9600:
            if (!g3ruh_mod_init(&e->g3ruh, fs))
                return false;
            if (!g3ruh_demod_init(&e->g3ruhDem, fs))
                return false;
            g3ruh_mod_setGain(&e->g3ruh, 0.8f);
            break;

        case PKT_MODEM_NXDN:
        case PKT_MODEM_DMRIP:
            /* Payload only: nothing to modulate or demodulate */
            break;

        default:
            return false;
    }

    hdlc_deframer_init(&e->deframer);
    return true;
}

static void onFrame(const uint8_t *frame, size_t len, void *user)
{
    pkt_engine_t *e = (pkt_engine_t *)user;

    e->rxFrames++;
    if (e->onFrame != NULL)
        e->onFrame(frame, len, e->user);
}

static void onDemodBit(bool bit, void *user)
{
    pkt_engine_t *e = (pkt_engine_t *)user;

    e->rxBits++;
    hdlc_rx_bit(&e->deframer, bit, onFrame, e);
}

static void onDemodLevel(bool level, void *user)
{
    pkt_engine_t *e = (pkt_engine_t *)user;

    e->rxBits++;
    hdlc_rx_level(&e->deframer, level, onFrame, e);
}

void pkt_rx_begin(pkt_engine_t *e, pkt_frame_cb cb, void *user)
{
    hdlc_deframer_init(&e->deframer);
    e->onFrame  = cb;
    e->user     = user;
    e->rxFrames = 0;
    e->rxFcsErr = 0;
    e->rxBits   = 0;

    switch (e->mode)
    {
        case PKT_MODEM_AFSK1200:
            afsk_demod_init(&e->afskDem, e->fs);
            break;

        case PKT_MODEM_AFSK9600:
            g3ruh_demod_init(&e->g3ruhDem, e->fs);
            break;

        default:
            break;
    }
}

void pkt_rx_block(pkt_engine_t *e, const int16_t *in, size_t n)
{
    switch (e->mode)
    {
        case PKT_MODEM_AFSK1200:
            afsk_demod_block(&e->afskDem, in, n, onDemodLevel, e);
            break;

        case PKT_MODEM_AFSK9600:
            g3ruh_demod_block(&e->g3ruhDem, in, n, onDemodBit, e);
            break;

        default:
            break;
    }

    e->rxFcsErr = e->deframer.badFcs;
}

void pkt_rx_payload(pkt_engine_t *e, const uint8_t *payload, size_t len)
{
    if (payload == NULL || len == 0)
        return;

    if (len > pkt_modem_info(e->mode)->maxPayload)
        len = pkt_modem_info(e->mode)->maxPayload;

    e->rxFrames++;
    if (e->onFrame != NULL)
        e->onFrame(payload, len, e->user);
}

bool pkt_rx_signal(const pkt_engine_t *e)
{
    switch (e->mode)
    {
        case PKT_MODEM_AFSK1200:
            return afsk_demod_signal(&e->afskDem);

        case PKT_MODEM_AFSK9600:
            return g3ruh_demod_signal(&e->g3ruhDem);

        default:
            return false;
    }
}

bool pkt_tx_begin(pkt_engine_t *e, const uint8_t *payload, size_t len,
                  uint32_t txdelayMs, uint32_t txtailMs)
{
    if ((pkt_modem_info(e->mode)->caps & PKT_CAP_TX) == 0)
        return false;

    if ((payload == NULL) || (len < 1) ||
        (len > pkt_modem_info(e->mode)->maxPayload) ||
        ((len + 2u) > sizeof(e->frame)))
        return false;

    /* KISS carries the frame without the FCS; the TNC appends it */
    memcpy(e->frame, payload, len);
    uint16_t fcs = ax25_fcs(payload, len);
    e->frame[len]     = (uint8_t)(fcs & 0xFFu);
    e->frame[len + 1] = (uint8_t)(fcs >> 8);

    /*
     * TXDELAY, the way a TNC2 spends it: the transmitter is keyed and the
     * channel carries HDLC flags (not silence) until the delay is over, so the
     * far end has something to open its squelch, settle its AGC and lock its
     * bit clock on. At 1200 baud a flag is 6.7 ms, at 9600 it is 0.83 ms, so
     * the count follows the rate. Two flags minimum, and the rest of a very
     * long delay is silence: the deframer does not care, and 240 flags is
     * already two seconds at 9600.
     */
    const uint32_t bitRate = (e->mode == PKT_MODEM_AFSK9600) ? G3RUH_BAUD
                                                             : (uint32_t)AFSK_BAUD;
    uint32_t flagBits = (bitRate * txdelayMs) / 1000u;
    uint32_t flags = flagBits / 8u;

    if (flags < 2u)
        flags = 2u;
    if (flags > 240u)
        flags = 240u;

    hdlc_tx_start(&e->framer, e->frame, len + 2u, (uint8_t)flags, 2);

    switch (e->mode)
    {
        case PKT_MODEM_AFSK1200:
            afsk_mod_reset(&e->afsk);
            break;

        case PKT_MODEM_AFSK9600:
            g3ruh_mod_reset(&e->g3ruh);
            break;

        default:
            return false;
    }

    e->silenceLeft = (uint32_t)(((uint64_t)e->fs * txdelayMs) / 1000u);
    e->tailLeft    = (uint32_t)(((uint64_t)e->fs * txtailMs) / 1000u);
    e->txBits      = 0;
    e->txActive    = true;
    return true;
}

size_t pkt_tx_block(pkt_engine_t *e, int16_t *out, size_t max)
{
    if (!e->txActive)
        return 0;

    const uint32_t spb = pkt_modem_spb(e->mode, e->fs);
    if (spb == 0)
    {
        e->txActive = false;
        return 0;
    }

    size_t written = 0;

    while ((written + spb) <= max)
    {
        if (e->silenceLeft > 0)
        {
            uint32_t n = (e->silenceLeft < spb) ? e->silenceLeft : spb;
            memset(&out[written], 0, n * sizeof(int16_t));
            written        += n;
            e->silenceLeft -= n;
            continue;
        }

        if (hdlc_tx_finished(&e->framer))
        {
            if (e->tailLeft == 0)
            {
                e->txActive = false;
                break;
            }
            uint32_t n = (e->tailLeft < spb) ? e->tailLeft : spb;
            memset(&out[written], 0, n * sizeof(int16_t));
            written      += n;
            e->tailLeft  -= n;
            continue;
        }

        bool bit = hdlc_tx_bit(&e->framer);
        e->txBits++;

        if (e->mode == PKT_MODEM_AFSK9600)
            g3ruh_mod_bit(&e->g3ruh, bit, &out[written]);
        else
            afsk_mod_bit(&e->afsk, bit, &out[written]);

        written += spb;
    }

    if (written == 0 && hdlc_tx_finished(&e->framer) && (e->tailLeft == 0))
        e->txActive = false;

    return written;
}

bool pkt_tx_busy(const pkt_engine_t *e)
{
    return e->txActive;
}

void pkt_tx_abort(pkt_engine_t *e)
{
    e->txActive    = false;
    e->silenceLeft = 0;
    e->tailLeft    = 0;
    e->framer.phase = HDLC_TX_DONE;
}
