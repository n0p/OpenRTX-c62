/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/radio.h"
#include "rtx/OpMode_WFM.hpp"
#include "rtx/rtx.h"
#include "core/state.h"
#include "core/utils.h"
#include "core/wfm.h"

/*
 * How often the tuner is asked for its status, ms.  The chip answers a
 * two-register read in about 0.3 ms on the bit-banged bus, so 10 Hz costs
 * nothing; the S-meter and the stereo flag are what the operator sees and
 * they do not need to move faster.
 */
static constexpr uint32_t WFM_POLL_MS = 100;

/*
 * Time the status has to stay bad before the mode calls the channel lost.
 * A moving receiver loses AFC lock for a moment on every retune and in every
 * fade; the flag only exists to tell the screen whether there is a station.
 */
static constexpr uint32_t WFM_LOST_MS = 500;

/** How long the mode waits for state_mutex before skipping a publish, ms. */
static constexpr uint32_t WFM_STATE_LOCK_MS = 5;

OpMode_WFM::OpMode_WFM() : enterRx(true), carrier(false), lastPoll(0), pollMs(0)
{
}

OpMode_WFM::~OpMode_WFM()
{
}

void OpMode_WFM::enable()
{
    enterRx = true;
    carrier = false;
    lastPoll = 0;
    pollMs = 0;
}

void OpMode_WFM::disable()
{
    radio_disableRtx();

    enterRx = false;
    carrier = false;
}

void OpMode_WFM::update(rtxStatus_t *const status, const bool newCfg)
{
    /*
     * Receive only.  A held PTT is not refused with silence: the status goes
     * back to RX and the receiver keeps playing, which is what the operator
     * sees on the screen as well.  radio_enableTx() would not key the PA in
     * this mode either (see radio_C62.cpp), but it is never called.
     */
    if (status->opStatus == TX)
        status->opStatus = RX;

    if ((status->opStatus == OFF) && enterRx) {
        /*
         * Entering, or coming back from OFF: power the tuner, tune the channel
         * and switch the analog audio path to the speaker.  radio_enableRx()
         * does all three for this mode because the audio does not go through
         * the codec and so there is no audio path to request.
         */
        radio_enableRx();
        status->opStatus = RX;
        enterRx = false;
        lastPoll = 0;
    }

    if (status->opStatus != RX)
    {
        /* Nothing to poll when not receiving, but not for free: this is a
         * thread, and a mode that returns without yielding spins the core. */
        sleepFor(0, 20);
        return;
    }

    const uint32_t now = (uint32_t)getTick();

    if (newCfg) {
        /* Retuned: the previous status describes another channel */
        carrier = false;
        lastPoll = 0;
    }

    if ((lastPoll != 0) && ((now - lastPoll) < WFM_POLL_MS))
    {
        sleepFor(0, 5);
        return;
    }

    lastPoll = now;

    wfm_status_t st;
    if (!wfm_poll(&st)) {
        /* No answer from the tuner: report the channel as lost, keep trying */
        pollMs += WFM_POLL_MS;
        if (pollMs >= WFM_LOST_MS)
            carrier = false;
        sleepFor(0, 20);
        return;
    }

    st.freq = status->rxFrequency;
    st.rds = false;     /* no RDS decoder in this receiver */

    /*
     * A station is a channel where a tune completed and the AFC is not
     * railed.  The chip soft mutes its own audio when AFCRL is set, so this
     * is the same criterion it uses, not a second opinion.
     */
    bool ok = st.tuned && !st.afcrl;

    if (ok) {
        carrier = true;
        pollMs = 0;
    } else {
        pollMs += WFM_POLL_MS;
        if (pollMs >= WFM_LOST_MS)
            carrier = false;
    }

    wfm_publish(&st);

    /*
     * Mirror it into the shared state so that screen code reads broadcast FM
     * the way it reads everything else.  Bounded, and skipped when the UI holds
     * the lock: the next poll in 100 ms will publish the same picture, while a
     * plain pthread_mutex_lock() here parks the RTX thread behind the UI --
     * which is the thread that has to keep the receiver alive.
     */
    if (state_tryLock(WFM_STATE_LOCK_MS))
    {
        state.wfm = st;
        state_unlock();
    }

    /* One poll per WFM_POLL_MS, and a thread that must not spin while it
     * waits for the next one. */
    sleepFor(0, 10);
}

bool OpMode_WFM::rxSquelchOpen()
{
    return carrier;
}
