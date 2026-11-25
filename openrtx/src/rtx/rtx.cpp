/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/radio.h"
#include "interfaces/platform.h"
#include "core/spectrum_service.h"
#include "core/utils.h"
#include "hwconfig.h"
#include <errno.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include "rtx/rtx.h"
#include "rtx/ModeArena.hpp"
#include "rtx/OpMode_FM.hpp"
#include "rtx/OpMode_Airband.hpp"
#include "rtx/OpMode_M17.hpp"
#include "rtx/OpMode_WFM.hpp"
#if defined(PLATFORM_C62)
#include "drivers/audio/csk6_codec.h"
#endif
#ifdef CONFIG_DMR
#include "rtx/OpMode_DMR.hpp"
#endif
#ifdef CONFIG_YSF
#include "rtx/OpMode_YSF.hpp"
#endif
#ifdef CONFIG_TETRA
#include "rtx/OpMode_TETRA.hpp"
#endif
#ifdef CONFIG_NXDN
#include "rtx/OpMode_NXDN.hpp"
#endif
#ifdef CONFIG_DPMR
#include "rtx/OpMode_DPMR.hpp"
#endif
#ifdef CONFIG_P25
#include "rtx/OpMode_P25.hpp"
#endif
#ifdef CONFIG_DSTAR
#include "rtx/OpMode_DSTAR.hpp"
#endif

static pthread_mutex_t   *cfgMutex;     // Mutex for incoming config messages
static const rtxStatus_t *newCnf;       // Pointer for incoming config messages
static rtxStatus_t        rtxStatus;    // RTX driver status
static rssi_t             rssi;         // Current RSSI in dBm
static bool               reinitFilter; // Flag for RSSI filter re-initialisation

/*
 * Transmit supervision.
 *
 * The guard records the phases of a transmission and their deadlines; the
 * supervisor (rtx_supervise(), from the main thread) reads it and, when a
 * deadline is missed, unkeys and cancels the transmit streams from outside the
 * RTX thread. That thread may be blocked in a stream sync or in a DSP call --
 * which is exactly the failure this protects against -- so the unkey cannot be
 * its job. It then finds its streams cancelled, unwinds, and finishes the
 * transmission through the mode's normal exit path.
 *
 * txMutex protects the guard and the abort request: the RTX thread writes
 * phases, the supervisor reads them and sets the request. It is never held
 * across a call into audio or radio, so a blocked RTX thread never holds it.
 */
static pthread_mutex_t txMutex = PTHREAD_MUTEX_INITIALIZER;
static struct txGuard  txGuard;
static bool            txAbortPending;
static uint32_t        txAbortTime;

/*
 * Absolute transmit clock (rtx/TxWatch.h).
 *
 * The guard above knows the phases of a transmission because the modes report
 * them, which means a mode that keys without reporting -- or a keying nobody
 * has a phase for -- is invisible to it. This clock knows one thing only: the
 * PA is on the air or it is not, sampled from the driver every 5 ms. It counts
 * the air, not the transmissions: an unattended chain of messages, a mode that
 * re-keys straight after its over, and a change of mode all continue the same
 * count. Only a release -- TXW_RELEASE_MS with the PA off -- puts it back to
 * zero. At TXW_LIMIT_MS the supervisor ends the transmission whatever the mode
 * believes, and keying stays refused until that release happens.
 *
 * It lives in this file, is sampled by rtx_supervise() (the main thread) and
 * depends on nothing that can be the thing that is stuck.
 */
static struct txWatch  txWatch;
static const char     *txWatchSource = "idle";
static bool            txWatchTimedOut;
static bool            txWatchCutActive;

/*
 * Why the last keying attempt was refused, as rtx_keyingAllowed() decided it.
 * "none" when the gate would let a press through.  It is what the status of
 * the PC protocol reports, so a PTT that does nothing is never silent
 * (docs/TX_SAFETY.md).
 */
static const char *keyVeto = "none";

/*
 * Transmit permission.
 *
 * switching is true from the moment a mode change starts until its new mode is
 * enabled: the gate vetoes every keying attempt in between, whatever the PTT
 * line says, and the platform's radio_enableTx() refuses to drive the PA.
 *
 * The gate itself (rtx/KeyGate.h) is the rule the modes ask: a release of the
 * PTT since the change and a press that lasts long enough. Both live under
 * txMutex, which is never held across a call into audio, radio or a mode.
 */
static struct keyGate keyGate;
static bool            switching;

