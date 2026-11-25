/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include <cstdio>
#include <string>

/*
 * The emulator's transmitter, with the same choke point as the radio: the PA
 * only comes up while the RTX layer permits a transmission. The counters are
 * what the host tests read (tests/unit/mode_switch.cpp): a mode change must
 * never increment either of them, and a normal PTT keys once.
 */
static enum opstatus radioStatus = OFF;
static unsigned keyed = 0;
static unsigned keysRefused = 0;

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

/** Forget the counters (host tests). */
void radio_linux_clearCounters(void)
{
    keyed = 0;
    keysRefused = 0;
}

void radio_init(const rtxStatus_t *rtxState)
{
    (void) rtxState;
    radioStatus = OFF;
    puts("radio_linux: init() called");
}

void radio_terminate()
{
    puts("radio_linux: terminate() called");
}

void radio_setOpmode(const enum opmode mode)
{
    std::string mStr(" ");
    if(mode == OPMODE_NONE) mStr = "NONE";
    if(mode == OPMODE_FM)   mStr = "FM";
    if(mode == OPMODE_DMR)  mStr = "DMR";
    if(mode == OPMODE_M17)  mStr = "M17";

    printf("radio_linux: setting opmode to %s\n", mStr.c_str());
}

bool radio_checkRxDigitalSquelch()
{
//     puts("radio_linux: radio_checkRxDigitalSquelch(), returning 'false'");
    return false;
}

void radio_enableRx()
{
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

    keyed++;
    radioStatus = TX;
}

void radio_disableRtx()
{
    radioStatus = OFF;
}

bool radio_txPathOff(void)
{
    return radioStatus != TX;
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
