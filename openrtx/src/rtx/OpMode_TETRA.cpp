/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/printk.h>
#include <hwconfig.h>
#include <tetra/tetra.h>
#include "interfaces/audio.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/TETRA/TetraDemod.h"
#include "core/psram_state.h"
#include "protocols/TETRA/TetraSpeech.h"
#include "protocols/TETRA/TetraStatus.h"
#include "protocols/TETRA/TetraUplink.h"
#include "core/mode_stats.h"
#include "rtx/OpMode_TETRA.hpp"
#ifdef CONFIG_TETRA_AFC
#include "core/state.h"
#endif

/* 30 ms per half buffer: room for libtetra's per-slot decoding */
#define HALF_SAMPLES TETRA_HALF_SAMPLES

/* Samples dropped after starting or retuning: 0.5 s */
#define SETTLE_SAMPLES (TETRA_DEMOD_IN_RATE / 2)

/*
 * While the operator listens to a cell's uplink, the receiver is not on the
 * downlink and there is nothing to stay locked to: what this end can see is
 * that the radio is no longer where the session put it (a channel, a new
 * frequency, a mode change). The UI already ends the session when it sees that;
 * this is the same rule applied from the mode, with the time it takes the
 * reconfigure the row asked for.
 */
#define UL_OFF_TOLERANCE_MS 2000

/*
 * The baseband block, the demodulator and the MAC state are in the mode's own
 * object (struct tetraModeState, OpMode_TETRA.hpp), so they live in the block
 * the modes share and die with the mode instead of sitting in .bss for the
 * whole power on (docs/MEMORY.md). `st` is bound to that object here, which is
 * what keeps the static helpers below unchanged.
 */
static struct tetraModeState *st = nullptr;

/* Read and written once per symbol: PSRAM (core/psram_state.h) */
static int8_t soft[HALF_SAMPLES * 3 / 4 + 8] PSRAM_STATE;

/* Diagnostics and remote control (see TetraStatus.h) */
static uint32_t crcBase; /* CRC passes of earlier tmo contexts */
static uint32_t lastCrc; /* CRC passes the demodulator was told of */
#ifdef CONFIG_C62_MODE_STATS
/* Per-mode statistics: the last values the monotonic counters reported */
static uint32_t statFrames;
static uint32_t statBad;
static uint32_t statCrc;
#endif
#ifdef CONFIG_TETRA_AFC
/*
 * Automatic frequency correction: every AFC_PERIOD_MS, the mean carrier
 * offset measured by the demodulator while blocks decode moves the
 * receiver's fine tuning towards a zero reading. That reading is about
 * 0.15 of the real offset (the receive path removes most of the carrier's
 * DC), but it is monotonic and zero where the phase error is lowest; the
 * loop gain allows for it. The offset found is kept across retunes: it is
 * mostly the radio's own (crystal, filters).
 */
#define AFC_PERIOD_MS 2000
#define AFC_GAIN 3       /* Hz of correction per Hz of mean reading */
#define AFC_STEP_MAX 300 /* Hz per period */
#define AFC_DEADBAND 25  /* Hz of mean reading */
#define AFC_RANGE 4000   /* Hz, either way: the C62 decodes about 4 kHz off */
#define AFC_CRC_RECENT_MS 1000
/*
 * Readings are trusted below this phase error (% of pi/4): near the edges
 * of the band where the C62 decodes (above 30%), the offset reading can
 * have the wrong sign
 */
#define AFC_ERROR_MAX 35
/*
 * Nothing decoded this long: the correction may be wrong for this signal
 * (another cell, a jump). Go back to the stored one, or from the stored one
 * to none, and start again.
 */
#define AFC_LOST_MS 8000
/*
 * Only with a signal this strong (dBm): a wrong correction leaves most of a
 * strong signal in the receive filter, but a weak signal that does not
 * decode is just weak, and moving the tuning only makes it worse
 */
#define AFC_LOST_RSSI -90
/*
 * The correction found is stored in the settings, so that a radio turned on
 * with a signal too weak for the loop to steer (above AFC_ERROR_MAX) starts
 * corrected; only moves of this much are stored, to spare the flash.
 */
#define AFC_STORE_STEP 200
static int32_t afcOffset;
static float afcSum;
static uint32_t afcCount;
static long long afcStart;
static long long lastCrcTick;
static int32_t afcApplied;
static bool afcLoaded; /* Stored correction read, once per power on */