static OpMode  *currMode;               // Pointer to currently active opMode handler
static OpMode     noMode;               // Empty opMode handler for opmode::NONE

/*
 * The modes' objects, in one block (docs/MEMORY.md).
 *
 * A radio runs one mode at a time, and every mode's object is large: sample
 * buffers for the whole 30 ms block, the transmit queues, the call state. Only
 * the active mode's object is constructed, in place, and it is destroyed when
 * the mode is left, so the block costs the size of the largest mode instead of
 * the sum of all of them.
 *
 * The registry is the single list of the modes this build has: size and
 * alignment come from the class, so a member added to a mode grows the block
 * by itself. A mode compiled out is not in it and does not count.
 */
namespace
{

constexpr modeArenaEntry modeEntries[] = {
    { OPMODE_FM, "FM", sizeof(OpMode_FM), alignof(OpMode_FM) },
    { OPMODE_AIRBAND, "AM airband", sizeof(OpMode_Airband),
      alignof(OpMode_Airband) },
    { OPMODE_WFM_BC, "WFM broadcast", sizeof(OpMode_WFM),
      alignof(OpMode_WFM) },
#ifdef CONFIG_M17
    { OPMODE_M17, "M17", sizeof(OpMode_M17), alignof(OpMode_M17) },
#endif
#ifdef CONFIG_TETRA
    { OPMODE_TETRA, "TETRA", sizeof(OpMode_TETRA), alignof(OpMode_TETRA) },
#endif
#ifdef CONFIG_DMR
    { OPMODE_DMR, "DMR", sizeof(OpMode_DMR), alignof(OpMode_DMR) },
#endif
#ifdef CONFIG_NXDN
    { OPMODE_NXDN, "NXDN", sizeof(OpMode_NXDN), alignof(OpMode_NXDN) },
#endif
#ifdef CONFIG_DPMR
    { OPMODE_DPMR, "dPMR", sizeof(OpMode_DPMR), alignof(OpMode_DPMR) },
#endif
#ifdef CONFIG_P25
    { OPMODE_P25, "P25", sizeof(OpMode_P25), alignof(OpMode_P25) },
#endif
#ifdef CONFIG_DSTAR
    { OPMODE_DSTAR, "D-STAR", sizeof(OpMode_DSTAR),
      alignof(OpMode_DSTAR) },
#endif
#ifdef CONFIG_YSF
    { OPMODE_YSF, "YSF", sizeof(OpMode_YSF), alignof(OpMode_YSF) },
#endif
};

/// Bytes of the largest mode in the registry: what the block has to be.
constexpr size_t largestModeBytes(void)
{
    size_t largest = 0;
    for (const modeArenaEntry &e : modeEntries)
        if (e.size > largest)
            largest = e.size;
    return largest;
}

/// Strictest alignment in the registry.
constexpr size_t strictestModeAlign(void)
{
    size_t strictest = 1;
    for (const modeArenaEntry &e : modeEntries)
        if (e.align > strictest)
            strictest = e.align;
    return strictest;
}

/*
 * SRAM, not PSRAM: what the objects hold is the mode's sample buffers -- the
 * 30 ms block being demodulated, the one being modulated -- and those are
 * touched inside the sample loop (docs/MEMORY.md has the split).
 */
alignas(strictestModeAlign()) uint8_t modeArenaStorage[largestModeBytes()];

ModeArena modeArena(modeArenaStorage, sizeof(modeArenaStorage), modeEntries,
                    sizeof(modeEntries) / sizeof(modeEntries[0]));

} /* namespace */

/**
 * Construct a mode's object in the block the arena just handed out.
 *
 * Placement new, so no allocation happens here: the memory is the arena's and
 * the object only runs its constructor over it.
 *
 * @param mode: OPMODE_* of the mode to construct.
 * @param block: the arena's block.
 * @return the object, or nullptr if this build has no such mode.
 */
