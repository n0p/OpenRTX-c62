/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/ModeReport.h"

#include <string.h>

#include "rtx/rtx.h"

#ifdef CONFIG_DMR
#include "protocols/DMR/DmrStatus.h"
#endif
#ifdef CONFIG_NXDN
#include "protocols/NXDN/NxdnStatus.h"
#endif
#ifdef CONFIG_P25
#include "protocols/P25/P25Status.h"
#endif
#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraStatus.h"
#endif

/*
 * One case per mode, and nothing else: the per-mode status modules already
 * own the counters (they are written by the rtx thread and read under their
 * own lock), so this file only translates them into the two answers the
 * carrier search needs.  Adding a mode with a frequency in its air interface
 * is a case here plus the matching field in its status struct.
 */

bool modeSyncCounters(uint8_t mode, struct modeCarrierReport *out)
{
    if (out == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    switch (mode) {
#ifdef CONFIG_DMR
        case OPMODE_DMR: {
            struct dmrStatus st;
            dmr_getStatus(&st);
            /* A burst is only accounted once its sync and slot type decoded */
            out->synced     = st.sync;
            out->haveFrames = true;
            out->validFrames = st.bursts;
            return true;
        }
#endif

#ifdef CONFIG_NXDN
        case OPMODE_NXDN: {
            struct nxdnStatus st;
            nxdn_getStatus(&st);
            /*
             * `frames` also counts the frames whose LICH parity failed, and a
             * failed LICH is exactly what a false sync looks like: subtract
             * them, so the count is "frames with a valid LICH".
             */
            out->synced      = st.sync;
            out->haveFrames  = true;
            out->validFrames = (st.frames > st.lichErrors)
                             ? (st.frames - st.lichErrors) : 0;
            return true;
        }
#endif

#ifdef CONFIG_P25
        case OPMODE_P25: {
            struct p25Status st;
            p25Status_get(&st);
            out->synced      = st.sync;
            out->haveFrames  = true;
            out->validFrames = st.frames;
            return true;
        }
#endif

#ifdef CONFIG_TETRA
        case OPMODE_TETRA: {
            struct tetraStatus st;
            struct tetraDiag   diag;
            tetra_getStatus(&st);
            tetra_getDiag(&diag);
            /* A TETRA block only counts once its CRC-16 passed */
            out->synced      = st.sync;
            out->haveFrames  = true;
            out->validFrames = diag.crcOk;
            return true;
        }
#endif

        case OPMODE_M17:
        case OPMODE_YSF:
        default:
            /*
             * M17 and YSF report synchronism through their own
             * rxSquelchOpen(), which the caller already samples, and neither
             * publishes a frame counter: they get the time criterion.
             */
            return false;
    }
}

bool modeReportedFrequency(uint8_t mode, struct modeReportedFrequency *out)
{
    if (out == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    switch (mode) {
#ifdef CONFIG_TETRA
        case OPMODE_TETRA: {
            struct tetraStatus st;
            tetra_getStatus(&st);

            /*
             * SYSINFO (SCH/F, SCH-HD) carries the downlink main carrier of the
             * serving cell, in Hz: that is the frequency the network itself
             * says it is on, so it is the correction when it is published.
             */
            if ((st.cell != 0) && (st.sysinfo != 0) && (st.dlFreq != 0)) {
                out->absolute  = true;
                out->frequency = st.dlFreq;
            }

            /*
             * And the demodulator measures how far off the tuning still is.
             * Its reading is a fraction of the real error -- the receive path
             * removes most of the carrier's DC -- and it is the same figure
             * the mode's own AFC steers on, so it is used as given and the
             * AFC keeps converging afterwards.
             */
            if ((st.phaseErr < 35) && (st.carrierHz != 0)) {
                out->offsetValid = true;
                out->offsetHz    = st.carrierHz;
            }

            return out->absolute || out->offsetValid;
        }
#endif

        default:
            /*
             * DMR Tier II/III, NXDN, P25, YSF, M17, dPMR, D-STAR and the
             * analog modes carry no absolute frequency in their air interface
             * (see the table in ModeReport.h): no correction, the tuning stays
             * where the operator put it.
             */
            return false;
    }
}
