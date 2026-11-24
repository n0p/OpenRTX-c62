/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/audio.h"
#include "interfaces/radio.h"
#include "protocols/M17/Datatypes.hpp"
#include "rtx/OpMode_M17.hpp"
#include "core/audio_codec.h"
#include <errno.h>
#include "core/gps.h"
#include "core/state.h"
#include "core/utils.h"
#include "rtx/rtx.h"

#ifdef PLATFORM_MOD17
#include "calibration/calibInfo_Mod17.h"
#include "interfaces/platform.h"

extern mod17Calib_t mod17CalData;
#endif

using namespace std;
using namespace M17;

OpMode_M17::OpMode_M17() : startRx(false), startTx(false), txBlocked(false),
                           locked(false), dataValid(false), extendedCall(false),
                           invertTxPhase(false), invertRxPhase(false),
                           rxAudioPath(-1), txAudioPath(-1), gpsTimer(0)
{

}

OpMode_M17::~OpMode_M17()
{
    disable();
}

void OpMode_M17::enable()
{
    codec_init();
    modulator.init();
    demodulator.init();
    locked       = false;
    dataValid    = false;
    extendedCall = false;
    startRx      = true;
    startTx      = false;
    txBlocked    = false;
}

void OpMode_M17::disable()
{
    startRx = false;
    startTx = false;
    platform_ledOff(GREEN);
    platform_ledOff(RED);

    /*
     * Stop the codec thread before its path is released.  A thread that finds
     * its path closed exits on its own, and then nobody joins it: its 16 KiB
     * of stack is malloc'd by startThread() and only stopThread() frees it.
     */
    codec_stop(rxAudioPath);
    codec_stop(txAudioPath);

    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    rxAudioPath = -1;
    txAudioPath = -1;

    /* Leaving the mode ends whatever transmission was open */
    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);

    codec_terminate();
    radio_disableRtx();
    modulator.terminate();
    demodulator.terminate();
}

void OpMode_M17::update(rtxStatus_t *const status, const bool newCfg)
{
    (void) newCfg;

    /* An aborted over does not key again until the operator lets go of the PTT */
    if(platform_getPttStatus() == false)
        txBlocked = false;
    #if defined(PLATFORM_MD3x0) || defined(PLATFORM_MDUV3x0)
    //
    // Invert TX phase for all MDx models.
    // Invert RX phase for MD-3x0 VHF and MD-UV3x0 radios.
    //
    const hwInfo_t* hwinfo = platform_getHwInfo();
    invertTxPhase = true;
    if(hwinfo->vhf_band == 1)
        invertRxPhase = true;
    else
        invertRxPhase = false;
    #elif defined(PLATFORM_MOD17)
    //
    // Get phase inversion settings from calibration.
    //
    invertTxPhase = (mod17CalData.bb_tx_invert == 1) ? true : false;
    invertRxPhase = (mod17CalData.bb_rx_invert == 1) ? true : false;
    #elif defined(PLATFORM_CS7000) || defined(PLATFORM_CS7000P) \
        || defined(PLATFORM_C62)
    invertTxPhase = true;
    #elif defined(PLATFORM_DM1701)
    invertTxPhase = true;
    invertRxPhase = true;
    #endif

    // Main FSM logic
    switch(status->opStatus)
    {
        case OFF:
            offState(status);
            break;

        case RX:
            rxState(status);
            break;

        case TX:
            txState(status);
            break;

        default:
            break;
    }

    // Led control logic
    switch(status->opStatus)
    {
        case RX:

            if(dataValid)
                platform_ledOn(GREEN);
            else
                platform_ledOff(GREEN);
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
}

void OpMode_M17::offState(rtxStatus_t *const status)
{
    radio_disableRtx();

    /* Covers every way into OFF: this is where a transmission ends */
    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);

    codec_stop(txAudioPath);
    audioPath_release(txAudioPath);

    if(startRx)
    {
        status->opStatus = RX;
        return;
    }

    if(platform_getPttStatus() && (status->txDisable == 0) && (txBlocked == false))
    {
        startTx = true;
        status->opStatus = TX;
        return;
    }

    // Sleep for 30ms if there is nothing else to do in order to prevent the
    // rtx thread looping endlessly and locking up all the other tasks
    sleepFor(0, 30);
}