static OpMode *constructMode(uint8_t mode, void *block)
{
    switch (mode) {
        case OPMODE_FM:
            return new (block) OpMode_FM();
        case OPMODE_AIRBAND:
            return new (block) OpMode_Airband();
        case OPMODE_WFM_BC:
            return new (block) OpMode_WFM();
#ifdef CONFIG_M17
        case OPMODE_M17:
            return new (block) OpMode_M17();
#endif
#ifdef CONFIG_TETRA
        case OPMODE_TETRA:
            return new (block) OpMode_TETRA();
#endif
#ifdef CONFIG_DMR
        case OPMODE_DMR:
            return new (block) OpMode_DMR();
#endif
#ifdef CONFIG_NXDN
        case OPMODE_NXDN:
            return new (block) OpMode_NXDN();
#endif
#ifdef CONFIG_DPMR
        case OPMODE_DPMR:
            return new (block) OpMode_DPMR();
#endif
#ifdef CONFIG_P25
        case OPMODE_P25:
            return new (block) OpMode_P25();
#endif
#ifdef CONFIG_DSTAR
        case OPMODE_DSTAR:
            return new (block) OpMode_DSTAR();
#endif
#ifdef CONFIG_YSF
        case OPMODE_YSF:
            return new (block) OpMode_YSF();
#endif
        default:
            return nullptr;
    }
}

/**
 * Force the transmitter off and prove it, before a mode change touches anything.
 *
 * A mode change reconfigures a half-duplex chip for a different modulation, so
 * it has to start from a radio that is not transmitting -- and the check has to
 * be on the PA enable lines, not on the driver's own variable, because the line
 * that is still high is exactly the one that matters.
 *
 * @return true when the transmit path is off.
 */
static bool forceTxOff(void)
{
    /* The first attempt is the normal one; two retries cover a busy bus */
    for (unsigned attempt = 0; attempt < 3; attempt++)
    {
        radio_disableRtx();
        if (radio_txPathOff())
            return true;
    }

    return false;
}

/**
 * Leave the radio idle and receiving, with the PA off.
 *
 * The path a mode change takes when it cannot construct the new mode: no
 * half-configured chip, no streams, the transmitter down and the receiver
 * available again for the squelch, the scan and the UI.
 */
static void leaveRadioIdle(void)
{
    forceTxOff();
    radio_enableRx();
}

/**
 * Leave the active mode and enter another one, or none.
 *
 * The order is the safety property, not a detail:
 *
 *  1. keying is vetoed and the PTT the gate had seen is forgotten, so the mode
 *     that is about to be constructed cannot key on a stale level;
 *  2. the PA is forced off and read back;
 *  3. only then is the chip told about the new mode (radio_setOpmode() is the
 *     driver's contract for a real mode change and it is told before the old
 *     mode is shut down), the old mode stops its streams and the object dies;
 *  4. the block goes back, the new object is constructed in it and enabled;
 *  5. the gate stays closed for KEYGATE_GUARD_MS after that and needs a
 *     release of the PTT before anything may key.
 *
 * currMode is noMode while the block is free, so a reader on another thread
 * (rxSquelchOpen(), from the UI and the scan) finds "no carrier" instead of
 * memory that is being reused.
 *
 * A mode that cannot be constructed leaves the radio idle on OPMODE_NONE: no
 * half-configured chip, no streams, PA off, receiver available, and the reason
 * in the log.
 *
 * @param mode: OPMODE_* to enter.
 */
static void switchMode(uint8_t mode)
{
    pthread_mutex_lock(&txMutex);
    switching = true;
    keyGate_beginTransition(&keyGate, (uint32_t)getTick());
    pthread_mutex_unlock(&txMutex);

    /*
     * 1. Transmitter down and verified, before the chip is reconfigured for a
     *    mode that may not even exist.
     */
    if (!forceTxOff())
    {
        #if defined(PLATFORM_C62)
        printf("MODE %u: PA read-back says still keyed before the change\r\n",
               (unsigned)mode);
        #endif
    }

    radio_setOpmode(static_cast< enum opmode >(mode));

    currMode->disable();

    OpMode *dying = currMode;
    currMode = &noMode;
    /* noMode is a plain object with no block behind it: it is never destroyed */
    if (dying != &noMode)
        dying->~OpMode(); /* virtual: the derived destructor runs */
    modeArena_release();

    rtxStatus.opStatus = OFF;

    if (mode == OPMODE_NONE)
    {
        pthread_mutex_lock(&txMutex);
        keyGate_endTransition(&keyGate, (uint32_t)getTick());
        switching = false;
        pthread_mutex_unlock(&txMutex);
        return;
    }

    OpMode *fresh = nullptr;
    int err = modeArena_acquire(mode);
    if (err == 0)
        fresh = constructMode(mode, modeArena.block());

    /*
     * The registry and the switch above are two lists of the same modes; a
     * mode in one and not the other would hand out a block nobody fills.
     */
    if (fresh != nullptr && fresh->getID() != mode) {
        fresh->~OpMode();
        fresh = nullptr;
        err = -EINVAL;
    }

    if (fresh == nullptr) {
        /*
         * Nothing to run: give the block back, leave the driver on NONE and
         * say so. The UI keeps working, the radio is simply idle.
         *
         * opMode goes back to NONE with it: otherwise every update would find
         * "active mode differs from the requested one" and try again, which is
         * a log line every 30 ms until the operator leaves the mode. A new
         * configuration that asks for it again is one attempt, which is what
         * an operator changing something expects.
         */
        modeArena_release();
        radio_setOpmode(OPMODE_NONE);
        leaveRadioIdle();
        rtxStatus.opMode = OPMODE_NONE;

        pthread_mutex_lock(&txMutex);
        keyGate_endTransition(&keyGate, (uint32_t)getTick());
        switching = false;
        pthread_mutex_unlock(&txMutex);

        #if defined(PLATFORM_C62)
        printf("MODE %u: no arena block (%d), staying idle, PA off, rx=%d\r\n",
               (unsigned)mode, err, (int)(radio_getStatus() == RX));
        #else
        (void)err;
        #endif
        return;
    }

    currMode = fresh;
    currMode->enable();

    pthread_mutex_lock(&txMutex);
    keyGate_endTransition(&keyGate, (uint32_t)getTick());
    switching = false;
    pthread_mutex_unlock(&txMutex);
}