static int32_t afcStored(void)
{
    return state.settings.tetraAfcOffset;
}

/**
 * Store the correction in the settings when it has moved far enough; they
 * are saved to flash a few seconds later (CONFIG_NVM_SAVE_ON_CHANGE).
 */
static void afcStore(void)
{
    int32_t delta = afcOffset - afcStored();
    if (delta < AFC_STORE_STEP && delta > -AFC_STORE_STEP)
        return;
    pthread_mutex_lock(&state_mutex);
    state.settings.tetraAfcOffset = (int16_t)afcOffset;
    pthread_mutex_unlock(&state_mutex);
}

static bool afcEnabled(void)
{
    return state.settings.tetraAfcOff == 0;
}

/**
 * Apply an offset to the receiver, if it is not applied already.
 */
static void afcApply(int32_t offset)
{
    if (offset != afcApplied) {
        radio_setRxOffset(offset);
        afcApplied = offset;
    }
}

/**
 * Called after each block of samples.
 */
static void afcUpdate(void)
{
    if (!afcEnabled()) {
        afcApply(0);
        afcSum = 0;
        afcCount = 0;
        return;
    }
    afcApply(afcOffset); /* Turned on again */

    /* Measure only while blocks decode: never steer on noise */
    long long now = getTick();
    if (tetra_tmo_locked(&st->tmo) && (now - lastCrcTick) < AFC_CRC_RECENT_MS
        && tetraDemod_phaseError(&st->demod) < AFC_ERROR_MAX) {
        afcSum += tetraDemod_carrierOffset(&st->demod);
        afcCount++;
    }

    /*
     * Lost for long, maybe because the correction is wrong for this signal
     * (another cell, a jump): from no correction it can be found again
     */
    if ((now - lastCrcTick) > AFC_LOST_MS && rtx_getRssi() > AFC_LOST_RSSI) {
        int32_t stored = afcStored();
        int32_t next = (afcOffset != stored) ? stored : 0;
        if (next == afcOffset)
            return;
        afcOffset = next;
        afcApply(afcOffset);
        lastCrcTick = now; /* Give it the time to decode */
        afcSum = 0;
        afcCount = 0;
        afcStart = now;
        return;
    }
    if ((now - afcStart) < AFC_PERIOD_MS)
        return;
    afcStart = now;

    uint32_t count = afcCount;
    float mean = (count > 0) ? afcSum / count : 0;
    afcSum = 0;
    afcCount = 0;
    if (count < 10 || fabsf(mean) <= AFC_DEADBAND)
        return;

    float step = AFC_GAIN * mean;
    if (step > AFC_STEP_MAX)
        step = AFC_STEP_MAX;
    else if (step < -AFC_STEP_MAX)
        step = -AFC_STEP_MAX;
    int32_t next = afcOffset + (int32_t)lrintf(step);
    if (next > AFC_RANGE)
        next = AFC_RANGE;
    else if (next < -AFC_RANGE)
        next = -AFC_RANGE;
    afcOffset = (next / 10) * 10; /* The BK4819 tunes in 10 Hz steps */
    afcApply(afcOffset);
    afcStore();
}
#endif
static volatile bool resetRequest;
static volatile bool eqSeed = true; /* Start from CONFIG_TETRA_EQ_INIT */

OpMode_TETRA::OpMode_TETRA()
    : startRx(false)
    , dataErrors(0)
    , settle(SETTLE_SAMPLES)
    , demodJumps(0)
    , basebandPath(-1)
    , basebandId(-1)
{
    /* The mode's state, in the object: see docs/MEMORY.md */
    st = &state;
}

OpMode_TETRA::~OpMode_TETRA()
{
    disable();
    /* The object is about to be recycled: no helper may touch it now */
    st = nullptr;
}

void OpMode_TETRA::reset()
{
    settle = SETTLE_SAMPLES;
    tetraDemod_init(&st->demod);
#ifdef CONFIG_TETRA_EQ_INIT
    static const float eqInit[TETRA_DEMOD_EQ_TAPS] = CONFIG_TETRA_EQ_INIT;
    if (eqSeed)
        tetraDemod_setEqualizer(&st->demod, eqInit);
#endif

    demodJumps = 0;
    resync(false);
    crcBase = 0;
    lastCrc = 0;
    tetraStatus_reset();
}

