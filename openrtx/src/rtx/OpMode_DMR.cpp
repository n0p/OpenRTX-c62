/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DMR operating mode of this tree, on the protocol package (protocols/DMR/).
 *
 * Receive: the radio's discriminator at 48 kHz goes straight into
 * dmr_demod_feed_disc(); the package's demodulator cuts the bursts, calls
 * dmr_proto_frame() on each and the protocol layer drives the voice queue,
 * the call log, the short messages (their inbox) and Tier III by itself.  The
 * mode has nothing to decode: it converts the samples, bridges the package's
 * inbox into core/messaging and keeps the UI's snapshot (DmrStatus.c).
 *
 * Transmit: the package's frame builders (dmr_tx.c) and modulator
 * (dmr_mod_dibits()) render one 30 ms slot per pass, the call laid out as a
 * mobile station: two voice LC headers, voice bursts in superframes of six,
 * the terminator.  The slot cadence is the blocking write to the output
 * stream, as in the old chain.  The air delay of the audio path (the
 * measured constant of dmr_glue.h) is taken out of the repeater alignment
 * by delaying the whole call by that many samples before the first burst.
 *
 * Everything below the mode is the package's; what is ours is marked.
 */

#include <string.h>
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "core/mode_stats.h"
#include "protocols/DMR/DmrStatus.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_glue.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_voice.h"
#include "rtx/OpMode_DMR.hpp"
#include "rtx/DmrKey.h"
#include "core/state.h"
#include "core/utils.h"

#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#endif

#ifdef CONFIG_DMR_TRUNK
#include "protocols/DMR/trunk/dmr_trunk.h"
#endif

#ifdef CONFIG_DMR_SMS
#include "core/messaging.h"
#include "protocols/DMR/dmr_sms.h"
#endif

/* Samples dropped after starting or retuning: 100 ms */
#define SETTLE_SAMPLES 4800

/* Received samples converted to float per call (stack, 2 KiB) */
#define FLOAT_CHUNK 512

#ifdef CONFIG_DMR_TX
/* PCM the encoder eats per frame */
#define VOCODER_PCM_SAMPLES 160
/* Frames the queue takes per slot: 1.5 is the average, two keeps up */
#define VOCODER_FRAMES_PER_SLOT 2
/* Voice bursts of a superframe (A..F) */
#define SUPERFRAME_BURSTS 6
/* Guard around the 132 symbols of a burst inside the 144 of a slot */
#define SLOT_GUARD_SYMS ((DMR_HALF_SAMPLES / DMR_TX_SPS - DMR_TX_BURST_DIBITS) / 2)
/*
 * Transmit level: the modulator wants the deviation of a run of +1.  The
 * radio driver scales the baseband to +-1.944 kHz for DMR symbols +-3 (it
 * applies its own gain, as for M17); this is the same figure the old chain
 * used (DMR_TX_GAIN) so that the deviation on the air does not change when
 * the protocol layer does.
 */
#define DMR_TX_LEVEL (23000.0f * 0.3165f * 1944.0f / 2400.0f)
/* The C62 transmitter inverts, as for M17 */
#define DMR_TX_INVERT true
/* Voice LC headers at the start of a call */
#define DMR_TX_HEADERS 2
/* Bursts of silence after the terminator before the PA goes off */
#define TX_TAIL_SLOTS 2
#endif

OpMode_DMR::OpMode_DMR()
    : txBlocked(false)
    , startRx(false)
    , dataErrors(0)
    , settle(SETTLE_SAMPLES)
    , basebandPath(-1)
    , basebandId(-1)
#ifdef CONFIG_DMR_SMS
    , smsSerial(0)
    , selfId(0)
    , smsTxId(-1)
    , smsDataSent(false)