void rtx_init(pthread_mutex_t *m)
{
    // Initialise mutex for configuration access
    cfgMutex = m;
    newCnf   = NULL;

    /*
     * Default initialisation for rtx status
     */
    rtxStatus.opMode        = OPMODE_NONE;
    rtxStatus.bandwidth     = BW_25;
    rtxStatus.txDisable     = 0;
    rtxStatus.opStatus      = OFF;
    rtxStatus.rxFrequency   = 430000000;
    rtxStatus.txFrequency   = 430000000;
    rtxStatus.txPower       = 0.0f;
    rtxStatus.sqlLevel      = 1;
    rtxStatus.rxToneEn      = 0;
    rtxStatus.rxTone        = 0;
    rtxStatus.txToneEn      = 0;
    rtxStatus.txTone        = 0;
    rtxStatus.invertRxPhase = false;
    rtxStatus.lsfOk         = false;
    rtxStatus.M17_src[0]    = '\0';
    rtxStatus.M17_dst[0]    = '\0';
    rtxStatus.M17_link[0]   = '\0';
    rtxStatus.M17_refl[0]   = '\0';
    rtxStatus.M17_meta_text[0] = '\0';
    rtxStatus.dmrColourCode = 1;
    rtxStatus.dmrTimeslot   = 1;
    rtxStatus.dmrSourceId   = 0;   /* set from settings.dmr_id on the first sync */
    rtxStatus.dmrDestId     = 9;   /* matches default_settings */
    rtxStatus.dmrDestType   = DMR_DEST_GROUP;
    /*
     * The modes' block: poison it before the first acquire, so that an acquire
     * that finds it written to is a bug and not the boot's own leftovers.
     */
    modeArena_bind(&modeArena);
    modeArena_release();
    currMode = &noMode;

    pthread_mutex_lock(&txMutex);
    txg_init(&txGuard, (uint32_t)getTick());
    keyGate_init(&keyGate, (uint32_t)getTick());
    txw_init(&txWatch, (uint32_t)getTick(), TXW_LIMIT_MS);
    txWatchSource    = "idle";
    txWatchTimedOut  = false;
    txWatchCutActive = false;
    switching = false;
    txAbortPending = false;
    pthread_mutex_unlock(&txMutex);

    /*
     * Initialise low-level platform-specific driver
     */
    radio_init(&rtxStatus);
    radio_updateConfiguration();

    /*
     * Initial value for RSSI filter
     */
    rssi         = radio_getRssi();
    reinitFilter = false;
}

void rtx_terminate()
{
    rtxStatus.opStatus = OFF;
    rtxStatus.opMode   = OPMODE_NONE;
    currMode->disable();
    /*
     * The object dies with the firmware: the block goes back so a terminate
     * and init cycle starts from poison, and the mode's destructor runs while
     * everything it may touch (the audio paths, the codec) is still there.
     */
    OpMode *dying = currMode;
    currMode = &noMode;
    if (dying != &noMode)
        dying->~OpMode();
    modeArena_release();
    radio_terminate();
}

void rtx_configure(const rtxStatus_t *cfg)
{
    /*
     * NOTE: an incoming configuration may overwrite a preceding one not yet
     * read by the radio task. This mechanism ensures that the radio driver
     * always gets the most recent configuration.
     */

    pthread_mutex_lock(cfgMutex);
    newCnf = cfg;
    pthread_mutex_unlock(cfgMutex);
}