/*
 * Restart libtetra's burst synchronisation. After a jump in the symbol
 * stream (samples lost, the demodulator re-acquiring) it would otherwise
 * keep decoding on the old slot grid for seconds before noticing.
 */
void OpMode_TETRA::resync(bool overrun)
{
    crcBase += tetra_tmo_crc_ok(&st->tmo);

    struct tetra_rx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = tetraStatus_onEvent;
    cfg.promiscuous = true;
    tetra_tmo_init(&st->tmo, &cfg);
#if TETRA_UPLINK_DECODE
    /*
     * Listening to the uplink of a cell: the MS to BS bursts are scrambled
     * with the colour code of the cell, and this context has just forgotten
     * what the downlink taught it. The session (TetraUplink.h) still knows
     * which cell the operator asked for; seed the decoder with it so bursts
     * decode without a downlink to learn from (libtetra, clause 9.4.4).
     */
    uint16_t mcc, mnc;
    uint8_t cc;
    if (tetraUplink_cell(&mcc, &mnc, &cc))
        tetra_tmo_set_uplink_cell(&st->tmo, mcc, mnc, cc);
#endif
    tetraStatus_lostSync(overrun);
    modeStats_resync(MS_MODE_TETRA);
}

void OpMode_TETRA::enable()
{
#ifdef CONFIG_TETRA_AFC
    /* The stored correction, once per power on */
    if (!afcLoaded) {
        int32_t stored = afcStored();
        if (stored > AFC_RANGE || stored < -AFC_RANGE)
            stored = 0;
        afcOffset = (stored / 10) * 10;
        afcLoaded = true;
    }
    /* Before receiving starts: no retune once it has settled */
    afcApply(afcEnabled() ? afcOffset : 0);
    afcSum = 0;
    afcCount = 0;
    afcStart = getTick();
    lastCrcTick = afcStart; /* Not "lost" before trying */
#endif
    reset();
    tetraSpeech_start();
    startRx = true;
}

void OpMode_TETRA::disable()
{
    startRx = false;
    stopSampling();
    tetraSpeech_stop();
#ifdef CONFIG_TETRA_AFC
    afcApply(0); /* The other modes tune where they are told */
#endif
    radio_disableRtx();
    platform_ledOff(GREEN);
    tetraStatus_reset();
}

