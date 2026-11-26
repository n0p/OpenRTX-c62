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
#include "core/psram_state.h"
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

/*
 * The two big tables of the service, in PSRAM: the list of stations heard
 * (32 entries with their comments) and the message mailbox, about 6 KB
 * together.  They are not part of `svc` because the struct also holds the
 * 20 ms audio block handed to audioStream_start(), which is DMA and has to
 * stay in SRAM.  Both are reset by aprs_service_init() every time the service
 * opens, which is what makes them deterministic in a NOLOAD section
 * (core/psram_state.h).
 */
static aprs_station_list_t stations PSRAM_STATE;
static aprs_mbox_t mbox PSRAM_STATE;

/* ------------------------------------------------------------- helpers --- */

static void ownCallsign(char *out, size_t cap)
{
    /*
     * The AX.25 source is the plaintext callsign from the settings plus the
     * APRS SSID, which is separate so that the FM/menu path keeps working
     * with a bare callsign. The base callsign is six characters in AX.25, and
     * ax25_addr_set() keeps exactly those, so the string is cut to the same
     * length here: a longer callsign in the settings would otherwise produce
     * an address that does not match what is transmitted.
     */
    snprintf(out, cap, "%.6s-%u", state.settings.callsign,
             (unsigned)(state.settings.aprs_ssid & 0x0Fu));
}

void aprs_ownCallsign(char *out, size_t cap)
{
    ownCallsign(out, cap);
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

/* ------------------------------------------------------------ settings --- */

/**
 * The first thing wrong with the station configuration, or "" when the radio
 * can transmit.  Every item is checked against what goes on the wire, not
 * against what the menu allowed: a callsign that is not an AX.25 address, an
 * SSID above 15, a path with a typo or a position outside the planet produce
 * frames that other stations cannot answer, and the operator has to be told
 * which one it is instead of watching a beacon that never appears.
 */
static const char *configProblem(void)
{
    if (!ax25_addrValid(state.settings.callsign))
        return "callsign: 1-6 chars, letters and digits";

    if (!ax25_ssidValid((int)state.settings.aprs_ssid))
        return "SSID: 0-15";

    if (!ax25_pathValid(state.settings.aprs_path, NULL))
        return "path: WIDE1-1,WIDE2-1 or a callsign";

    if (state.settings.aprs_lat_e4 < -900000 ||
        state.settings.aprs_lat_e4 >  900000)
        return "latitude: -90.0000..90.0000";

    if (state.settings.aprs_lon_e4 < -1800000 ||
        state.settings.aprs_lon_e4 >  1800000)
        return "longitude: -180.0000..180.0000";

    if (state.settings.aprs_table != '/' && state.settings.aprs_table != '\\')
        return "symbol table: / or \\";

    return "";
}

bool aprs_configValid(void)
{
    return configProblem()[0] == '\0';
}

const char *aprs_configError(void)
{
    return configProblem();
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

    aprs_station_update(&stations, &f, &info,
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

            if (aprs_mbox_onMessage(&mbox, src, info.text, info.msgNo,
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
            aprs_mbox_onAck(&mbox, src, info.msgNo,
                            info.type == APRS_INFO_REJ);
            break;

        default:
            break;
    }
}

/* --------------------------------------------------------------- RX API --- */

#ifdef SIM_INJECT
bool aprs_sim_rxFrame(const uint8_t *frame, size_t len, uint32_t nowMs)
{
    /* The frame the simulator hands over is the one the deframer would have
       delivered: addresses, control, PID, information field. onFrame() parses
       it again so that there is one path for the air and for the bench. */
    ax25_frame_t probe;
    uint8_t copy[AX25_MAX_INFO + 64u];

    if ((frame == NULL) || (len > sizeof(copy)))
        return false;
    if (len < AX25_MIN_FRAME)
        return false;

    memcpy(copy, frame, len);
    if (!ax25_parse(copy, len, &probe))
        return false;

    onFrame(copy, len, NULL);
    svc.lastRxMs = nowMs;
    return true;
}
#endif

void aprs_service_init(void)
{
    memset(&svc, 0, sizeof(svc));
    hdlc_deframer_init(&svc.deframer);
    aprs_station_list_init(&stations);
    aprs_mbox_init(&mbox);
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
    /*
     * The addressee is an AX.25 address with an optional SSID, and the station
     * itself has to be able to transmit: a message queued for a peer nobody
     * can address would only fill the outbox with retries.
     */
    if (!aprs_configValid())
        return false;

    if (!ax25_digiValid(to))
        return false;

    return aprs_mbox_queue(&mbox, to, text, now);
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
    if (!aprs_configValid())
        return false;

    if (channelBusy(now))
        return false;

    if (beaconDue(now))
        return true;

    for (size_t i = 0; i < 4; i++)
    {
        if (svc.ack[i].used)
            return true;
    }

    return aprs_mbox_nextTx(&mbox, now) != NULL;
}

bool aprs_txPrepare(char *info, size_t cap, aprs_pkt_t *kind, uint32_t now)
{
    svc.txMsg = NULL;

    if (!aprs_configValid())
        return false;

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

    aprs_out_msg_t *o = aprs_mbox_nextTx(&mbox, now);
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
                aprs_mbox_markTx(&mbox, svc.txMsg, now);
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

    aprs_mbox_tick(&mbox, now);
}

/* -------------------------------------------------------------- status --- */

const aprs_station_list_t *aprs_stations(void)
{
    return &stations;
}

aprs_mbox_t *aprs_mailbox(void)
{
    return &mbox;
}

void aprs_clearStations(void)
{
    aprs_station_list_init(&stations);
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
    return aprs_mbox_unread(&mbox) > 0;
}
