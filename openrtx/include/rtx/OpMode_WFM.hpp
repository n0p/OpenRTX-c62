/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_WFM_H
#define OPMODE_WFM_H

#include "OpMode.hpp"

/**
 * Commercial FM broadcast reception (WFM), 85.0-108.0 MHz.
 *
 * This mode has no demodulator, and that is the point: the radio carries a
 * dedicated broadcast receiver (a Beken BK1080) with its own LNA, IF, stereo
 * decoder and audio DACs, and its output goes to the speaker amplifier
 * through the analog volume pot, straight past the CSK6 codec and the HiFi4.
 * The mode therefore manages three things and nothing else:
 *
 *  - the receiver: the radio driver powers the tuner, writes its
 *    configuration and tunes the channel (platform/drivers/baseband/
 *    radio_C62.cpp, drivers/baseband/BK1080.c);
 *  - the analog audio path: the speaker amplifier has to be on while the
 *    mode receives.  This is not the codec path, so it is opened through
 *    radio_enableRx() and not through audioPath_request(), which would also
 *    start the codec DAC and mix it into the same amplifier input;
 *  - the status: level, stereo/mono and channel quality, read from the tuner
 *    at 10 Hz and published in state.wfm and through core/wfm.h.
 *
 * Receive only, by definition: PTT is refused, the PA is never keyed, and
 * opmode_canTransmit() says so as well.
 *
 * There is no RDS/RadioText on this hardware: the BK1080 has no RDS
 * demodulator (its register map ends at 0x21 and has no group decoder), and
 * what it outputs is decoded stereo audio, not the MPX that an external RDS
 * decoder would need.  See docs/RX_WFM.md.
 */
class OpMode_WFM : public OpMode
{
public:

    /**
     * Constructor.
     */
    OpMode_WFM();

    /**
     * Destructor.
     */
    ~OpMode_WFM();

    /**
     * Enable the operating mode.
     */
    virtual void enable() override;

    /**
     * Disable the operating mode: turns the receiver off and closes the
     * analog audio path.
     */
    virtual void disable() override;

    /**
     * Update the internal FSM and poll the receiver status.
     *
     * @param status: pointer to the rtxStatus_t structure holding the current
     * RTX status.  Only the operating status is changed, and only from TX
     * back to RX: this mode has no transmitter.
     * @param newCfg: true when a new RTX configuration has been applied, which
     * forces a status poll on the new channel.
     */
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    /**
     * Get the mode identifier corresponding to the OpMode class.
     *
     * @return OPMODE_WFM_BC.
     */
    virtual opmode getID() override
    {
        return OPMODE_WFM_BC;
    }

    /**
     * Check if a station is being received: a tune has completed and the AFC
     * is not railed, which is the receiver's own idea of a valid channel.
     */
    virtual bool rxSquelchOpen() override;

private:

    bool     enterRx;   ///< Still has to start the receiver
    bool     carrier;   ///< Receiver reports a valid channel
    uint32_t lastPoll;  ///< Tick of the last status poll
    uint32_t pollMs;    ///< Interval between status polls, ms
};

#endif /* OPMODE_WFM_H */
