/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_OPMODE_NXDN_HPP
#define OPENRTX_OPMODE_NXDN_HPP

/**
 * @file OpMode_NXDN.hpp
 * @brief NXDN receive mode: 4FSK demodulation of the discriminator output,
 * frame sync, LICH, SACCH and voice through the AMBE+2 decoder.
 *
 * Receive only: NXDN transmit would need the AMBE+2 encoder and a deviation
 * calibration this radio has not been through, and there is no NXDN
 * infrastructure to test it against.
 */

#include <stdint.h>

#include "core/state.h"
#include "rtx/OpMode.hpp"
#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"

class OpMode_NXDN : public OpMode
{
public:
    /**
     * @brief Instantiate the NXDN receive mode.
     *
     * @param channel: 0 for 12.5 kHz, 1 for 6.25 kHz.
     */
    explicit OpMode_NXDN();

    void enable() override;
    void disable() override;
    void update(rtxStatus_t *const status, const bool newCfg) override;
    bool rxSquelchOpen() override;

private:
    static void frameTrampoline(const struct nxdnFrame *frame, void *user);
    void onFrame(const struct nxdnFrame *frame);
    void startSampling();
    void stopSampling();

    /* The 72 bits of a superframe's SACCH, 18 per frame */
    uint8_t sacchBits[72];
    bool sacchHave[NXDN_SUPERFRAME];

    int basebandPath;
    int basebandId;
    int16_t settle;
    int dataErrors;
    bool startRx;
};

#endif /* OPENRTX_OPMODE_NXDN_HPP */