#endif
#ifdef CONFIG_DMR_TX
    , txRunning(false)
    , txFirstBlock(false)
    , txIsData(false)
    , txEnding(false)
    , txStage(0)
    , txVoiceBurst(0)
    , txAligned(false)
    , txSaidStream(false)
    , txSilence(0)
    , micPath(-1)
    , micId(-1)
    , txPath(-1)
    , txId(-1)
    , frameHead(0)
    , frameCount(0)
#endif
#ifdef CONFIG_DMR_TRUNK
    , trunkHz(0)
#endif
{
#ifdef CONFIG_DMR_TX
    memset(&tx, 0, sizeof(tx));
    memset(&txMod, 0, sizeof(txMod));
    memset(txBurst, 0, sizeof(txBurst));
    memset(frameQueue, 0, sizeof(frameQueue));
    memset(txSamples, 0, sizeof(txSamples));
    memset(rxSamples, 0, sizeof(rxSamples));
    memset(micPcm, 0, sizeof(micPcm));
#endif
}

OpMode_DMR::~OpMode_DMR()
{
    disable();
}

void OpMode_DMR::reset()
{
    settle = SETTLE_SAMPLES;
    dataErrors = 0;
    dmr_demod_reset();
    dmr_proto_reset();
    dmr_voice_reset();
    dmrStatus_reset();

    /* Ours: the mode's row of the stats page (core/mode_stats.h) */
    statsFrames = 0;
    statsValid = 0;
    statsOverruns = 0;
    modeStats_resync(MS_MODE_DMR);
}

void OpMode_DMR::enable()
{
    if (!dmr_demod_init())
        return; /* no sample ring: the mode stays silent but does not crash */

    /* Ours: the records the package reads, from the channel and settings */
    dmrGlue_reset();
    c62_dmr.id = state.settings.dmr_id;
    c62_dmr.cc = (uint8_t)(state.channel.dmr.rxColorCode & 0x0Fu);
    c62_dmr.slot = state.settings.dmr_timeslot;
    c62_dmr.tg = state.settings.dmr_dest_id;
    c62_dmr.privateCall =
        (state.settings.dmr_dest_type == DMR_DEST_USER);
    c62_dmr.txAirDelayUs = state.settings.dmr_tx_air_delay_us != 0
                              ? state.settings.dmr_tx_air_delay_us
                              : dmrGlue_airDelayUs();

    reset();
    dmr_voice_set_pref((state.settings.dmr_timeslot == 2) ? 1 : 0);
#ifdef CONFIG_DMR_SMS
    dmrSms_setOwnId(state.settings.dmr_id);
    selfId = state.settings.dmr_id;
    smsSerial = 0;
#endif
    dmrSpeech_start();
    startRx = true;
    txBlocked = false;

#ifdef CONFIG_DMR_TRUNK
    /* Ours: where the engine starts, from the channel it was selected on */
    if (c62_t3.mode != C62_T3_OFF) {
        dmrTrunk_start(state.channel.rx_frequency, state.channel.tx_frequency);
        trunkHz = state.channel.rx_frequency;
    }
#endif
}

void OpMode_DMR::disable()
{
    startRx = false;
#ifdef CONFIG_DMR_TX
    stopTx();
#endif
    /* Leaving the mode ends whatever transmission was open */
    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);
    stopSampling();
    dmrSpeech_stop();
#ifdef CONFIG_DMR_TRUNK
    dmrTrunk_stop();
#endif
    radio_disableRtx();
    platform_ledOff(GREEN);
    dmrStatus_clear();
}

void OpMode_DMR::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, rxSamples, 2 * DMR_HALF_SAMPLES,
                                   48000, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DMR::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

bool OpMode_DMR::rxSquelchOpen()
{
    return dmrSpeech_playing();
}

/*
 * Receive: convert and feed. The package does the rest; what it decoded is
 * read back through its snapshots (DmrStatus.c) and its message inbox.
 */
