/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/radio.h"
#include "rtx/OpMode_FM.hpp"
#include "rtx/rtx.h"
#include "core/utils.h"
#include "core/state.h"
#include <stdio.h>

#ifdef CONFIG_APRS
#include "core/aprs_service.h"
#include "core/audio_stream.h"
#include "protocols/APRS/ax25.h"
#include "protocols/APRS/hdlc.h"
#include "protocols/APRS/afsk.h"
#include <string.h>

/*
 * APRS transmission parameters. The gain is in units of full scale of the
 * audio stream: the deviation it produces is set by the radio driver, and
 * -2 dB leaves room for the pre-emphasis shelf without clipping.
 */
static constexpr float    APRS_TX_GAIN     = 0.80f;
static constexpr float    APRS_TX_PREEMPH  = 0.5f;
static constexpr uint32_t APRS_TX_RATE     = 48000;
static constexpr size_t   APRS_TX_BLOCK    = 960;   /* 20 ms at 48 kHz */
static constexpr uint8_t  APRS_PRE_FLAGS   = 4;
static constexpr uint8_t  APRS_POST_FLAGS  = 2;

static int16_t aprsTxBuf[2 * APRS_TX_BLOCK];
#endif

#if defined(PLATFORM_TTWRPLUS)
#include "drivers/baseband/AT1846S.h"
#endif

/**
 * \internal
 * On MD-UV3x0 radios the volume knob does not regulate the amplitude of the
 * analog signal towards the audio amplifier but it rather serves to provide a
 * digital value to be fed into the HR_C6000 lineout DAC gain. We thus have to
 * provide the helper function below to keep the real volume level consistent
 * with the knob position.
 */
#if defined(PLATFORM_TTWRPLUS)
void _setVolume()
{
    static uint8_t oldVolume = 0xFF;
    uint8_t volume = platform_getVolumeLevel();

    if(volume == oldVolume)
        return;

    // AT1846S volume control is 4 bit
    AT1846S::instance().setRxAudioGain(volume / 16, volume / 16);
    oldVolume = volume;
}
#endif

OpMode_FM::OpMode_FM() : rfSqlOpen(false), sqlOpen(false), enterRx(true)
{
}

OpMode_FM::~OpMode_FM()
{
}

void OpMode_FM::enable()
{
    // When starting, close squelch and prepare for entering in RX mode.
    rfSqlOpen = false;
    sqlOpen   = false;
    enterRx   = true;
}

void OpMode_FM::disable()
{
    // Clean shutdown.
    platform_ledOff(GREEN);
    platform_ledOff(RED);
    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    radio_disableRtx();
    rfSqlOpen = false;
    sqlOpen   = false;
    enterRx   = false;
}