rtxStatus_t rtx_getCurrentStatus()
{
    return rtxStatus;
}

struct txGuard *rtx_getTxGuard()
{
    return &txGuard;
}

/**
 * Force the transmitter off from outside the RTX thread.
 *
 * The PA goes first, so nothing is on the air while the streams are torn down,
 * and the streams go second: cancelling them is what wakes the RTX thread if it
 * is blocked waiting for one of their buffer boundaries.
 */
static void abortTransmission(uint32_t now, const char *why)
{
    /*
     * PA off first, and confirmed: the mode's abortTx() cancels the streams,
     * which is what wakes the thread blocked in one of them, and the radio must
     * already be off the air when that thread resumes.
     */
    radio_disableRtx();
    if (!radio_txPathOff())
        radio_disableRtx();

    currMode->abortTx();

    #if defined(PLATFORM_C62)
    /*
     * Throttled: an abort is retried every 20 ms until the mode leaves
     * transmit, and a 115200 baud line cannot take that many lines without
     * becoming the reason the abort does not complete.
     */
    static uint32_t lastPrint = 0;
    if ((uint32_t)(now - lastPrint) < 500)
        return;
    lastPrint = now;

    char summary[96];
    char trace[96];
    txg_format(&txGuard, summary, sizeof(summary));
    txg_formatTrace(&txGuard, trace, sizeof(trace));
    printf("TX ABORT %s: %s | %s\r\n", why, trace, summary);
    #else
    (void)now;
    (void)why;
    #endif
}

/**
 * One line in the log per transmission, with the trace of what it did.
 *
 * Printed on the way out, never on the way in: a serial line at 115200 baud
 * takes about 5 ms for one of these lines, and the keying path has a budget of
 * its own. The live state of an ongoing transmission is on the remote control
 * instead ("tx" command), which is also where a transmission that never ends
 * can be seen.
 */
static void logTxExit(void)
{
    #if defined(PLATFORM_C62)
    /*
     * Keyed on the transmission counter, not on the phase: an over that starts
     * and fails inside a single rtx_task() never shows STARTING here, and the
     * one that matters most is exactly that one.
     */
    static uint32_t lastSeen = 0;
    uint32_t seen;

    pthread_mutex_lock(&txMutex);
    seen = txGuard.transmissions;
    pthread_mutex_unlock(&txMutex);

    if (seen == lastSeen)
        return;
    lastSeen = seen;

    char trace[110];
    char summary[130];
    txg_formatTrace(&txGuard, trace, sizeof(trace));
    txg_format(&txGuard, summary, sizeof(summary));
    /*
     * The exit line carries the transmit clock too: how much air this
     * transmission put out and, after a cut, that it was the radio and not the
     * operator who ended it.
     */
    printf("TX exit air=%lums/%s %s | %s | pk=%u\r\n",
           (unsigned long)txw_airMs(&txWatch, (uint32_t)getTick()),
           rtx_txWatchReason(), trace, summary,
           (unsigned int)csk6codec_getTxPeak());
    /* The next over starts its baseband peak from zero */
    csk6codec_clearTxPeak();
    #endif
}

/**
 * The line the radio leaves behind when it had to stop a transmission itself.
 *
 * It says what was keyed, on whose behalf, for how long and what the modes
 * believed they were doing, because that is what the operator has to report
 * when the radio comes back hot. Printed once per cut.
 *
 * @param now: current time, ms.
 */
static void logTxTimeout(uint32_t now)
{
    char summary[96];
    char trace[96];
    txg_format(&txGuard, summary, sizeof(summary));
    txg_formatTrace(&txGuard, trace, sizeof(trace));

    #if defined(PLATFORM_C62)
    printf("TX TIMEOUT %us: mode=%s src=%s air=%lums ph=%s | %s | %s\r\n",
           (unsigned int)(TXW_LIMIT_MS / 1000),
           modeArena.nameOf(rtx_currentOpMode()),
           txWatchSource, (unsigned long)txw_airMs(&txWatch, now),
           txg_phaseName(txGuard.phase), summary, trace);
    #else
    (void)now;
    #endif
}

