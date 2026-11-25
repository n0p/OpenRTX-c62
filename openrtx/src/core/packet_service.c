/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Packet modem service. See core/packet_service.h for what it is and
 * docs/KISS.md for the protocol, the modes and the measurements.
 *
 * Threading, which is the only subtle part:
 *
 *  - the service thread owns the KISS decoder, the modem engine, the transmit
 *    queue and the audio paths it opens;
 *  - it transmits only while the operator is not pressing the PTT and the rtx
 *    thread is in RX, it keys the radio for one frame and gives it back;
 *  - the UI reads the counters through packet_service_status(), which copies
 *    them with the state mutex held by nobody: every field is a 32-bit
 *    counter or a small enum, so a reader can never see a torn value, and a
 *    frame that arrives while the page is being drawn shows up on the next
 *    poll.
 */
#include "core/packet_service.h"
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/state.h"
#include "core/settings.h"
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/APRS/ax25.h"
#include "protocols/KISS/kiss.h"
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>

#define PKT_FS            48000u   /* the TX path and the RX tap rate     */
#define PKT_BLOCK         480u     /* 10 ms of audio per stream buffer    */
#define PKT_QUEUE         3u       /* frames waiting for the channel      */
#define PKT_THREAD_STACK  3072
#define PKT_THREAD_PRIO   6

/* Name kept short: the linker map is read by hand often enough */
static struct
{
    /* configuration, mirrored from the settings and from KISS */
    bool        enabled;
    kiss_port_t port;
    bool        portBound;

    /* KISS */
    kiss_dec_t  dec;
    uint8_t     out[KISS_MAX_PAYLOAD * 2u + 8u];

    /* parameters in force: KISS 10 ms units, as the wire has them */
    uint8_t     txDelay;      /* 10 ms    */
    uint8_t     persist;
    uint8_t     slotTime;     /* 10 ms    */
    uint8_t     txTail;       /* 10 ms    */
    bool        fullDuplex;

    /* transmit queue */
    struct
    {
        uint8_t data[HDLC_MAX_LEN];
        size_t  len;
    } queue[PKT_QUEUE];
    uint8_t     qHead, qCount;
    uint32_t    txFrames, txDropped, txErrors;

    /* receive */
    uint32_t    rxFrames, rxFcs;

    /* audio */
    pathId      rxPath;
    streamId    rxStream;
    bool        rxOpen;
    int16_t     rxBuf[2u * PKT_BLOCK];
    bool        carrier;

    /* channel access */
    uint32_t    nextSlotMs;
    bool        slotArmed;

    pkt_engine_t engine;
    bool         engineReady;
    uint8_t      modem;
} svc;

static struct k_thread thread;
static K_THREAD_STACK_DEFINE(threadStack, PKT_THREAD_STACK);
static bool started;

/* ---------------------------------------------------------- transport --- */

static size_t portRead(uint8_t *buf, size_t cap)
{
    if (!svc.portBound || (svc.port.read == NULL))
        return 0;
    return svc.port.read(buf, cap, svc.port.user);
}

static void portWrite(const uint8_t *buf, size_t len)
{
    if (!svc.portBound || (svc.port.write == NULL) || (len == 0))
        return;
    if (!svc.port.write(buf, len, svc.port.user))
        svc.txErrors++;
}

void packet_service_bind(const kiss_port_t *port)
{
    if (port == NULL)
    {
        svc.portBound = false;
        return;
    }
    svc.port      = *port;
    svc.portBound = true;
}

/* ------------------------------------------------------------- engine --- */

static void onFrame(const uint8_t *payload, size_t len, void *user);

static bool engineStart(uint8_t modem)
{
    if (!pkt_engine_init(&svc.engine, (pkt_modem_t)modem, PKT_FS))
        return false;

    svc.modem       = modem;
    svc.engineReady = true;

    /* Payload modes have no demodulator but they do deliver frames through the
     * engine's callback, so the receiver has to be armed for all of them. */
    pkt_rx_begin(&svc.engine, onFrame, NULL);
    return true;
}