void OpMode_DMR::feed(const dataBlock_t &block)
{
    size_t n = block.len;
    const stream_sample_t *in = block.data;

    /*
     * Ours: the block cost and level of the stats page.  The frame counts are
     * the package's own (dmr_proto_get_stats), read as deltas per block so the
     * page keeps the meaning it had with the old chain.
     */
    modeStats_blockStart(MS_MODE_DMR);
    for (size_t i = 0; i < n; i += FLOAT_CHUNK) {
        float chunk[FLOAT_CHUNK];
        const size_t m = (n - i < FLOAT_CHUNK) ? n - i : FLOAT_CHUNK;
        for (size_t k = 0; k < m; k++)
            chunk[k] = (float)in[i + k];
        dmr_demod_feed_disc(chunk, (int)m);
    }
    modeStats_block(MS_MODE_DMR, block.data, n);

    dmr_proto_stats_t st;
    dmr_proto_get_stats(&st);
    const uint32_t frames = st.frames - statsFrames;
    const uint32_t valid = st.frames_valid - statsValid;

    if (frames > 0) {
        modeStats_frames(MS_MODE_DMR, frames);
        modeStats_valid(MS_MODE_DMR, valid);
        if (frames > valid)
            modeStats_badCrc(MS_MODE_DMR, frames - valid);
    }
    statsFrames = st.frames;
    statsValid = st.frames_valid;

    dmr_demod_status_t ds;
    dmr_demod_get_status(&ds);
    if (ds.overruns > statsOverruns) {
        modeStats_lost(MS_MODE_DMR, ds.overruns - statsOverruns);
        statsOverruns = ds.overruns;
    }

    modeStats_rssi(MS_MODE_DMR, (int)radio_getRssi());

    dmrStatus_update();
#ifdef CONFIG_DMR_SMS
    smsPoll();
#endif
#ifdef CONFIG_DMR_TRUNK
    if (dmrTrunk_running())
        dmrTrunk_level((int)radio_getRssi());
#endif
}

#ifdef CONFIG_DMR_SMS
/*
 * Ours: the package keeps its own inbox (dmrSms_*, DMR_SMS_INBOX messages);
 * the UI reads core/messaging.  Every message the DMR side marks unread is
 * copied into the shared store once and marked read there, so both sides
 * agree on what has been shown.
 */
void OpMode_DMR::smsPoll()
{
    const uint32_t serial = dmrSms_serial();

    if (serial == smsSerial)
        return;
    smsSerial = serial;

    for (unsigned i = 0; i < dmrSms_count(); i++) {
        struct dmrSmsMsg msg;

        if (!dmrSms_get(i, &msg) || !msg.unread)
            continue;
        messaging_received(MSG_TRANSPORT_DMR, msg.src, msg.dst, msg.text,
                           false, (uint32_t)getTick());
        dmrSms_markRead(i);
    }

    /* A TMS acknowledgement of ours: the message is delivered */
    if (dmrSms_txState(NULL, NULL) == DMR_SMS_TX_ACKED)
        messaging_confirmed(0, 0, (uint32_t)getTick());
}

/**
 * Ours: the oldest message the UI queued in core/messaging, handed to the
 * package's list (dmrSms_send) with the format the operator picked.  One
 * message per data call; the package takes it from there, one burst per
 * TDMA frame.
 */
void OpMode_DMR::smsStartTx(const rtxStatus_t *status)
{
    (void)status;

    const struct message *msg = messaging_head(MSG_TRANSPORT_DMR);

    if (msg == NULL)
        return;

    /* A DMR destination is a 24-bit radio or talkgroup ID */
    if ((msg->dst == 0) || (msg->dst > 0xFFFFFFu)) {
        messaging_fail(msg->id, (uint32_t)getTick());
        return;
    }

    const uint32_t dst = msg->dst;
    const int begun = messaging_begin(MSG_TRANSPORT_DMR);

    if (begun < 0)
        return;

    msg = messaging_find(begun);
    if (msg == NULL)
        return;

    if (!dmrSms_send(dst, !c62_dmr.privateCall,
                     dmrGlue_smsFormat(state.settings.sms_format), msg->text,
                     c62_dmr.id)) {
        messaging_done(begun, false, 0, (uint32_t)getTick());
        return;
    }

    smsTxId = begun;
    smsDataSent = false;
}
#endif /* CONFIG_DMR_SMS */

