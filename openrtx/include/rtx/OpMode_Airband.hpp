/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_AIRBAND_H
#define OPMODE_AIRBAND_H

#include "core/audio_path.h"
#include "OpMode.hpp"

/**
 * Operating mode for the aeronautical band, 118.000-136.975 MHz, AM voice.
 *
 * Receive only: the band is not one this radio may transmit in, and the
 * amplitude modulation the aircraft use cannot be produced by a
 * frequency-modulating transmitter anyway. The class therefore manages a
 * receiver and nothing else:
 *
 *  - the signal path, which the radio driver configures (AM detector, IF
 *    filter, AGC, and the audio processing that holds the level);
 *  - the squelch, which in AM is a *carrier* squelch: the audio of an AM
 *    receiver follows the level of the carrier, so a noise squelch would open
 *    on the noise the AGC has brought up and closed when the carrier appears.
 *    The receiver's RSSI is the carrier, so that is what this uses, with
 *    hysteresis and a hang time so that a fading aircraft does not make the
 *    speaker chatter;
 *  - the audio path to the speaker, opened and closed by the squelch.
 */
class OpMode_Airband : public OpMode
{
public:

    /**
     * Constructor.
     */
    OpMode_Airband();

    /**
     * Destructor.
     */
    ~OpMode_Airband();

    /**
     * Enable the operating mode.
     */
    virtual void enable() override;

    /**
     * Disable the operating mode: closes the audio path and turns the
     * receiver off.
     */
    virtual void disable() override;

    /**
     * Update the internal FSM.
     *
     * @param status: pointer to the rtxStatus_t structure containing the
     * current RTX status. Only the operating status is changed, and only from
     * TX back to RX: this mode has no transmitter.
     * @param newCfg: flag used inform the internal FSM that a new RTX
     * configuration has been applied.
     */
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    /**
     * Get the mode identifier corresponding to the OpMode class.
     *
     * @return the corresponding flag from the opmode enum.
     */
    virtual opmode getID() override
    {
        return OPMODE_AIRBAND;
    }

    /**
     * Check if RX squelch is open.
     *
     * @return true if RX squelch is open.
     */
    virtual bool rxSquelchOpen() override;

private:

    bool   carrierOpen; ///< Carrier above the squelch threshold.
    bool   sqlOpen;     ///< Audio path open.
    bool   enterRx;     ///< Still has to start the receiver.
    pathId rxAudioPath; ///< Audio path ID for RX.
    uint32_t hangUntil; ///< Tick at which an open squelch may close again.
};

#endif /* OPMODE_AIRBAND_H */