static void rxClose(void)
{
    if (!svc.rxOpen)
        return;

    audioStream_terminate(svc.rxStream);
    audioPath_release(svc.rxPath);
    svc.rxStream = -1;
    svc.rxPath   = -1;
    svc.rxOpen   = false;
    svc.carrier  = false;
}

static bool rxOpen(void)
{
    if (svc.rxOpen)
        return true;

    svc.rxPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    if (svc.rxPath < 0)
        return false;

    svc.rxStream = audioStream_start(svc.rxPath, svc.rxBuf, 2u * PKT_BLOCK,
                                     PKT_FS, STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (svc.rxStream < 0)
    {
        audioPath_release(svc.rxPath);
        svc.rxPath = -1;
        return false;
    }

    pkt_rx_begin(&svc.engine, onFrame, NULL);
    svc.rxOpen = true;
    return true;
}

/* ------------------------------------------------------------ receive --- */

static void onFrame(const uint8_t *payload, size_t len, void *user)
{
    (void)user;

    svc.rxFrames++;
    svc.rxFcs = svc.engine.rxFcsErr;

    size_t n = kiss_enc_data(svc.out, sizeof(svc.out), 0, payload, len, false);
    portWrite(svc.out, n);
}

static void receiveStep(void)
{
    if (!rxOpen())
        return;

    dataBlock_t block = inputStream_getData(svc.rxStream);
    if ((block.data == NULL) || (block.len == 0))
        return;

    pkt_rx_block(&svc.engine, block.data, block.len);
    svc.carrier = pkt_rx_signal(&svc.engine);
}

/* ----------------------------------------------------------- transmit --- */

/**
 * Put one frame on the air. Blocking for the length of the frame: the caller
 * is the service thread and there is nothing else it should be doing.
 *
 * The audio goes out through the codec's MCU source, the same route the other
 * data modes use, so the FM modulator only sees a baseband signal.
 */
static bool transmit(const uint8_t *frame, size_t len)
{
    if ((pkt_modem_info((pkt_modem_t)svc.modem)->caps & PKT_CAP_TX) == 0)
        return false;

    /* The microphone path is the PTT's; never fight it */
    if (platform_getPttStatus())
        return false;

    rxClose();

    pathId txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;

    streamId txStream = audioStream_start(txPath, svc.rxBuf, 2u * PKT_BLOCK,
                                          PKT_FS,
                                          STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (txStream < 0)
    {
        audioPath_release(txPath);
        return false;
    }

    if (!pkt_tx_begin(&svc.engine, frame, len, svc.txDelay * 10u,
                      svc.txTail * 10u))
    {
        audioStream_terminate(txStream);
        audioPath_release(txPath);
        return false;
    }

    radio_enableTx();
    platform_ledOn(RED);

    int16_t *buf  = outputStream_getIdleBuffer(txStream);
    bool     done = false;

    while (!done)
    {
        size_t w = pkt_tx_block(&svc.engine, buf, PKT_BLOCK);

        if (w == 0)
        {
            /* Nothing more to modulate: two blocks of silence so the last
             * flag leaves the codec before the PA drops */
            memset(buf, 0, sizeof(int16_t) * PKT_BLOCK);
            w = PKT_BLOCK;
            done = true;
        }
        else if (w < PKT_BLOCK)
        {
            memset(&buf[w], 0, sizeof(int16_t) * (PKT_BLOCK - w));
        }

        if (!outputStream_sync(txStream, true))
        {
            svc.txErrors++;
            break;
        }

        buf = outputStream_getIdleBuffer(txStream);
    }

    audioStream_stop(txStream);
    audioPath_release(txPath);
    radio_disableRtx();
    radio_enableRx();
    platform_ledOff(RED);

    return done;
}

/* --------------------------------------------------------- KISS input --- */

static void handleParam(uint8_t cmd, uint8_t value)
{
    switch (cmd)
    {
        case KISS_CMD_TXDELAY:
            svc.txDelay = value;
            break;

        case KISS_CMD_PERSIST:
            svc.persist = value;
            break;

        case KISS_CMD_SLOTTIME:
            svc.slotTime = value;
            break;

        case KISS_CMD_TXTAIL:
            svc.txTail = value;
            break;

        case KISS_CMD_FULLDUP:
            svc.fullDuplex = value != 0;
            break;

        default:
            break;
    }
}

static void queueFrame(const uint8_t *data, size_t len)
{
    if ((len == 0) || (len > state.settings.packet_maxframe))
    {
        svc.txDropped++;
        return;
    }

    if (svc.qCount >= PKT_QUEUE)
    {
        svc.txDropped++;
        return;
    }

    uint8_t slot = (uint8_t)((svc.qHead + svc.qCount) % PKT_QUEUE);
    memcpy(svc.queue[slot].data, data, len);
    svc.queue[slot].len = len;
    svc.qCount++;
}

static void pumpKiss(void)
{
    uint8_t buf[64];
    size_t  n = portRead(buf, sizeof(buf));

    for (size_t i = 0; i < n; i++)
    {
        switch (kiss_dec_byte(&svc.dec, buf[i]))
        {
            case KISS_EV_DATA:
                queueFrame(svc.dec.msg.data, svc.dec.msg.len);
                break;

            case KISS_EV_PARAM:
                if (svc.dec.msg.len >= 1)
                    handleParam(svc.dec.msg.cmd, svc.dec.msg.data[0]);
                break;

            case KISS_EV_RETURN:
                /* 0xFF: the host leaves KISS. The radio stays a modem with
                 * the setting off, which is what the page shows. */
                svc.enabled = false;
                break;

            case KISS_EV_UNKNOWN:
            case KISS_EV_OVERFLOW:
                svc.txDropped++;
                break;

            default:
                break;
        }
    }
}

/* ------------------------------------------------------ channel access --- */

/**
 * CSMA, as TNC2 implements it: in half duplex, wait one slot time and
 * transmit with probability PERSIST/256, otherwise wait another slot. In full
 * duplex the host is assumed to know what it is doing and there is no sense.
 */
static bool channelFree(uint32_t nowMs)
{
    if (svc.fullDuplex)
        return true;

    if (!svc.slotArmed)
    {
        svc.nextSlotMs = nowMs + (svc.slotTime * 10u);
        svc.slotArmed  = true;
        return false;
    }

    if ((int32_t)(nowMs - svc.nextSlotMs) < 0)
        return false;

    /* New slot: roll the dice once per slot */
    svc.nextSlotMs = nowMs + (svc.slotTime * 10u);
    return ((uint32_t)(k_cycle_get_32() >> 16) & 0xFFu) < svc.persist;
}

static void txStep(uint32_t nowMs)
{
    if (svc.qCount == 0)
    {
        svc.slotArmed = false;
        return;
    }

    if (!svc.engineReady || (svc.modem == PKT_MODEM_NXDN) ||
        (svc.modem == PKT_MODEM_DMRIP))
        return;     /* receive-only modes: the queue is never drained */

    if (!channelFree(nowMs))
        return;

    uint8_t slot = svc.qHead;
    bool ok = transmit(svc.queue[slot].data, svc.queue[slot].len);

    svc.qHead = (uint8_t)((svc.qHead + 1u) % PKT_QUEUE);
    svc.qCount--;

    if (ok)
        svc.txFrames++;
    else
        svc.txErrors++;

    svc.slotArmed = false;
}

/* -------------------------------------------------------------- thread --- */

static void packetMain(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;

    while (true)
    {
        if (!svc.enabled)
        {
            rxClose();
            sleepFor(0u, 100u);
            continue;
        }

        pumpKiss();

        /* The payload modes are fed by their own decoders, not by the tap */
        if ((svc.modem == PKT_MODEM_AFSK1200) || (svc.modem == PKT_MODEM_AFSK9600))
            receiveStep();

        txStep((uint32_t)getTick());

        /* 10 ms: one audio block, which is what receiveStep consumes */
        sleepFor(0u, 10u);
    }
}

/* ---------------------------------------------------------------- API --- */

void packet_service_init(void)
{
    if (started)
        return;

    memset(&svc, 0, sizeof(svc));

    svc.rxPath   = -1;
    svc.rxStream = -1;

    kiss_dec_init(&svc.dec, false);

    svc.enabled    = state.settings.packet_enable;
    svc.txDelay    = (uint8_t)(state.settings.packet_txdelay_ms / 10u);
    svc.persist    = state.settings.packet_persist;
    svc.slotTime   = (uint8_t)(state.settings.packet_slottime_ms / 10u);
    svc.txTail     = (uint8_t)(state.settings.packet_txtail_ms / 10u);
    svc.fullDuplex = state.settings.packet_fulldup != 0;

    engineStart(state.settings.packet_modem);

    k_thread_create(&thread, threadStack, K_THREAD_STACK_SIZEOF(threadStack),
                    packetMain, NULL, NULL, NULL, PKT_THREAD_PRIO, 0,
                    K_NO_WAIT);

    started = true;
}

bool packet_service_enabled(void)
{
    return svc.enabled;
}

void packet_service_setEnabled(bool on)
{
    svc.enabled = on;
    state.settings.packet_enable = on;
}

bool packet_service_setModem(uint8_t modem)
{
    if (modem >= PKT_MODEM_COUNT)
        return false;

    rxClose();

    if (!engineStart(modem))
        return false;

    state.settings.packet_modem = modem;
    return true;
}

void packet_service_setParam(uint8_t cmd, uint8_t value)
{
    handleParam(cmd, value);

    /* A value set from the page is a preference: store the ones that are */
    switch (cmd)
    {
        case KISS_CMD_TXDELAY:
            state.settings.packet_txdelay_ms = (uint16_t)value * 10u;
            break;
        case KISS_CMD_PERSIST:
            state.settings.packet_persist = value;
            break;
        case KISS_CMD_SLOTTIME:
            state.settings.packet_slottime_ms = (uint16_t)value * 10u;
            break;
        case KISS_CMD_TXTAIL:
            state.settings.packet_txtail_ms = (uint16_t)value * 10u;
            break;
        case KISS_CMD_FULLDUP:
            state.settings.packet_fulldup = (value != 0) ? 1 : 0;
            break;
        default:
            break;
    }
}

void packet_service_status(packet_status_t *out)
{
    if (out == NULL)
        return;

    out->enabled    = svc.enabled;
    out->txActive   = svc.engineReady && pkt_tx_busy(&svc.engine);
    out->carrier    = svc.carrier;
    out->modem      = svc.modem;
    out->modemId    = (pkt_modem_t)svc.modem;
    out->portOpen   = svc.portBound;
    out->txFrames   = svc.txFrames;
    out->rxFrames   = svc.rxFrames;
    out->rxFcs      = svc.rxFcs;
    out->txDropped  = svc.txDropped;
    out->txErrors   = svc.txErrors;
    out->queued     = svc.qCount;
    out->txDelay10ms = svc.txDelay;
    out->persist    = svc.persist;
    out->slotTime10ms = svc.slotTime;
    out->txTail10ms = svc.txTail;
    out->fullDuplex = svc.fullDuplex;
}

void packet_service_clearCounters(void)
{
    svc.txFrames  = 0;
    svc.rxFrames  = 0;
    svc.rxFcs     = 0;
    svc.txDropped = 0;
    svc.txErrors  = 0;
    svc.dec.frames = 0;
    svc.dec.bad    = 0;
}

bool packet_service_sendNow(const uint8_t *payload, size_t len)
{
    if ((payload == NULL) || (len == 0))
        return false;

    queueFrame(payload, len);
    return true;
}

/* ------------------------------------------------------- payload hooks --- */

void packet_service_nxdnPayload(const uint8_t *l3, size_t len)
{
    if ((!svc.enabled) || (svc.modem != PKT_MODEM_NXDN) || !svc.engineReady)
        return;

    pkt_rx_payload(&svc.engine, l3, len);
}

void packet_service_dmrPayload(const uint8_t *data, size_t len)
{
    if ((!svc.enabled) || (svc.modem != PKT_MODEM_DMRIP) || !svc.engineReady)
        return;

    pkt_rx_payload(&svc.engine, data, len);
}