void OpMode_FM::update(rtxStatus_t *const status, const bool newCfg)
{
    (void) newCfg;

    #if defined(PLATFORM_TTWRPLUS)
    // Set output volume by changing the HR_C6000 DAC gain
    _setVolume();
    #endif

    // RX logic
    if(status->opStatus == RX)
    {
        // RF squelch mechanism: the squelch level is the S-level opening
        // it, S0 (open) to S9 + 20 dB, as on the S-meter
        rssi_t squelch = sLevelToRssi(status->sqlLevel);
        rssi_t rssi    = rtx_getRssi();

        // Provide a bit of hysteresis, only change state if the RSSI has
        // moved more than 1dBm on either side of the current squelch setting.
        if((rfSqlOpen == false) && (rssi > (squelch + 1))) rfSqlOpen = true;
        if((rfSqlOpen == true)  && (rssi < (squelch - 1))) rfSqlOpen = false;

        // Local flags for current RF and tone squelch status
        bool rfSql   = ((status->rxToneEn == 0) && (rfSqlOpen == true));
        bool toneSql = ((status->rxToneEn == 1) && radio_checkRxDigitalSquelch());

        // Audio control
        if((sqlOpen == false) && (rfSql || toneSql))
        {
            rxAudioPath = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
            if(rxAudioPath > 0) sqlOpen = true;
        }

        if((sqlOpen == true) && (rfSql == false) && (toneSql == false))
        {
            audioPath_release(rxAudioPath);
            sqlOpen = false;
        }

#ifdef CONFIG_APRS
        /*
         * APRS receive: a second path from the same discriminator into the
         * MCU, next to the one that plays on the speaker. The codec fans the
         * capture out to both, so opening the tap does not interrupt audio.
         * With decoding off nothing is opened and no sample is touched.
         */
        aprsReceiveStep(rfSql || toneSql);
#endif
    }
    else if((status->opStatus == OFF) && enterRx)
    {
        radio_disableRtx();

        radio_enableRx();
        status->opStatus = RX;
        enterRx = false;
    }

#ifdef CONFIG_APRS
    /*
     * APRS transmission. This is deliberately not the PTT path: a packet goes
     * out only when the service has one queued (a message from the menu or the
     * beacon timer), and a held PTT always wins, because voice is what the
     * PTT is for.
     *
     * Only the service knows whether there is something to send, so the
     * settings do not gate this: a message typed in the APRS screens goes out
     * even with decoding switched off (it just will not see the answer).
     */
    if ((status->opStatus == RX) && (!platform_getPttStatus()))
    {
        uint32_t now = (uint32_t)getTick();
        if(aprs_txPending(now))
        {
            char       info[128];
            aprs_pkt_t kind = APRS_PKT_NONE;
            if(aprs_txPrepare(info, sizeof(info), &kind, now))
            {
                aprsSendPacket(info);
                aprs_txDone(kind, (uint32_t)getTick());
            }
        }
    }
#endif

    // TX logic
    if(platform_getPttStatus() && (status->opStatus != TX) &&
                                  (status->txDisable == 0))
    {
        audioPath_release(rxAudioPath);
        radio_disableRtx();

        txAudioPath = audioPath_request(SOURCE_MIC, SINK_RTX, PRIO_TX);
        radio_enableTx();

        status->opStatus = TX;
    }

    if(!platform_getPttStatus() && (status->opStatus == TX))
    {
        audioPath_release(txAudioPath);
        radio_disableRtx();

        status->opStatus = OFF;
        enterRx = true;
        sqlOpen = false;  // Force squelch to be redetected.
    }

    // Led control logic
    switch(status->opStatus)
    {
        case RX:
            if(radio_checkRxDigitalSquelch())
            {
                platform_ledOn(GREEN);  // Red + green LEDs ("orange"): tone squelch open
                platform_ledOn(RED);
            }
            else if(rfSqlOpen)
            {
                platform_ledOn(GREEN);  // Green LED only: RF squelch open
                platform_ledOff(RED);
            }
            else
            {
                platform_ledOff(GREEN);
                platform_ledOff(RED);
            }

            break;

        case TX:
            platform_ledOff(GREEN);
            platform_ledOn(RED);
            break;

        default:
            platform_ledOff(GREEN);
            platform_ledOff(RED);
            break;
    }

    // Sleep thread for 30ms for 33Hz update rate
    sleepFor(0u, 30u);
}

bool OpMode_FM::rxSquelchOpen()
{
    return sqlOpen;
}

#ifdef CONFIG_APRS

void OpMode_FM::aprsReceiveStep(bool channelOpen)
{
    const bool want = state.settings.aprs_decode && channelOpen;

    if(want && (!aprs_rxActive()))
        aprs_rxBegin();

    if((!want) && aprs_rxActive())
    {
        aprs_rxEnd();
        return;
    }

    /*
     * Polling blocks for one 20 ms block, which is what paces the mode while
     * decoding: the 30 ms sleep at the end of update() is then redundant, but
     * harmless.
     */
    if(aprs_rxActive())
        aprs_rxPoll();
}

