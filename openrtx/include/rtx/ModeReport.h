/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MODE_REPORT_H
#define MODE_REPORT_H

/**
 * @file ModeReport.h
 * @brief What each mode publishes about the carrier it is receiving, in one
 *        place, for code that is not a mode (the VFO carrier search).
 *
 * Two questions are asked of a mode from outside, and both have the same
 * answer shape in every mode:
 *
 *   - is it synchronised, and how many frames has it decoded with their check
 *     passing?  A sync word on its own is a false positive; two or three
 *     frames behind it are a carrier.  This is what
 *     modeSyncCounters() reads, out of the per-mode status modules
 *     (DmrStatus.h, NxdnStatus.h, P25Status.h, TetraStatus.h);
 *   - does the network publish the frequency it is really on, or has the
 *     receiver measured the carrier offset?  That is
 *     modeReportedFrequency(), the hook a mode implements when its air
 *     interface carries a frequency.  Only TETRA does, today.
 *
 * @warning The counters are monotonic for the whole power on (the status
 * modules keep them across a retune on purpose) and they wrap at 2^32: a
 * caller that wants "frames since I started listening" compares against a
 * baseline taken when it started, and treats a smaller value as a reset.
 *
 * Which modes publish what (docs/VFO_SEARCH.md has the table with the
 * reasoning):
 *
 * | mode              | sync | frames | absolute frequency |
 * |-------------------|------|--------|--------------------|
 * | FM, AM airband    | --   | --     | no                 |
 * | Broadcast FM      | --   | --     | no                 |
 * | M17               | yes  | no     | no                 |
 * | DMR II / III      | yes  | yes    | **no**             |
 * | TETRA             | yes  | yes    | **yes**: SCH/F and |
 * |                   |      |        | SCH-HD SYSINFO, plus
 * |                   |      |        | the demodulator's  |
 * |                   |      |        | carrier offset     |
 * | NXDN              | yes  | yes    | no                 |
 * | P25 phase 1       | yes  | yes    | no                 |
 * | YSF               | yes  | no     | no                 |
 * | dPMR, D-STAR      | --   | --     | no                 |
 *
 * A DMR repeater's split, a NXDN channel's frequency, a P25 NAC's channel
 * plan and a YSF reflector's frequency are all configuration, not air
 * interface: those modes cannot correct the tuning and the search falls back
 * to whatever the operator dialled in.  Adding a mode that does publish one
 * (TETRA DMO with its own carrier, or a trunked control channel that names its
 * traffic channel) is one more case in ModeReport.c and nothing else.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a mode reports about its synchronisation and its frames. */
struct modeCarrierReport
{
    bool     synced;      /**< The mode's own detector says it is locked     */
    bool     haveFrames;  /**< `validFrames` is meaningful                   */
    uint32_t validFrames; /**< Frames decoded with their check passing       */
};

/**
 * Read the carrier report of a mode.
 *
 * Analog modes and modes whose status module is not compiled in answer with
 * `synced` false and `haveFrames` false; the caller then has to use whatever
 * else it has (the squelch, the RSSI, or the mode's own rxSquelchOpen()).
 *
 * @param mode: operating mode, from the opmode enum.
 * @param out: destination, always written.
 * @return true if the mode published anything (either field set).
 */
bool modeSyncCounters(uint8_t mode, struct modeCarrierReport *out);

/** The frequency a mode says the signal really is on. */
struct modeReportedFrequency
{
    bool     absolute;    /**< The network published its frequency           */
    uint32_t frequency;   /**< That frequency, Hz                            */
    bool     offsetValid; /**< A residual carrier offset was measured        */
    int32_t  offsetHz;    /**< Hz to add to the tuning to correct it         */
};

/**
 * Ask a mode for the frequency the network published, and for the carrier
 * offset its demodulator measured.
 *
 * This is the generic correction hook of the VFO carrier search: a mode that
 * knows where the signal really is answers with `absolute` and leaves the
 * search to retune there; a mode that only measures a small error answers
 * with `offsetValid` and `offsetHz`.  Everything the hook does not fill in is
 * "no correction", which is the answer of every mode but TETRA.
 *
 * @param mode: operating mode, from the opmode enum.
 * @param out: destination, always written.
 * @return true if the mode reports at least one of the two.
 */
bool modeReportedFrequency(uint8_t mode, struct modeReportedFrequency *out);

#ifdef __cplusplus
}
#endif

#endif /* MODE_REPORT_H */