void rtx_supervise()
{
    const uint32_t now = (uint32_t)getTick();
    const bool ptt = platform_getPttStatus() ? true : false;
    const bool keyed = (radio_getStatus() == TX);
    bool abort = false;

    pthread_mutex_lock(&txMutex);

    /*
     * The absolute clock is fed from the driver's state, not from a mode: a
     * keying is a keying whoever asked for it, and it starts at the first
     * sample that finds the PA on the air (within one 5 ms tick).
     */
    const bool rising = keyed && (!txWatch.keyed);
    txw_sample(&txWatch, now, keyed);
    if (rising)
    {
        txWatchSource = ptt ? "ptt"
                            : (keyGate_unattendedGranted(&keyGate, now)
                                   ? "unattended"
                                   : "mode");
        txWatchTimedOut = false;
        txWatchCutActive = false;
    }
    else if ((!keyed) && (!txWatch.keyed) && (!txWatch.blocked))
    {
        txWatchSource = "idle";
        txWatchCutActive = false;
    }

    const bool guardAbort = (txg_tick(&txGuard, now, ptt) == TXG_ACTION_ABORT);
    const bool clockCut = (txw_tick(&txWatch, now) == TXW_ACTION_CUT);
    /* A transmission that is over its bar and still keyed: keep forcing it */
    const bool clockStuck = txw_blocked(&txWatch) && keyed;

    if (clockCut)
    {
        txWatchTimedOut = true;
        txWatchCutActive = true;
    }

    if (guardAbort || clockCut || clockStuck)
    {
        /*
         * Request the abort once, then repeat it every 20 ms until the mode
         * leaves transmit: the first attempt may have been refused by a radio
         * driver that was busy, and this is the last line before the watchdog.
         */
        if (!txAbortPending) {
            txAbortPending = true;
            txAbortTime = now;
            abort = true;
        } else if ((now - txAbortTime) >= 20) {
            txAbortTime = now;
            abort = true;
        }
    }

    const bool byClock = clockCut || (txWatchCutActive && (!guardAbort));
    pthread_mutex_unlock(&txMutex);

    /* The line goes out once per cut; the abort itself is retried */
    if (clockCut)
        logTxTimeout(now);

    if (abort)
        abortTransmission(now, byClock ? "clock" : txg_reasonName(txGuard.reason));
}

