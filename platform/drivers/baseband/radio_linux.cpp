/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include "interfaces/delays.h"
#include <cstdio>
#include <string>

/*
 * The emulator's transmitter, with the same two choke points as the radio: the
 * PA only comes up while the RTX layer permits a transmission (rtx_txPermitted,
 * rtx.h) and while the driver's own transmit clock has air left (rtx/TxWatch.h,
 * docs/TX_SAFETY.md). The counters are what the host tests read
 * (tests/unit/mode_switch.cpp): a mode change must never increment any of them,
 * a normal PTT keys once, and a keying the barrier ended is refused and forced
 * off however many times it is asked for.
 */
static enum opstatus radioStatus = OFF;
/* Mode the driver was last configured for, and the host tests' refuse hook */
static enum opmode radioMode = OPMODE_NONE;
static enum opmode rxRefuseMode = OPMODE_NONE;
static bool rxRefuse;
static unsigned keyed = 0;
static unsigned keysRefused = 0;
static unsigned barrierCuts = 0;

/*
 * The second barrier. The radio's version also arms a kernel timer; here the
 * test advances the clock itself (radio_txWatch()) and calls radio_enableTx()
 * as many times as it likes, which is the loop the barrier has to survive.
 */
static struct txWatch hostWatch;
static bool hostWatchReady = false;

static uint32_t hostNow()
{
    return (uint32_t)getTick();
}

static void hostBarrierUp()
{
    if (!hostWatchReady)
    {
        txw_init(&hostWatch, hostNow(), TXW_DRIVER_LIMIT_MS);
        hostWatchReady = true;
    }
}

/** How many times the PA was keyed since boot (host tests). */
unsigned radio_linux_keyCount(void)
{
    return keyed;
}

/** How many keying attempts the gate refused (host tests). */
unsigned radio_linux_refusedCount(void)
{
    return keysRefused;
}

/** How many keyings the driver's own barrier ended (host tests). */
unsigned radio_linux_barrierCount(void)
{
    return barrierCuts;
}

/** Forget the counters (host tests). */
void radio_linux_clearCounters(void)
{
    keyed = 0;
    keysRefused = 0;
    barrierCuts = 0;
}

void radio_init(const rtxStatus_t *rtxState)
{
    (void) rtxState;
    radioStatus = OFF;
    hostWatchReady = false;
    hostBarrierUp();
    radio_paForceOff();
    puts("radio_linux: init() called, PA off");
}

void radio_terminate()
{
    puts("radio_linux: terminate() called");
}

void radio_setOpmode(const enum opmode mode)
{
    radioMode = mode;

    std::string mStr(" ");
    if(mode == OPMODE_NONE) mStr = "NONE";
    if(mode == OPMODE_FM)   mStr = "FM";
    if(mode == OPMODE_DMR)  mStr = "DMR";
    if(mode == OPMODE_M17)  mStr = "M17";

    printf("radio_linux: setting opmode to %s\n", mStr.c_str());
}

/*
 * The CTCSS decoder lives in the transceiver, so the host has no way of hearing
 * a tone. It answers from a flag the tests set instead, which is what lets the
 * FM mode's tone-squelch rule be walked without a radio: it starts as "no
 * tone", and the test declares that the chip has decoded one.
 */
static bool digitalSquelch = false;

bool radio_checkRxDigitalSquelch()
{
    return digitalSquelch;
}

/* Host tests: what the chip reports about the CTCSS tone of the carrier. */
void radio_linux_setDigitalSquelch(bool present)
{
    digitalSquelch = present;
}

/*
 * Host tests: make the receiver refuse to start in one mode, the way a driver
 * whose chip does not come up with the profile of that mode does. The mode then
 * never reports RX, which is the case the mode switch has to survive (revert to
 * the mode it came from and say so).
 */
void radio_linux_refuseRx(uint8_t mode, bool refuse)
{
    rxRefuseMode = static_cast<opmode>(mode);
    rxRefuse = refuse;
}

void radio_enableRx()
{
    if (rxRefuse && (radioMode == rxRefuseMode))
        return;

    radioStatus = RX;
}

void radio_enableTx()
{
    /* Same contract as the radio: no permitted transmission, no PA */
    if (!rtx_txPermitted())
    {
        keysRefused++;
        return;
    }

    /* And the driver's own barrier: spent means no keying, whatever the gate */
    hostBarrierUp();
    txw_sample(&hostWatch, hostNow(), radioStatus == TX);
    if (!txw_keyAllowed(&hostWatch, hostNow()))
    {
        txw_tick(&hostWatch, hostNow());
        barrierCuts++;
        radio_paForceOff();
        return;
    }

    keyed++;
    radioStatus = TX;
    txw_sample(&hostWatch, hostNow(), true);
}

void radio_disableRtx()
{
    if (hostWatchReady)
        txw_sample(&hostWatch, hostNow(), false);

    radioStatus = OFF;
}

bool radio_txPathOff(void)
{
    return radioStatus != TX;
}

bool radio_paForceOff(void)
{
    hostBarrierUp();
    radioStatus = OFF;
    txw_sample(&hostWatch, hostNow(), false);
    return true;
}

struct txWatch *radio_txWatch(void)
{
    hostBarrierUp();
    return &hostWatch;
}

void radio_updateConfiguration()
{
    puts("radio_linux: updateConfiguration() called");
}

rssi_t radio_getRssi()
{
    // Commented to reduce verbosity on Linux
    // printf("radio_linux: requested RSSI at freq %d, returning -100dBm\n", rxFreq);
    return static_cast< rssi_t >(emulator_state.RSSI);
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}

const char *radio_txRefusedReason()
{
    return "none";
}