void OpMode_TETRA::startSampling()
{
    basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, st->baseband, 2 * HALF_SAMPLES,
                                   TETRA_DEMOD_IN_RATE,
                                   STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_TETRA::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

/*
 * The "Cell uplink" session, seen from the receiving thread: libtetra was
 * seeded with the cell when the context was created (resync), and what is left
 * is ending the session when the receiver is no longer where it put it, or
 * when what arrives is a downlink of a different cell. The UI ends it too, from
 * the frequency it sees; both agree, and whichever runs first wins.
 */
static bool ulAway; /* The receiver is not on the session's uplink */
static long long ulAwayTick;

static void ulSessionCheck(const rtxStatus_t *rtx)
{
    uint32_t ulHz = tetraUplink_freq();

    if ((ulHz == 0) || !tetraUplink_active()) {
        ulAway = false;
        return;
    }

    if (rtx->rxFrequency != ulHz) {
        /* The reconfigure the row asked for takes a frame or two */
        long long now = getTick();

        if (!ulAway) {
            ulAway = true;
            ulAwayTick = now;
        } else if ((now - ulAwayTick) > UL_OFF_TOLERANCE_MS) {
            printk("[TETRA UL] the receiver moved away: uplink session off\n");
            tetraUplink_stop();
            ulAway = false;
        }
        return;
    }
    ulAway = false;

    /* A downlink where the cell's uplink was expected belongs to someone else */
    struct tetraStatus ts;
    tetra_getStatus(&ts);
    if (ts.sync && ts.cell) {
        uint16_t mcc, mnc;
        uint8_t cc;

        if (tetraUplink_cell(&mcc, &mnc, &cc) &&
            ((ts.mcc != mcc) || (ts.mnc != mnc) || (ts.colourCode != cc))) {
            printk("[TETRA UL] another cell on the uplink frequency\n");
            tetraUplink_cellLost();
        }
    }
}

void OpMode_TETRA::update(rtxStatus_t *const rtx, const bool newCfg)
{
    switch (rtx->opStatus) {
        case OFF:
            radio_disableRtx();
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

        case RX:
            break;

        default:
            /* Receive only */
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
    }

    /* A new frequency is a new cell: start from scratch */
    if (newCfg || resetRequest) {
        resetRequest = false;
        reset();
    }

    /* The operator's "Cell uplink" session: the UI tunes, this keeps it honest */
    ulSessionCheck(rtx);

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer of samples. A failed read means we were
     * late and samples were lost: the symbol stream jumped, so resync, and
     * do not lose more by sleeping (unless the stream keeps failing).
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        modeStats_lost(MS_MODE_TETRA, 1);
        if (dataErrors++ == 0)
            resync(true);
        else
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;

    /*
     * Drop what follows a start or a retune: the receiver and the audio
     * path settle, and the demodulator would take their transient for the
     * carrier offset and keep it.
     */
    if (settle > 0) {
        settle -= (block.len < settle) ? block.len : settle;
        return;
    }

    modeStats_blockStart(MS_MODE_TETRA);
    size_t n = tetraDemod_process(&st->demod, block.data, block.len, soft,
                                sizeof(soft));

    /* The demodulator followed a timing jump or re-acquired: resync too */
    uint32_t jumps = tetraDemod_reacquisitions(&st->demod);
    if (jumps != demodJumps) {
        demodJumps = jumps;
        resync(false);
    }

    tetra_tmo_push_soft(&st->tmo, soft, n);
    modeStats_block(MS_MODE_TETRA, block.data, block.len);

    /* Blocks decode: the demodulator keeps its lock */
    uint32_t crc = crcBase + tetra_tmo_crc_ok(&st->tmo);
    if (crc != lastCrc) {
        lastCrc = crc;
        tetraDemod_signalOk(&st->demod);
#ifdef CONFIG_TETRA_AFC
        lastCrcTick = getTick();
#endif
    }

    /*
     * Per-mode statistics: what this block carried. The speech counters are
     * monotonic for the whole power on, so the deltas survive a mode change.
     * Under the option, this is the only place that recomputes them.
     */
#ifdef CONFIG_C62_MODE_STATS
    uint32_t sframes, sbad, sother;
    tetraSpeech_stats(&sframes, &sbad, &sother);
    modeStats_frames(MS_MODE_TETRA, sframes - statFrames);
    modeStats_badCrc(MS_MODE_TETRA, sbad - statBad);
    modeStats_valid(MS_MODE_TETRA, crc - statCrc);
    modeStats_error(MS_MODE_TETRA, 10u * tetraDemod_phaseError(&st->demod));
    /* Timing loop: drift in samples at 144 kHz, one phase is 1/8 symbol */
    modeStats_timing(MS_MODE_TETRA, (int32_t)(st->demod.drift * 125.0f), 125);
    modeStats_rssi(MS_MODE_TETRA, (int)rtx_getRssi());
    statFrames = sframes;
    statBad = sbad;
    statCrc = crc;
#endif
#ifdef CONFIG_TETRA_AFC
    afcUpdate();
#endif

    tetraStatus_setDemod(tetraDemod_phaseError(&st->demod),
                         tetraDemod_carrierOffset(&st->demod));

    if (tetraStatus_sync())
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}

void tetra_getDiag(struct tetraDiag *out)
{
    out->crcOk = crcBase + tetra_tmo_crc_ok(&st->tmo);
    out->realigned = tetra_tmo_realigned(&st->tmo, NULL);
    out->locked = tetra_tmo_locked(&st->tmo);
    out->reacquired = st->demod.reacquired;
    out->slips = st->demod.slips;
    out->symbols = st->demod.symbols;
    out->phaseErr = tetraDemod_phaseError(&st->demod);
    out->carrierHz = tetraDemod_carrierOffset(&st->demod);
    out->clockPpm = tetraDemod_clockOffset(&st->demod);
    out->eqSeed = eqSeed;
    memcpy(out->eq, st->demod.eq, sizeof(out->eq));
}

void tetra_requestReset(bool seedEqualizer)
{
    eqSeed = seedEqualizer;
    resetRequest = true;
}
