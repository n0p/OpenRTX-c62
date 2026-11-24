/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APRS service: the receive tap on the FM discriminator, the transmit
 * scheduler (beacon / messages / acknowledgements), the received station list
 * and the mailbox.
 *
 * The design constraints, in order of importance:
 *
 *  1. Decoding off means no cost: the tap is not even opened, so the mode
 *     never touches a sample.
 *  2. Receiving never interrupts audio: the codec fans the discriminator out
 *     to both the speaker path and the MCU path, so the tap is free.
 *  3. Transmitting is always explicit and rate limited: a message from the
 *     menu or the beacon timer, never the PTT, at most one packet every
 *     APRS_TX_MIN_GAP_MS.
 */
#include "core/aprs_service.h"
#include "protocols/APRS/afsk.h"
#include "protocols/APRS/hdlc.h"
#include "core/state.h"
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* One block of audio for the tap: 20 ms at 24 kHz, the same size the C62's
 * codec hands out for the other digital modes. */
#define APRS_RX_BLOCK   480u

static struct
{
    bool     active;
    pathId   path;
    streamId stream;
    int16_t  buf[2u * APRS_RX_BLOCK];
    bool     pathOpen;

    afsk_demod_t    demod;
    hdlc_deframer_t deframer;
    aprs_station_list_t stations;
    aprs_mbox_t     mbox;

    /* Pending acknowledgements: a short FIFO, because a station that has just
     * sent us a message is the only thing we owe an answer to. */
    struct
    {
        char peer[APRS_CALL_MAX];
        char msgNo[6];
        bool used;
    } ack[4];

    bool     beaconRequested;
    uint32_t lastBeaconMs;
    bool     beaconValid;
    uint32_t lastTxMs;
    bool     txValid;
    uint32_t lastRxMs;
    uint16_t rxCount;

    /* The message whose packet is currently being transmitted */
    aprs_out_msg_t *txMsg;
} svc;

/* ------------------------------------------------------------- helpers --- */

static void ownCallsign(char *out, size_t cap)
{
    /* The AX.25 source is the plaintext callsign from the settings plus the
     * APRS SSID, which is separate so that the FM/menu path keeps working
     * with a bare callsign. */
    snprintf(out, cap, "%s-%u", state.settings.callsign,
             (unsigned)state.settings.aprs_ssid);
}

static aprs_pos_t localPosition(void)
{
    aprs_pos_t p;
    memset(&p, 0, sizeof(p));
    p.lat    = (double)state.settings.aprs_lat_e4 / 10000.0;
    p.lon    = (double)state.settings.aprs_lon_e4 / 10000.0;
    p.table  = state.settings.aprs_table;
    p.symbol = state.settings.aprs_symbol;
    snprintf(p.comment, sizeof(p.comment), "%s", state.settings.aprs_comment);
    return p;
}

static uint32_t nowMs(void)
{
    /* getTick() is k_uptime_get() on the C62 (platform/mcu/CSK6011B/drivers/
     * delays.c): milliseconds since boot, monotonic. */
    return (uint32_t)getTick();
}

/* ------------------------------------------------------------ frame RX --- */

static bool isOwnPacket(const ax25_frame_t *f)
{
    char own[APRS_CALL_MAX];
    ownCallsign(own, sizeof(own));

    char src[APRS_CALL_MAX];
    ax25_addr_format(src, sizeof(src), &f->src);
    if (strcmp(src, own) == 0)
        return true;

    /* A digipeated copy of our own packet carries us in the path */
    return aprs_path_contains(f, own);
}