#ifdef CONFIG_DMR_TX
/**
 * How many samples of silence to emit before the first burst: the air delay
 * of the audio path, taken modulo one TDMA frame, so the bursts land in the
 * repeater's slots rather than a fixed number of milliseconds late.
 *
 * Only through a repeater (a channel whose TX and RX frequencies differ):
 * simplex has no one to align with, and delaying it would only add lag.
 */
size_t OpMode_DMR::txPreRoll()
{
    if (!txAligned)
        return 0;

    /* Ours: the arithmetic is in dmr_glue.c, so the host tests can check it */
    return (size_t)dmrGlue_preRollSamples();
}

/**
 * Configure the chain for this channel and open the baseband stream.
 */
bool OpMode_DMR::startBaseband(const rtxStatus_t *status)
{
    dmr_tx_setup(&tx, status->dmrSourceId, status->dmrDestId,
                 status->dmrDestType != DMR_DEST_USER,
                 status->dmrColourCode & 0x0Fu);
    dmr_mod_init(&txMod, DMR_TX_LEVEL, DMR_TX_INVERT);

    txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (txPath < 0)
        return false;

    txId = audioStream_start(txPath, txSamples, 2 * DMR_HALF_SAMPLES, 48000,
                             STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    return (txId >= 0);
}

/**
 * Refuse this transmission, and refuse it out loud.
 *
 * Every way out of startTx() that is not a keyed radio comes here.  Two things
 * have to happen, and both were missing: the PTT is blocked until it is
 * released (txBlocked, cleared in update() on the release edge) so the RTX
 * thread does not try to key again on every 30 ms pass -- a failing keying
 * retried at that rate is a thread that never yields, and with the same
 * priority and no time slicing that is the whole radio stopped -- and the
 * reason is written down, because a PTT that does nothing must be explainable
 * from the PC (docs/TX_SAFETY.md).
 */
bool OpMode_DMR::txRefuse(const char *why)
{
    txBlocked = true;

    /* Throttled: retried keyings would otherwise own the serial line */
    static uint32_t lastPrint;

    const uint32_t now = (uint32_t)getTick();
    if ((now - lastPrint) >= 1000) {
        lastPrint = now;
        printk("[DMR TX] no transmito: %s\n", why);
    }

    txg_exit(rtx_getTxGuard(), now, TXG_REASON_START_FAILED);
    return false;
}

bool OpMode_DMR::startTx(const rtxStatus_t *status)
{
    struct txGuard *guard = rtx_getTxGuard();

    txg_enter(guard, (uint32_t)getTick());

    if (!dmrEnc_available()) {
        /* No encoder: the chain would send silent frames */
        return txRefuse("no hay codificador AMBE+2 en esta imagen");
    }

    if (!settings_dmrIdValid(status->dmrSourceId)) {
        /*
         * No own ID: the link control would carry 0 in its source field, a call
         * from nobody that no repeater can answer. The screen says the same
         * thing (ui_main.c, "NO TX: DMR ID 0") and Settings -> Identity is
         * where the number is set.
         */
        return txRefuse("sin ID propio: Ajustes > Identidad, 1..16777215");
    }

    if (!dmrKey_allowed(status->dmrDestType, status->dmrDestId,
                        status->dmrSourceId)) {
        /*
         * A destination that cannot be encoded: no call. Block the PTT until
         * it is released and the settings change, instead of returning here
         * and being asked again on the next 30 ms tick for ever.
         *
         * The guard goes back to idle: a transmission that never opened must
         * not leave a phase behind for the supervisor to time out, or a
         * refused PTT reads as a stuck transmitter 250 ms later.
         */
        return txRefuse("el destino no se puede codificar (DMRID corto)");
    }

    if (!startBaseband(status)) {
        txg_streamsFailed(guard, (uint32_t)getTick());
        stopTx();
        return txRefuse("la salida al transmisor no se abrio");
    }

    micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (micPath < 0) {
        stopTx();
        return txRefuse("la ruta del micro no se abrio");
    }
    micId = audioStream_start(micPath, micPcm, 2 * VOCODER_PCM_SAMPLES, 8000,
                              STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (micId < 0) {
        stopTx();
        return txRefuse("el flujo del micro no arranco");
    }

    dmrEnc_reset();
    frameHead = 0;
    frameCount = 0;
    txSaidStream = false;
    txRunning = true;
    txFirstBlock = false;
    txIsData = false;
    txEnding = false;
    txStage = 0;
    txVoiceBurst = 0;
    dmrTxCall_begin(&txCall, DMR_TX_HEADERS);

    /*
     * Ours: a channel with different TX and RX frequencies is a repeater,
     * which is the only case where the burst has to land inside a slot that
     * is not ours, and so the only one where the air delay matters.
     */
    txAligned = (status->txFrequency != status->rxFrequency);
    txSilence = txPreRoll();

    txg_streamsOpen(guard, (uint32_t)getTick());
    radio_enableTx();
    if (radio_getStatus() != TX) {
        /* The driver refused to key (the gate, a frequency or a power it
         * cannot use): it says why, and the mode says which step it was */
        const char *why = radio_txRefusedReason();

        stopTx();
        return txRefuse((why != NULL) ? why : "el driver no claveo el PA");
    }
    txg_note(guard, TXG_EV_KEYED, (uint32_t)getTick());
    modeStats_txKey(MS_MODE_DMR, (uint32_t)getTick());
    return true;
}

void OpMode_DMR::stopTx()
{
    const bool wasRunning = txRunning;

    txRunning = false;
    txFirstBlock = false;
    txIsData = false;

    if (txId >= 0)
        audioStream_stop(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
    if (txPath >= 0)
        audioPath_release(txPath);
    if (micPath >= 0)
        audioPath_release(micPath);

    txId = micId = -1;
    txPath = micPath = -1;

    if (wasRunning) {
        radio_disableRtx();
        modeStats_txEnd(MS_MODE_DMR);
    }

    txg_exit(rtx_getTxGuard(), (uint32_t)getTick(), TXG_REASON_MODE);
}

/**
 * Encode what the microphone has, at most `slots` frames.
 */
bool OpMode_DMR::fillMic(unsigned slots)
{
    for (unsigned i = 0; (i < slots) && (frameCount < DMR_TX_QUEUE_FRAMES); i++) {
        dataBlock_t block = inputStream_getData(micId);

        if ((block.data == NULL) || (block.len < VOCODER_PCM_SAMPLES))
            break;

        uint8_t *dst = frameQueue[(frameHead + frameCount) % DMR_TX_QUEUE_FRAMES];

        if (!dmrEnc_encode((const int16_t *)block.data, dst))
            break;
        frameCount++;
    }
    return frameCount > 0;
}

/**
 * Write the rendered half into the stream and wait for it to be played.
 *
 * The wait is the driver's, and it has a deadline (csk6_codec's ioDeadline):
 * what it does not have is a return value anyone looked at.  A sync that fails
 * -- the DSP stalled, the stream was cancelled by the supervisor, the audio
 * table was busy -- used to be ignored and the mode carried on transmitting
 * into a stream that was not moving, one slot after another, for as long as the
 * PTT was held.  Now it ends the over and says why.
 */
bool OpMode_DMR::putHalf()
{
    struct txGuard *guard = rtx_getTxGuard();
    stream_sample_t *idle = outputStream_getIdleBuffer(txId);

    if (idle == NULL)
        return false;

    memcpy(idle, txSamples, DMR_HALF_SAMPLES * sizeof(stream_sample_t));
    if (!outputStream_sync(txId, true))
        return false;

    if (!txFirstBlock) {
        txFirstBlock = true;
        txg_firstBlock(guard, (uint32_t)getTick());
    } else {
        txg_block(guard, (uint32_t)getTick());
    }
    modeStats_txBlock(MS_MODE_DMR, (uint32_t)getTick());
    return true;
}

/**
 * Render one 30 ms half: silence, or our burst in its slot, and the
 * alignment silence at the start of a repeater call.
 */
static void renderHalf(dmr_mod_t *mod, int16_t *out, const uint8_t *dibits,
                       bool carry, size_t silence)
{
    /* Whole symbols: 144 per half */
    for (unsigned s = 0; s < DMR_HALF_SAMPLES / DMR_TX_SPS; s++) {
        const uint8_t *d = NULL;

        if (silence > 0) {
            silence -= DMR_TX_SPS;
        } else if (carry && (s >= SLOT_GUARD_SYMS)
                   && (s < SLOT_GUARD_SYMS + DMR_TX_BURST_DIBITS)) {
            d = &dibits[s - SLOT_GUARD_SYMS];
        }
        dmr_mod_dibits(mod, d, 1, out + s * DMR_TX_SPS);
    }
}

/**
 * One 30 ms slot of a voice call.
 *
 * @return true when the call has finished (terminator and tail on the air).
 */
bool OpMode_DMR::txSlot(const rtxStatus_t *status)
{
    const bool ptt = platform_getPttStatus();
    bool carry = false;

    fillMic(VOCODER_FRAMES_PER_SLOT);

    if (!ptt || txEnding)
        txEnding = true;

    /*
     * What this slot carries is the planner's decision (rtx/DmrTxPlan.h): the
     * headers, the voice, the terminator when the PTT goes up, and the tail.
     * It is a pure function of the counters, which is what lets the host test
     * walk the real sequence and decode what it produced.
     */
    unsigned step = DMRTX_NOTHING;

    if (txSilence >= DMR_HALF_SAMPLES) {
        /* Still in the alignment silence: a whole half of it */
        txSilence -= DMR_HALF_SAMPLES;
    } else {
        step = dmrTxNextSlot(&txCall, ptt, txEnding, status->dmrTimeslot,
                             TX_TAIL_SLOTS);
    }

    switch (step) {
        case DMRTX_HEADER:
            dmr_tx_header(&tx, txBurst);
            carry = true;
            break;

        case DMRTX_VOICE: {
            uint8_t ambe[3][9];

            memset(ambe, 0, sizeof(ambe));
            for (unsigned i = 0; i < 3; i++) {
                if (frameCount == 0)
                    break;
                memcpy(ambe[i], frameQueue[frameHead], 9);
                frameHead = (frameHead + 1) % DMR_TX_QUEUE_FRAMES;
                frameCount--;
            }
            /* Ours: a slot that went out with fewer than its three frames */
            if (frameCount < 3)
                modeStats_txStarve(MS_MODE_DMR);
            dmr_tx_voice(&tx, (const uint8_t (*)[9])ambe, txBurst);
            carry = true;
            break;
        }

        case DMRTX_TERMINATOR:
            dmr_tx_terminator(&tx, txBurst);
            carry = true;
            break;

        case DMRTX_DONE:
            /* The terminator and its tail are on the air: leave */
            renderHalf(&txMod, txSamples, txBurst, false, txSilence);
            (void)putHalf();
            return true;

        default:
            break;
    }

    renderHalf(&txMod, txSamples, txBurst, carry, txSilence);
    if (txSilence > 0)
        txSilence = (txSilence > DMR_HALF_SAMPLES) ? txSilence - DMR_HALF_SAMPLES
                                                   : 0;

    if (!putHalf()) {
        /*
         * The sink is not consuming: this is not a slot to retry, it is the end
         * of the over.  The PTT stays blocked until it is released, so the
         * thread does not come straight back into a stream that is not moving.
         */
        txBlocked = true;
        if (!txSaidStream) {
            txSaidStream = true;
            printk("[DMR TX] se perdio la salida de audio: vuelvo a RX\n");
        }
        return true;
    }

    return false;
}

bool OpMode_DMR::needDataTx(const rtxStatus_t *status) const
{
    /*
     * A queued message is the only transmission in this radio that starts with
     * nobody pressing anything, so it is the one that most needs the gate: it
     * waits for the mode change to end and settle, and it never runs with the
     * PTT down during it.
     */
    if (!rtx_unattendedTxAllowed(status))
        return false;
    if (txBlocked)
        return false;
    if ((status->opStatus != RX) && (status->opStatus != OFF))
        return false;

#ifdef CONFIG_DMR_SMS
    return dmrSms_pending() || (messaging_pending(MSG_TRANSPORT_DMR) > 0);
#else
    return false;
#endif
}

bool OpMode_DMR::startDataTx(const rtxStatus_t *status)
{
    /*
     * The message call is a transmission like any other and the guard has to
     * see it: without a phase here the supervisor would have nothing to time
     * out, and a stream that never accepts a block would leave the PA keyed
     * until the battery died (the failure this path had).
     */
    struct txGuard *guard = rtx_getTxGuard();
    txg_enter(guard, (uint32_t)getTick());

#ifdef CONFIG_DMR_SMS
    /* Ours: the queued message goes into the package's list first */
    smsStartTx(status);
#endif

    if (!startBaseband(status)) {
        txg_streamsFailed(guard, (uint32_t)getTick());
        stopTx();
        return txRefuse("la salida al transmisor no se abrio (datos)");
    }

    micPath = -1;
    micId = -1;
    txRunning = true;
    txFirstBlock = false;
    txIsData = true;
    txEnding = false;
    txStage = 0;
    txSilence = 0;
    txAligned = false;
    txSaidStream = false;

    txg_streamsOpen(guard, (uint32_t)getTick());
    radio_enableTx();
    if (radio_getStatus() != TX) {
        const char *why = radio_txRefusedReason();

        stopTx();
        return txRefuse((why != NULL) ? why : "el driver no claveo el PA");
    }
    txg_note(guard, TXG_EV_KEYED, (uint32_t)getTick());
    modeStats_txKey(MS_MODE_DMR, (uint32_t)getTick());
    return true;
}

/**
 * One 30 ms slot of a short message: one burst per TDMA frame in our slot.
 *
 * The package's list (dmrSms_txNext) ends with its last data block; a data
 * call has no LC terminator (the package's own mode ends it the same way, on
 * silence), so one tail slot closes it.
 */
bool OpMode_DMR::dataTxSlot(const rtxStatus_t *status)
{
    bool carry = false;

    /*
     * One slot of every 60 ms TDMA frame is ours, whichever timeslot the call
     * was set up on: txVoiceBurst counts the 30 ms slots here, so the parity
     * picks slot 1 or slot 2 (the same rule the voice planner applies).
     */
    const bool ourSlot = ((txVoiceBurst & 1u)
                          == (uint8_t)((status->dmrTimeslot == 2) ? 1u : 0u));

    if (txStage == 0) {
        if (ourSlot) {
            uint8_t dt = 0;
            uint8_t b[12];

            memset(b, 0, sizeof(b));
            if (dmrSms_txNext(&dt, b)) {
                dmr_tx_block(status->dmrColourCode & 0x0Fu, dt, b, txBurst);
                carry = true;
            } else {
                dmrSms_txDone(true);
                smsDataSent = true; /* the list reached the air */
                txStage = 1; /* the tail */
            }
        }
        txVoiceBurst++;
    } else if (++txVoiceBurst >= 4) {
        return true;
    }

    renderHalf(&txMod, txSamples, txBurst, carry, 0);
    if (!putHalf()) {
        txBlocked = true;
        if (!txSaidStream) {
            txSaidStream = true;
            printk("[DMR TX] se perdio la salida de audio (datos)\n");
        }
        return true;
    }
    return false;
}
#endif /* CONFIG_DMR_TX */

void OpMode_DMR::abortTx()
{
    txBlocked = true;

#ifdef CONFIG_DMR_TX
    if (txId >= 0)
        audioStream_terminate(txId);
    if (micId >= 0)
        audioStream_terminate(micId);
#endif
}

#ifdef CONFIG_DMR_TRUNK
/*
 * Ours: the engine says where to listen; the radio retunes there.  The
 * platform takes the frequency from the rtx status, so the mode writes it and
 * reopens the receiver.
 */
void OpMode_DMR::trunkPoll(rtxStatus_t *const status)
{
    uint32_t hz = 0;
    uint8_t slot = 0;

    if (!dmrTrunk_poll(&hz, &slot) || (hz == 0) || (hz == trunkHz))
        return;

    trunkHz = hz;
    status->rxFrequency = hz;
    dmr_demod_reset();
    radio_disableRtx();
    radio_enableRx();
}
#endif

void OpMode_DMR::update(rtxStatus_t *const rtx, const bool newCfg)
{
    if (platform_getPttStatus() == false)
        txBlocked = false;

    switch (rtx->opStatus) {
        case OFF:
#ifdef CONFIG_DMR_TX
            stopTx();
#endif
            radio_disableRtx();
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

#ifdef CONFIG_DMR_TX
        case TX: {
            bool finished;
            struct txGuard *guard = rtx_getTxGuard();

            if (platform_getPttStatus() == false)
                txg_finishing(guard, (uint32_t)getTick());

            if (txIsData)
                finished = dataTxSlot(rtx);
            else
                finished = txSlot(rtx);

            if (finished) {
                const bool wasData = txIsData;
                stopTx();
#ifdef CONFIG_DMR_SMS
                /*
                 * Ours: the entry the mode handed to the package.  DMR short
                 * messages are unconfirmed unless the receiver answers with a
                 * delivery response, which arrives later (smsPoll), so a call
                 * whose bursts went out is reported as sent, not as failed.
                 */
                if (wasData && (smsTxId >= 0)) {
                    messaging_done(smsTxId, smsDataSent, 0,
                                   (uint32_t)getTick());
                    smsTxId = -1;
                }
#endif
                rtx->opStatus = OFF;
                startRx = true;
            }
            return;
        }
#endif

        case RX:
#ifdef CONFIG_DMR_SMS
            selfId = rtx->dmrSourceId;
#endif
            break;

        default:
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

#ifdef CONFIG_DMR_TX
    /*
     * The gate, not the level of the PTT line: a change of mode cannot key the
     * transmitter, the press has to be the operator's and to have lasted long
     * enough. This is the path that used to go on the air on a mode change.
     */
    if (rtx_keyingAllowed(rtx) && !txBlocked) {
        stopSampling();
        radio_disableRtx();
        if (startTx(rtx)) {
            rtx->opStatus = TX;
            return;
        }
        startRx = true;
    }

    if (needDataTx(rtx)) {
        stopSampling();
        radio_disableRtx();
        if (startDataTx(rtx)) {
            rtx->opStatus = TX;
            return;
        }
        startRx = true;
    }
#endif

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
    }

    if (newCfg) {
        reset();
#ifdef CONFIG_DMR_TRUNK
        trunkHz = rtx->rxFrequency;
#endif
    }

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0) {
            dmr_demod_note_overrun();
        } else {
            sleepFor(0, 10);
        }
        return;
    }
    dataErrors = 0;

    /* Drop what follows a start or a retune: the receiver settles */
    if (settle > 0) {
        settle -= (block.len < settle) ? block.len : settle;
        return;
    }

    feed(block);

#ifdef CONFIG_DMR_TRUNK
    trunkPoll(rtx);
#endif

    if (dmrSpeech_playing())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
