/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>
#include "rtx/rtx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This file provides a common interface for the platform-dependent low-level
 * rtx driver. Top level application code normally does not have to call directly
 * the API functions provided here, since all the transceiver managment, comprised
 * the handling of digital protocols is done by the 'rtx' module.
 *
 * The radio functionalities are controlled by means of an rtxStatus_t data
 * structure containing all the parameters required to define a given operating
 * configuration for the RF stage such as TX and RX frequencies, ...
 * The data structure is internally accessed by each of the API functions and is
 * guaranteed that the access is performed in read only mode.
 */

/**
 * Initialise low-level radio transceiver.
 *
 * @param rtxState: pointer to an rtxStatus_t structure used to describe the
 * operating configuration of the radio module.
 */
void radio_init(const rtxStatus_t *rtxState);

/**
 * Shut down low-level radio transceiver.
 */
void radio_terminate();

/**
 * Set current operating mode.
 *
 * @param mode: new operating mode.
 */
void radio_setOpmode(const enum opmode mode);

/**
 * Check if digital squelch is opened, that is if a CTC/DCS code is being
 * detected.
 *
 * @return true if RX digital squelch is enabled and if the configured CTC/DCS
 * code is present alongside the carrier.
 */
bool radio_checkRxDigitalSquelch();

/**
 * Enable AF output towards the speakers.
 */
void radio_enableAfOutput();

/**
 * Disable AF output towards the speakers.
 */
void radio_disableAfOutput();

/**
 * Enable the RX stage.
 */
void radio_enableRx();

/**
 * Enable the TX stage.
 */
void radio_enableTx();

/**
 * Disable both the RX and TX stages, as long as transmission of CTC/DCS code
 * and digital squelch.
 */
void radio_disableRtx();

/**
 * Update configuration of the radio module to match the one currently described
 * by the rtxStatus_t configuration data structure.
 * This function has to be called whenever the configuration data structure has
 * been updated, to ensure all the operating parameters of the radio driver are
 * correctly configured.
 */
void radio_updateConfiguration();

/**
 * Set a fine offset of the receive frequency, added to the configured one,
 * for an automatic frequency correction. Applied at once when receiving.
 * Implemented only by radios with CONFIG_TETRA_AFC.
 *
 * @param hz: offset in Hz.
 */
void radio_setRxOffset(int32_t hz);

/**
 * Get the current RSSI level in dBm.
 *
 * @return RSSI level in dBm.
 */
rssi_t radio_getRssi();

/**
 * Get the current operating status of the radio module.
 *
 * @return current operating status.
 */
enum opstatus radio_getStatus();

/*
 * Spectrum analyser.
 *
 * The receiver the C62 uses (BK4819) has no I/Q output: what it can be asked
 * for is the RSSI of the frequency it is tuned to, and the audio of the
 * demodulator. A spectrum is therefore a sweep: tune, let the PLL settle, read
 * REG_67. These four calls are that, and only the platforms whose receiver can
 * be tuned a single word at a time implement them (the C62 does; nothing else
 * calls them). See core/spectrum_service.h for who is allowed to call them and
 * when, and docs/SPECTRUM.md for the numbers.
 */

/**
 * Take the receiver over for sweeping: remember the IF filter and the audio
 * routing, select the widest receive path and mute the AF output.
 *
 * @return false if the receiver is not receiving (off or transmitting).
 */
bool radio_spectrumEnter(void);

/**
 * Switch between the sweep profile and the normal one.
 *
 * @param on: true selects the widest IF and mutes the audio, false restores the
 *            filter and the AF output the mode had.
 */
void radio_spectrumSweep(bool on);

/**
 * One point of the sweep: tune, wait for the synthesizer, read the level.
 *
 * @param frequency: frequency to tune, Hz.
 * @param settleUs: time given to the PLL before the level is read, us. Zero
 *                  means "ask the chip": the driver polls the glitch indicator
 *                  and reads as soon as the IF has recovered (see
 *                  docs/SPECTRUM.md §3), with a bounded number of polls.
 * @param pointUs: filled with the measured duration of the whole point, us.
 * @return RSSI level of that frequency, dBm.
 */
int16_t radio_spectrumPoint(uint32_t frequency, uint32_t settleUs,
                            uint32_t *pointUs);

/**
 * What the last sweep session found out about the receiver's settling.
 *
 * Optional: only the platforms that implement the sweep define it, and the
 * service calls it behind PLATFORM_C62. It exists so the operator can see
 * whether the waiting was done by the chip's own indicator or by a clock.
 *
 * @param polls: average number of glitch polls per point, 0 if unused.
 * @return false when the gate has been given up on for this session.
 */
bool radio_spectrumGate(uint32_t *polls);

/**
 * Put the receiver back on the configured frequency, with its own filter and
 * audio, and stay there: the normal receive path runs again from here.
 */
void radio_spectrumPark(void);

/** Give the receiver back after a sweep session. */
void radio_spectrumLeave(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_H */
