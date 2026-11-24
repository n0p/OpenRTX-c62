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
 * The band is receive only by default. Built with CONFIG_C62_AIR_TX the mode
 * can also transmit, by modulating the PA supply with the voice envelope (see
 * docs/TX_AIRBAND.md) -- the BK4819 has no amplitude modulator of its own --
 * and then it manages a transmitter as well:
 *
 *  - the confirmation gate of core/airband.h: the first PTT on a frequency of
 *    the band raises a warning instead of keying, and only a second press, with
 *    the warning on screen, arms the mode for the rest of the power cycle.
 *    Nothing about that is stored: a radio switched off is locked again;
 *
 * The rest of the class manages a receiver:
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

#ifdef CONFIG_C62_AIR_TX
    /**
     * Stop a transmission: close the microphone path and turn the radio
     * around, letting the driver ramp the carrier down first.
     *
     * @param status: RTX status, set back to RX.
     */
    void stopTx(rtxStatus_t *const status);

    /**
     * Ask for the operator's confirmation, or key the transmitter if he has
     * already given it.
     *
     * @param status: RTX status.
     */
    void requestTx(rtxStatus_t *const status);

    /**
     * The transmit supervisor unkeyed the radio: close the microphone path and
     * leave the mode ready to receive again.
     */
    virtual void abortTx() override;
#endif

    bool   carrierOpen; ///< Carrier above the squelch threshold.
    bool   sqlOpen;     ///< Audio path open.
    bool   enterRx;     ///< Still has to start the receiver.
    pathId rxAudioPath; ///< Audio path ID for RX.
    uint32_t hangUntil; ///< Tick at which an open squelch may close again.
    bool   pttLatch;    ///< PTT already acted on, for the edge detection.
    uint32_t promptSince; ///< Tick at which the warning went on screen.
    pathId txAudioPath; ///< Audio path ID for TX.
};

#endif /* OPMODE_AIRBAND_H */