void OpMode_FM::aprsSendPacket(const char *info)
{
    /* Build the frame: our callsign and SSID, APRS as destination, the
     * digipeater path from the settings. */
    char src[12];
    /* Six characters of base callsign and two of SSID: the AX.25 limit, the
     * same cut ax25_addr_set() makes. */
    snprintf(src, sizeof(src), "%.6s-%u", state.settings.callsign,
             (unsigned)(state.settings.aprs_ssid & 0x0Fu));

    ax25_frame_t f;
    memset(&f, 0, sizeof(f));
    ax25_addr_set(&f.dst, "APRS");
    ax25_addr_set(&f.src, src);
    f.nDigis  = ax25_parse_path(state.settings.aprs_path, f.digi,
                                AX25_MAX_DIGIS);
    f.info    = (const uint8_t *)info;
    f.infoLen = strlen(info);

    uint8_t frame[HDLC_MAX_LEN];
    size_t  len = ax25_build_ui(frame, sizeof(frame), &f);
    if(len == 0)
        return;

    /*
     * The radio is going to be keyed: both receive paths go away, the packet
     * goes out, and the mode returns to RX afterwards.
     */
    if(aprs_rxActive())
        aprs_rxEnd();
    audioPath_release(rxAudioPath);
    sqlOpen = false;    /* force the squelch to be re-detected on return */

    pathId txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if(txPath < 0)
    {
        radio_disableRtx();
        return;
    }

    streamId txStream = audioStream_start(txPath, aprsTxBuf,
                                          2u * APRS_TX_BLOCK, APRS_TX_RATE,
                                          STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if(txStream < 0)
    {
        audioPath_release(txPath);
        radio_disableRtx();
        return;
    }

    radio_enableTx();
    platform_ledOn(RED);

    afsk_mod_t mod;
    afsk_mod_init(&mod, APRS_TX_RATE, APRS_TX_PREEMPH);
    afsk_mod_setGain(&mod, APRS_TX_GAIN);

    hdlc_framer_t tx;
    hdlc_tx_start(&tx, frame, len, APRS_PRE_FLAGS, APRS_POST_FLAGS);

    /* TXDELAY: the transmitter is keyed and the line silent before the first
     * flag, 300 ms by default, which is what the far end needs to open its
     * squelch and settle its AGC. */
    uint32_t silence = (APRS_TX_RATE * state.settings.aprs_txdelay_ms) / 1000u;

    int16_t *buf = outputStream_getIdleBuffer(txStream);
    size_t   fill = 0;
    bool     done = false;

    /* Two extra blocks of tail, so the last flag leaves the modulator */
    uint32_t tail = 2u * APRS_TX_BLOCK;

    while(!done)
    {
        while(fill + mod.samplesPerBit <= APRS_TX_BLOCK)
        {
            if(silence > 0)
            {
                size_t n = (silence < mod.samplesPerBit) ? silence
                                                         : mod.samplesPerBit;
                memset(&buf[fill], 0, n * sizeof(int16_t));
                fill    += n;
                silence -= n;
                continue;
            }

            if(hdlc_tx_finished(&tx))
            {
                if(tail == 0)
                {
                    done = true;
                    break;
                }
                memset(&buf[fill], 0, mod.samplesPerBit * sizeof(int16_t));
                fill += mod.samplesPerBit;
                tail -= mod.samplesPerBit;
                continue;
            }

            afsk_mod_bit(&mod, hdlc_tx_bit(&tx), &buf[fill]);
            fill += mod.samplesPerBit;
        }

        if(fill > 0)
        {
            if(outputStream_sync(txStream, true) == false)
                break;

            buf  = outputStream_getIdleBuffer(txStream);
            fill = 0;
        }
    }

    audioStream_stop(txStream);
    audioPath_release(txPath);
    radio_disableRtx();
    platform_ledOff(RED);
}

#endif /* CONFIG_APRS */