void OpMode_M17::rxState(rtxStatus_t *const status)
{
    if(startRx)
    {
        demodulator.startBasebandSampling();

        radio_enableRx();

        startRx = false;
    }

    bool newData = demodulator.update(invertRxPhase);
    bool lock    = demodulator.isLocked();

    // Reset frame decoder when transitioning from unlocked to locked state.
    if((lock == true) && (locked == false))
    {
        // A new transmission opens its own audio path: end a held one
        codec_stop(rxAudioPath);
        audioPath_release(rxAudioPath);

        decoder.reset();
        locked = lock;
    }

    if(locked)
    {
        // Process new data
        if(newData)
        {
            auto  type    = decoder.decodeFrame(demodulator.getSoftFrame());
            auto  lsf     = decoder.getLsf();
            status->lsfOk = lsf.valid();

            if(status->lsfOk)
            {
                dataValid = true;

                // Retrieve stream source and destination data
                Callsign dst = lsf.getDestination();
                Callsign src = lsf.getSource();
                strncpy(status->M17_dst, dst, 10);
                
                // Copy source callsign (may be overridden for extended callsigns)
                strncpy(status->M17_src, src, 10);

                // Retrieve extended callsign data
                streamType_t streamType = lsf.getType();

                if(streamType.fields.encType == ENCRYPTION_NONE)
                {
                    meta_t& meta = lsf.metadata();

                    switch(streamType.fields.encSubType)
                    {
                        case META_EXTD_CALLSIGN:
                        {
                            extendedCall = true;
                            Callsign exCall1(meta.extended_call_sign.call1);
                            Callsign exCall2(meta.extended_call_sign.call2);

                            // The source callsign only contains the last link when
                            // receiving extended callsign data: store the first
                            // extended callsign in M17_src.
                            strncpy(status->M17_src,  exCall1, 10);
                            strncpy(status->M17_refl, exCall2, 10);
                            strncpy(status->M17_link, src, 10);
                            break;
                        }
                        case META_TEXT:
                        {
                            metaText.addBlock(meta);
                            const char* txt = metaText.getText();
                            if(txt != nullptr)
                                strncpy(status->M17_meta_text, txt, sizeof(status->M17_meta_text) - 1);
                            break;
                        }
                        default:
                            // M17_src already set above
                            break;
                    }
                }
                // M17_src already set above for non-encrypted streams

                // Check CAN on RX, if enabled.
                // If check is disabled, force match to true.
                bool canMatch =  (streamType.fields.CAN == status->can)
                              || (status->canRxEn == false);

                // Check if the destination callsign of the incoming transmission
                // matches with ours
                bool callMatch = (Callsign(status->source_address) == dst)
                               || dst.isSpecial();

                // Open audio path only if CAN and callsign match
                uint8_t pthSts = audioPath_getStatus(rxAudioPath);
                if((pthSts == PATH_CLOSED) && (canMatch == true) && (callMatch == true))
                {
                    rxAudioPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
                    pthSts = audioPath_getStatus(rxAudioPath);
                }

                // Extract audio data and sent it to codec
                if((type == FrameType::STREAM) && (pthSts == PATH_OPEN))
                {
                    // (re)start codec2 module if not already up
                    if(codec_running() == false)
                        codec_startDecode(rxAudioPath);

                    StreamFrame sf = decoder.getStreamFrame();
                    codec_pushFrame(sf.data(),     false);
                    codec_pushFrame(sf.data() + 8, false);
                }
            }
        }
    }

    locked = lock;

    if(platform_getPttStatus() && (txBlocked == false))
    {
        demodulator.stopBasebandSampling();
        locked = false;
        status->opStatus = OFF;
    }

    // Force invalidation of LSF data as soon as lock is lost (for whatever cause)
    if(locked == false)
    {
        status->lsfOk = false;
        dataValid     = false;
        extendedCall  = false;
        status->M17_meta_text[0] = '\0';
        status->M17_link[0] = '\0';
        status->M17_refl[0] = '\0';

        metaText.reset();

        // Let the codec play the audio already queued, unless leaving RX
        if((status->opStatus != RX) || codec_queueEmpty(rxAudioPath))
        {
            codec_stop(rxAudioPath);
            audioPath_release(rxAudioPath);
        }
    }
}

bool OpMode_M17::popFrame(uint8_t *dst)
{
    for(unsigned i = 0; i < TX_FRAME_WAIT_MS; i++)
    {
        if(codec_popFrame(dst, false) == 0)
            return true;

        /* The encoder thread stopped: no frame is ever going to arrive */
        if(codec_running() == false)
            return false;

        sleepFor(0, 1);
    }

    return false;
}

/**
 * Force the end of the over, from the supervisor thread.
 *
 * Only the two things that are safe from another thread: cancel the baseband
 * output stream -- which is what wakes this thread if it is blocked in it --
 * and stop the encoder, whose thread would otherwise keep filling a queue
 * nobody reads.  The audio paths and the modulator belong to the RTX thread and
 * are released by offState() when it leaves transmit.
 */
void OpMode_M17::abortTx()
{
    txBlocked = true;
    codec_abort();
    modulator.abort();
}

