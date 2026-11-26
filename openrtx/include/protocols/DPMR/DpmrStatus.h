/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DPMR_STATUS_H
#define OPENRTX_DPMR_STATUS_H

/**
 * @file DpmrStatus.h
 * @brief What the dPMR receive mode has decoded, for the UI.
 *
 * The mode used to keep everything a burst gives in private accessors of
 * OpMode_DPMR that nothing read, so the main screen could only say "dPMR RX" or
 * "SRCH" (it asked the frame grid, rtx_rxSquelchOpen()). This module is the
 * published copy, the same shape as NXDN's NxdnStatus.h: written by the rtx
 * thread one frame at a time, read by the UI with a struct copy
 * (dpmr_getStatus()), and turned into the texts of the main screen by
 * ui/default/ui_mode_state.c.
 *
 * What a dPMR burst really gives, and where each field comes from:
 *
 *  - `sync`          a frame of any kind decoded since the mode reset;
 *  - `channelCode`   the 12 bit channel code of clause 6.1.5, which rides in
 *                    the frames that open with it (2 and 4 of a superframe) and
 *                    in the header frame, -1 until one decodes;
 *  - `callType`      VCALL from a header of type start, DISC from a disconnect
 *                    request, TX_REL from the END frame that closes the
 *                    transmission -- the three events a call has;
 *  - `source`/`dest` the 24 bit own and called addresses of the header frame,
 *                    which is the only frame that carries them whole;
 *  - `endSeen`       an END frame decoded, with the 5 bit status message of
 *                    clause 5.15 when its end type says there is one;
 *  - `evm`/counters  what the demodulator measured and how much it decoded.
 *
 * Nothing here is a setting and nothing here is written from the UI.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dpmrFrame;

/** The call events a dPMR transmission has, in the vocabulary NXDN uses. */
enum dpmrCallType
{
    DPMR_CALLTYPE_NONE = 0, /**< Nothing heard yet                     */
    DPMR_CALLTYPE_VCALL,    /**< A header of type start opened a call  */
    DPMR_CALLTYPE_TX_REL,   /**< An END frame closed the transmission  */
    DPMR_CALLTYPE_DISC      /**< A disconnect request was received     */
};

struct dpmrStatus
{
    bool sync;            /**< A frame decoded since the mode reset      */
    bool receiving;       /**< The frame grid is locked right now        */
    bool voice;           /**< A TCH decoded as AMBE+2 since the reset   */
    int16_t channelCode;  /**< 12 bit channel code, -1 until decoded     */

    uint8_t callType;     /**< enum dpmrCallType, the last call event     */
    bool idsValid;        /**< The header carried both addresses          */
    uint32_t source;      /**< Own (transmitting) 24 bit address           */
    uint32_t dest;        /**< Called 24 bit address                       */
    uint8_t mode;         /**< Communication mode of the call (clause 5.7),
                               0xFF until a header decodes                */
    uint8_t ht;           /**< Header type of the last header (clause 5.11),
                               0xFF until one decodes                     */

    bool endSeen;         /**< An END frame decoded since the reset       */
    bool endHasStatus;    /**< That END carried ET = 01 (clause 5.15)     */
    uint8_t endStatus;    /**< Its 5 bit status message                   */
    uint8_t endWait;      /**< Tx wait of the last END (clause 5.14)      */

    uint16_t evm;         /**< EVM of the last frame, in thousandths      */

    uint32_t frames;      /**< Frames decoded since the mode was enabled  */
    uint32_t cchErrors;   /**< Payload frames whose CCH CRC-7 failed      */
    uint32_t voiceFrames; /**< Of which carried voice                     */
    uint32_t ambeFrames;  /**< AMBE+2 frames handed to the vocoder        */
    uint32_t headers;     /**< Header frames whose HI0 passed its CRC     */
    uint32_t ends;        /**< END frames that decoded                    */
};

/** Clear the state, keeping the counters (new frequency, mode change). */
void dpmrStatus_reset(void);

/** Clear everything, counters included (leaving the mode). */
void dpmrStatus_clear(void);

/**
 * @brief Account one decoded frame, call from the rtx thread.
 *
 * The counters, the channel code, the header's addresses and mode, the call
 * event the frame means and the END status. A frame whose CCH did not close its
 * CRC still counts as a frame and as an error, but publishes no identifier.
 */
void dpmrStatus_frame(const struct dpmrFrame *frame);

/**
 * @brief The mode's squelch, as dpmrPhy_synced() reports it.
 *
 * Kept apart from `sync` on purpose: `sync` says the fields below belong to a
 * call that was heard (and stays true so the operator can read the release
 * after the transmission ends), `receiving` says the grid is locked now.
 */
void dpmrStatus_receiving(bool on);

/** Copy the current status. */
void dpmr_getStatus(struct dpmrStatus *out);

/** Name of a call type for the screen: "VCALL", "TX_REL", "DISC", NULL. */
const char *dpmrStatus_callName(uint8_t callType);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DPMR_STATUS_H */