void rtx_task()
{
    /*
     * Spectrum analyser. While the analyser's screen is open the sweep owns the
     * receiver, and it does so from this thread because this is the thread that
     * owns the chip: the RSSI read below and the sweep would otherwise fight
     * over the same tuning word, and every bin would look like a squelch
     * opening to the mode.
     *
     * The service returns false when it has parked the receiver on the centre
     * frequency with its own filter and audio, and the rest of this function
     * then runs unchanged: that is how the squelch, the audio path and the
     * mode's own decisions keep working between two sweeps, and how a carrier
     * on the centre pauses the sweep instead of being chopped by it.
     */
    if (spectrumService_poll())
        return;

    /*
     * A transmission the supervisor had to abort: it already unkeyed the PA and
     * cancelled the streams, so all that is left is to leave the mode's
     * transmit state -- the mode's own exit path releases the audio paths and
     * stops the encoder, from the thread that owns them.
     */
    bool aborted = false;
    pthread_mutex_lock(&txMutex);
    if (txAbortPending) {
        txAbortPending = false;
        aborted = true;
    }
    pthread_mutex_unlock(&txMutex);

    if (aborted)
    {
        /*
         * The supervisor unkeyed already; this thread owns the chip, so it
         * repeats it (idempotent) and makes sure the receiver is what the radio
         * is left in, even if the mode's own unwind does not reach its RX
         * branch (that is the case this exists for).
         */
        rtxStatus.opStatus = OFF;
        radio_disableRtx();
    }

    // Check if there is a pending new configuration and, in case, read it.
    bool reconfigure = false;
    if(pthread_mutex_trylock(cfgMutex) == 0)
    {
        if(newCnf != NULL)
        {
            // Copy new configuration and override opStatus flags
            uint8_t tmp = rtxStatus.opStatus;
            memcpy(&rtxStatus, newCnf, sizeof(rtxStatus_t));
            rtxStatus.opStatus = tmp;

            reconfigure = true;
            newCnf = NULL;
        }

        pthread_mutex_unlock(cfgMutex);
    }

    if(reconfigure)
    {
        // Force TX and RX tone squelch to off for OpModes different from FM.
        if(rtxStatus.opMode != OPMODE_FM)
        {
            rtxStatus.txToneEn = 0;
            rtxStatus.rxToneEn = 0;
        }

        /*
         * Handle change of opMode:
         * - deactivate current opMode and switch operating status to "OFF";
         * - update pointer to current mode handler to the OpMode object for the
         *   selected mode;
         * - enable the new mode handler
         */
        if(currMode->getID() != rtxStatus.opMode)
        {
            /*
             * One block, one mode: the old object is destroyed and its memory
             * given back before the new one is constructed in it, so the radio
             * never holds two modes' state (docs/MEMORY.md).
             */
            switchMode(rtxStatus.opMode);
        }

        // Tell radio driver that there was a change in its configuration.
        radio_updateConfiguration();
    }

    /*
     * RSSI update block, run only when radio is in RX mode.
     *
     * RSSI value is passed through a filter with a time constant of 60ms
     * (cut-off frequency of 15Hz) at an update rate of 33.3Hz.
     *
     * The low pass filter skips an update step if a new configuration has
     * just been applied. This is a workaround for the AT1846S returning a
     * full-scale RSSI value immediately after one of its parameters changed,
     * thus causing the squelch to open briefly.
     *
     * Also, the RSSI filter is re-initialised every time radio stage is
     * switched back from TX/OFF to RX. This provides a workaround for some
     * radios reporting a full-scale RSSI value when transmitting.
     */
    if(rtxStatus.opStatus == RX)
    {

        if(!reconfigure)
        {
            if(!reinitFilter)
            {
                /*
                 * Filter RSSI value using 15.16 fixed point math. Equivalent
                 * floating point code is: rssi = 0.74*radio_getRssi() + 0.26*rssi
                 */
                int32_t filt_rssi = radio_getRssi() * 0xBD70    // 0.74 * radio_getRssi
                                  + rssi            * 0x428F;   // 0.26 * rssi
                rssi = (filt_rssi + 32768) >> 16;               // Round to nearest
            }
            else
            {
                rssi = radio_getRssi();
                reinitFilter = false;
            }
        }
    }
    else
    {
        // Reinit required if current operating status is TX or OFF
        reinitFilter = true;
    }

    /*
     * Forward the periodic update step to the currently active opMode handler.
     * Call is placed after RSSI update to allow handler's code have a fresh
     * version of the RSSI level.
     */
    currMode->update(&rtxStatus, reconfigure);

    /*
     * After an abort the receiver is the radio's state, whatever the mode did.
     * A keyed PA that nobody released is what this guards against, and a mode
     * that unwinds without reaching its RX branch would otherwise leave a
     * silent radio instead of a working one.
     */
    if (aborted && (rtxStatus.opStatus != TX) && (radio_getStatus() != RX))
        radio_enableRx();

    logTxExit();
}

rssi_t rtx_getRssi()
{
    return rssi;
}

bool rtx_rxSquelchOpen()
{
    /*
     * Before rtx_init() there is no mode yet: answer "no carrier" instead of
     * dereferencing a null pointer, because the UI and the scan ask this
     * before the RTX thread has started (and the host tests never start it).
     */
    return (currMode != NULL) ? currMode->rxSquelchOpen() : false;
}

/**
 * A platform that cannot read its PA lines back answers from its own status.
 * The C62 overrides this with a read of the register that drives them.
 */
extern "C" __attribute__((weak)) bool radio_txPathOff(void)
{
    return radio_getStatus() != TX;
}

bool rtx_isSwitching(void)
{
    pthread_mutex_lock(&txMutex);
    const bool value = switching;
    pthread_mutex_unlock(&txMutex);
    return value;
}

uint8_t rtx_currentOpMode(void)
{
    return (currMode != NULL) ? (uint8_t)currMode->getID() : (uint8_t)OPMODE_NONE;
}

struct keyGate *rtx_getKeyGate()
{
    return &keyGate;
}

struct txWatch *rtx_getTxWatch()
{
    return &txWatch;
}

const char *rtx_txWatchReason()
{
    return txWatchTimedOut ? "timeout" : txWatchSource;
}