static void onFrame(const uint8_t *frame, size_t len, void *user)
{
    (void)user;

    ax25_frame_t f;
    if (!ax25_parse((uint8_t *)frame, len, &f))
        return;

    /* APRS good practice: never act on your own packets, they come back from
     * the digipeaters with your callsign in the path. */
    if (isOwnPacket(&f))
        return;

    aprs_info_t info;
    if (!aprs_parse_info((const char *)f.info, f.infoLen, &info))
        return;

    char src[APRS_CALL_MAX];
    ax25_addr_format(src, sizeof(src), &f.src);

    const aprs_pos_t *lp = NULL;
    aprs_pos_t local = localPosition();
    if ((state.settings.aprs_lat_e4 != 0) || (state.settings.aprs_lon_e4 != 0))
        lp = &local;

    aprs_station_update(&svc.stations, &f, &info,
                        lp != NULL, local.lat, local.lon, nowMs());

    svc.rxCount++;
    svc.lastRxMs = nowMs();

    switch (info.type)
    {
        case APRS_INFO_MESSAGE:
        {
            /*
             * Only messages addressed to us matter, and each one is
             * acknowledged, which is what stops the sender retrying. The
             * mailbox drops duplicates, so a retransmission does not show up
             * twice in the inbox but is acknowledged again.
             */
            char own[APRS_CALL_MAX];
            ownCallsign(own, sizeof(own));
            if (strcmp(info.addressee, own) != 0)
                break;

            if (aprs_mbox_onMessage(&svc.mbox, src, info.text, info.msgNo,
                                    nowMs()))
            {
                if (state.settings.aprs_msgAck)
                    aprs_queueAck(src, info.msgNo);
            }
            else if (state.settings.aprs_msgAck)
            {
                aprs_queueAck(src, info.msgNo);
            }
            break;
        }

        case APRS_INFO_ACK:
        case APRS_INFO_REJ:
            aprs_mbox_onAck(&svc.mbox, src, info.msgNo,
                            info.type == APRS_INFO_REJ);
            break;

        default:
            break;
    }
}

/* --------------------------------------------------------------- RX API --- */

void aprs_service_init(void)
{
    memset(&svc, 0, sizeof(svc));
    hdlc_deframer_init(&svc.deframer);
    aprs_station_list_init(&svc.stations);
    aprs_mbox_init(&svc.mbox);
    svc.path = -1;
    svc.stream = -1;
}