void OpMode_M17::txState(rtxStatus_t *const status)
{
    frame_t m17Frame;
    struct txGuard *tx = rtx_getTxGuard();

    if(startTx)
    {
        startTx = false;
        txg_enter(tx, (uint32_t)getTick());

        LinkSetupFrame lsf;

        lsf.clear();
        lsf.setSource(status->source_address);

        Callsign dst(status->destination_address);
        if(!dst.isEmpty())
            lsf.setDestination(dst);

        streamType_t type;
        type.fields.dataMode = DATAMODE_STREAM;     // Stream
        type.fields.dataType = DATATYPE_VOICE;      // Voice data
        type.fields.CAN      = status->can;             // Channel access number

        lsf.setType(type);

        if(strlen(state.settings.M17_meta_text) > 0) {
            metaText.setText(state.settings.M17_meta_text);
            metaText.getNextBlock(lsf.metadata());
        }

        if(state.settings.gps_enabled) {
            lsf.setGnssData(&state.gps_data, GNSS_STATION_HANDHELD);
            gpsTimer = 0;
        }

        encoder.reset();
        encoder.encodeLsf(lsf, m17Frame);

        txAudioPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
        if((txAudioPath < 0) || (codec_startEncode(txAudioPath) == false))
        {
            /* No encoder: keying the PA would transmit an empty stream and,
             * with nobody pushing frames, block this thread for ever */
            audioPath_release(txAudioPath);
            txAudioPath = -1;
            radio_disableRtx();
            txg_streamsFailed(tx, (uint32_t)getTick());
            startRx = true;
            status->opStatus = OFF;
            return;
        }

        /*
         * The baseband stream and the audio path come before the PA: if the
         * modulator cannot start (no baseband buffer, no SINK_RTX path) there
         * is nothing to transmit, and holding the PA would only splatter.
         */
        if(modulator.start() == false)
        {
            codec_stop(txAudioPath);
            audioPath_release(txAudioPath);
            txAudioPath = -1;
            radio_disableRtx();
            txg_streamsFailed(tx, (uint32_t)getTick());
            startRx = true;
            status->opStatus = OFF;
            return;
        }

        /* Both streams are up: the encoder reads the microphone, the modulator
         * writes the baseband. From here the supervisor expects samples. */
        txg_streamsOpen(tx, (uint32_t)getTick());

        radio_enableTx();
        txg_note(tx, TXG_EV_KEYED, (uint32_t)getTick());

        modulator.invertPhase(invertTxPhase);
        modulator.sendPreamble();
        /* The preamble is on the air: the sink has consumed its first block */
        txg_firstBlock(tx, (uint32_t)getTick());
        modulator.sendFrame(m17Frame);
        txg_block(tx, (uint32_t)getTick());
    }
    payload_t dataFrame;
    bool      lastFrame = false;
    bool      encoderDead = false;

    // Wait until there are 16 bytes of compressed speech, then send them
    if(!popFrame(dataFrame.data()) || !popFrame(dataFrame.data() + 8))
    {
        /*
         * The encoder died under us.  End the over here instead of waiting on a
         * condition that will never be signalled: the radio would stay keyed.
         */
        memset(dataFrame.data(), 0x00, dataFrame.size());
        lastFrame   = true;
        encoderDead = true;
        startRx     = true;
        status->opStatus = OFF;
        txg_note(tx, TXG_EV_ENCODER, (uint32_t)getTick());
    }

    if(platform_getPttStatus() == false)
    {
        lastFrame = true;
        startRx   = true;
        status->opStatus = OFF;
    }

    /* From here the tail is on the air: it has a deadline of its own */
    if(lastFrame)
        txg_finishing(tx, (uint32_t)getTick());

    encoder.encodeStreamFrame(dataFrame, m17Frame, lastFrame);
    modulator.sendFrame(m17Frame);
    txg_block(tx, (uint32_t)getTick());

    // After encoding a stream frame the encoder advances its LICH counter.
    // When it wraps back to zero a new superframe begins and the encoder
    // will accept an updated LSF.  Schedule the next meta-text block or
    // GPS update at this boundary so the new data is transmitted during
    // the upcoming superframe.
    if(encoder.superframeBoundary())
    {
        if(strlen(state.settings.M17_meta_text) > 0) {
            auto lsf = encoder.getCurrentLsf();
            metaText.getNextBlock(lsf.metadata());
            encoder.updateLsfData(lsf);
        }

        if(state.settings.gps_enabled) {
            gpsTimer++;

            if(gpsTimer >= GPS_UPDATE_TICKS) {
                auto lsf = encoder.getCurrentLsf();
                lsf.setGnssData(&state.gps_data, GNSS_STATION_HANDHELD);
                encoder.updateLsfData(lsf);
                gpsTimer = 0;
            }
        }
    }

    if(lastFrame)
    {
        encoder.encodeEotFrame(m17Frame);
        modulator.sendFrame(m17Frame);
        modulator.stop();

        txg_exit(tx, (uint32_t)getTick(),
                 encoderDead ? TXG_REASON_ENCODER : TXG_REASON_PTT);
    }
}

bool OpMode_M17::compareCallsigns(const std::string& localCs,
                                  const std::string& incomingCs)
{
    if((incomingCs == "ALL") || (incomingCs == "INFO") || (incomingCs == "ECHO"))
        return true;

    std::string truncatedLocal(localCs);
    std::string truncatedIncoming(incomingCs);

    int slashPos = localCs.find_first_of('/');
    if(slashPos <= 2)
        truncatedLocal = localCs.substr(slashPos + 1);

    slashPos = incomingCs.find_first_of('/');
    if(slashPos <= 2)
        truncatedIncoming = incomingCs.substr(slashPos + 1);

    if(truncatedLocal == truncatedIncoming)
        return true;

    return false;
}