bool rtx_keyingAllowed(const rtxStatus_t *status)
{
    const uint32_t now = (uint32_t)getTick();
    const bool ptt = platform_getPttStatus() ? true : false;

    pthread_mutex_lock(&txMutex);
    keyGate_sample(&keyGate, now, ptt);
    /*
     * The absolute clock vetoes keying too: once a transmission has used its
     * 90 s, nothing keys until the radio has been released, whatever the gate
     * says about releases and hold times.
     */
    const bool clockOk = txw_keyAllowed(&txWatch, now);
    bool allowed = (!switching) && clockOk && keyGate_pttKey(&keyGate, now);
    if ((!allowed) && ptt && (!switching))
        keyGate_noteVeto(&keyGate);
    pthread_mutex_unlock(&txMutex);

    /*
     * The reason, in the order the gate applies them.  "A press nobody made
     * yet" is not a refusal and stays "none": the field says why a
     * transmission cannot happen, not that the operator has not pressed.
     */
    const char *reason = "none";
    if (switching)
        reason = "mode change";
    else if (!clockOk)
        reason = "air limit";

    if ((status != NULL) && (status->txDisable != 0))
    {
        reason = "tx disabled";
        allowed = false;
    }

    /*
     * No power means no transmission (docs/TX_SAFETY.md).  A driver handed
     * 0 mW either refuses the keying (and, on the C62, used to do it in
     * silence: no LED, no RF, no error) or, worse, keys the PA at its minimum
     * because it clamps.  The gate is the last place that can say no while
     * there is still someone to tell: the reason goes into the status the
     * protocol reports and into the log.
     */
    if ((status != NULL) && (status->txPower == 0))
    {
        reason = "no tx power";
        allowed = false;
    }

    keyVeto = reason;
    return allowed;
}

const char *rtx_keyingVetoReason(void)
{
    return keyVeto;
}

bool rtx_unattendedTxAllowed(const rtxStatus_t *status)
{
    if (status != NULL && status->txDisable != 0)
        return false;

    const uint32_t now = (uint32_t)getTick();
    const bool ptt = platform_getPttStatus() ? true : false;

    pthread_mutex_lock(&txMutex);
    keyGate_sample(&keyGate, now, ptt);
    const bool allowed = (!switching) && txw_keyAllowed(&txWatch, now)
                      && keyGate_unattended(&keyGate, now);
    if (allowed)
        keyGate_grantUnattended(&keyGate, now);
    pthread_mutex_unlock(&txMutex);

    return allowed;
}

bool rtx_txPermitted(void)
{
    const uint32_t now = (uint32_t)getTick();
    const bool ptt = platform_getPttStatus() ? true : false;

    pthread_mutex_lock(&txMutex);
    keyGate_sample(&keyGate, now, ptt);
    const bool permitted = (!switching) && txw_keyAllowed(&txWatch, now)
                        && (keyGate_pttKey(&keyGate, now)
                            || (ptt == false
                                && keyGate_unattended(&keyGate, now)
                                && keyGate_unattendedGranted(&keyGate, now)));
    pthread_mutex_unlock(&txMutex);

    return permitted;
}

bool opmode_canTransmit(uint8_t mode)
{
    switch (mode) {
        case OPMODE_FM:
            return true;
        case OPMODE_M17:
            return true;
        case OPMODE_DMR:
#ifdef CONFIG_DMR_TX
            /* Transmit chain and AMBE+2 encoder compiled in: duplex mode */
            return true;
#else
            /* Receive only: the PTT branch is compiled out */
            return false;
#endif
        case OPMODE_TETRA:
            /* Receive only: OpMode_TETRA forces the status back to RX */
            return false;
        case OPMODE_DSTAR:
#ifdef CONFIG_DSTAR_TX
            /* The transmit chain and its stage machine are compiled in */
            return true;
#else
            /* Receive only: nothing keys the carrier in this build */
            return false;
#endif
        case OPMODE_AIRBAND:
#ifdef CONFIG_C62_AIR_TX
            /*
             * AM transmit by modulating the PA supply: the mode asks for the
             * operator's confirmation before it keys anything, so a "yes" here
             * only means the transmitter exists, not that it will transmit.
             */
            return true;
#else
            /* Receive only: nothing keys the carrier in this build */
            return false;
#endif
        case OPMODE_P25:
#ifdef CONFIG_P25_TX
            /* Header, voice when a source is installed, and the terminator */
            return true;
#else
            return false;
#endif
        case OPMODE_YSF:
#ifdef CONFIG_YSF_TX
            /* V/D mode 1: header, voice, terminator */
            return true;
#else
            return false;
#endif
        case OPMODE_NXDN:
#ifdef CONFIG_NXDN_TX
            /* Preamble, voice, release and tail, on the DMR PA chain */
            return true;
#else
            return false;
#endif
        case OPMODE_DPMR:
#ifdef CONFIG_DPMR_TX
            /* Header, superframes and END frame on the same 4FSK chain */
            return true;
#else
            return false;
#endif
        case OPMODE_WFM_BC:
            /* Receive only: there is no transmitter for this mode */
            return false;
        default:
            return false;
    }
}