bool aprs_rxBegin(void)
{
    if (svc.active)
        return true;

    svc.path = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    if (svc.path < 0)
        return false;

    svc.stream = audioStream_start(svc.path, svc.buf, 2u * APRS_RX_BLOCK,
                                   24000, STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (svc.stream < 0)
    {
        audioPath_release(svc.path);
        svc.path = -1;
        return false;
    }

    afsk_demod_init(&svc.demod, 24000);
    hdlc_deframer_init(&svc.deframer);
    svc.active = true;
    return true;
}

void aprs_rxEnd(void)
{
    if (!svc.active)
        return;

    audioStream_terminate(svc.stream);
    audioPath_release(svc.path);
    svc.stream = -1;
    svc.path   = -1;
    svc.active = false;
}

void aprs_rxPoll(void)
{
    if (!svc.active)
        return;

    dataBlock_t block = inputStream_getData(svc.stream);
    if ((block.data == NULL) || (block.len == 0))
        return;

    for (size_t i = 0; i < block.len; i++)
    {
        bool level;
        if (afsk_demod_sample(&svc.demod, block.data[i], &level))
            hdlc_rx_level(&svc.deframer, level, onFrame, NULL);
    }
}

bool aprs_rxActive(void)
{
    return svc.active;
}

/* --------------------------------------------------------------- TX API --- */

static bool channelBusy(uint32_t now)
{
    return svc.txValid
        && ((uint32_t)(now - svc.lastTxMs) < APRS_TX_MIN_GAP_MS);
}

void aprs_queueAck(const char *peer, const char *msgNo)
{
    for (size_t i = 0; i < 4; i++)
    {
        if (svc.ack[i].used)
            continue;

        snprintf(svc.ack[i].peer, sizeof(svc.ack[i].peer), "%s", peer);
        snprintf(svc.ack[i].msgNo, sizeof(svc.ack[i].msgNo), "%s", msgNo);
        svc.ack[i].used = true;
        return;
    }
}

bool aprs_requestBeacon(uint32_t now)
{
    /*
     * An explicit request from the menu always queues a beacon, even with the
     * automatic beacon off: the operator asked for it. The setting governs the
     * timer, not the button.
     */
    svc.beaconRequested = true;
    (void)now;
    return true;
}

bool aprs_sendMessage(const char *to, const char *text, uint32_t now)
{
    return aprs_mbox_queue(&svc.mbox, to, text, now);
}

static bool beaconDue(uint32_t now)
{
    if (!state.settings.aprs_beaconEn && !svc.beaconRequested)
        return false;

    if (svc.beaconRequested)
        return true;

    uint32_t interval = aprs_beaconIntervalS();
    if (interval == 0)
        return false;

    if (!svc.beaconValid)
        return true;

    return (uint32_t)(now - svc.lastBeaconMs) >= (interval * 1000u);
}

uint32_t aprs_beaconIntervalS(void)
{
    uint32_t s = state.settings.aprs_interval_s;
    if (s < 30u)
        s = 30u;
    return s;
}

bool aprs_txPending(uint32_t now)
{
    if (channelBusy(now))
        return false;

    if (beaconDue(now))
        return true;

    for (size_t i = 0; i < 4; i++)
    {
        if (svc.ack[i].used)
            return true;
    }

    return aprs_mbox_nextTx(&svc.mbox, now) != NULL;
}

bool aprs_txPrepare(char *info, size_t cap, aprs_pkt_t *kind, uint32_t now)
{
    svc.txMsg = NULL;

    if (channelBusy(now))
        return false;

    /*
     * Priority: acknowledgements first (they hold up the other station's
     * retry timer), then messages, then the beacon. A message is never
     * starved by a beacon.
     */
    for (size_t i = 0; i < 4; i++)
    {
        if (!svc.ack[i].used)
            continue;

        if (aprs_build_ack(info, cap, svc.ack[i].peer, svc.ack[i].msgNo) == 0)
            return false;

        *kind = APRS_PKT_ACK;
        return true;
    }

    aprs_out_msg_t *o = aprs_mbox_nextTx(&svc.mbox, now);
    if (o != NULL)
    {
        if (aprs_build_message(info, cap, o->peer, o->text, o->msgNo) == 0)
            return false;

        svc.txMsg = o;
        *kind = APRS_PKT_MESSAGE;
        return true;
    }

    if (beaconDue(now))
    {
        aprs_pos_t p = localPosition();
        bool havePos = (state.settings.aprs_lat_e4 != 0)
                    || (state.settings.aprs_lon_e4 != 0);

        if (havePos)
        {
            if (aprs_build_position(info, cap, &p, true) == 0)
                return false;
        }
        else if (aprs_build_status(info, cap, state.settings.aprs_comment) == 0)
        {
            return false;
        }

        *kind = APRS_PKT_BEACON;
        return true;
    }

    *kind = APRS_PKT_NONE;
    return false;
}

void aprs_txDone(aprs_pkt_t kind, uint32_t now)
{
    svc.lastTxMs = now;
    svc.txValid  = true;

    switch (kind)
    {
        case APRS_PKT_ACK:
            for (size_t i = 0; i < 4; i++)
            {
                if (svc.ack[i].used)
                {
                    svc.ack[i].used = false;
                    break;
                }
            }
            break;

        case APRS_PKT_MESSAGE:
            if (svc.txMsg != NULL)
            {
                aprs_mbox_markTx(&svc.mbox, svc.txMsg, now);
                svc.txMsg = NULL;
            }
            break;

        case APRS_PKT_BEACON:
            svc.lastBeaconMs = now;
            svc.beaconValid  = true;
            svc.beaconRequested = false;
            break;

        default:
            break;
    }

    aprs_mbox_tick(&svc.mbox, now);
}

/* -------------------------------------------------------------- status --- */

const aprs_station_list_t *aprs_stations(void)
{
    return &svc.stations;
}

aprs_mbox_t *aprs_mailbox(void)
{
    return &svc.mbox;
}

void aprs_clearStations(void)
{
    aprs_station_list_init(&svc.stations);
}

bool aprs_beaconEnabled(void)
{
    return state.settings.aprs_beaconEn;
}

uint16_t aprs_rxCount(void)
{
    return svc.rxCount;
}

uint32_t aprs_lastRxMs(void)
{
    return svc.lastRxMs;
}

bool aprs_hasUnread(void)
{
    return aprs_mbox_unread(&svc.mbox) > 0;
}
