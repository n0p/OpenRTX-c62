/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file ui_mode_state.c
 * @brief The decoded state of each receive mode, for the main screen.
 *
 * These are the texts that used to be read-only rows of each mode's quick
 * menu - which is also that mode's Settings page, so the operator had to open
 * Settings to see whether the radio had locked, and the page was a list of
 * things that could not be actuated. The state belongs on the main screen
 * (docs/UI.md, "El estado del modo") and this file is where each protocol's
 * snapshot becomes a line of text.
 *
 * The rules the text follows, and the reason for each:
 *
 *  - nothing decoded reads as "SRCH" (or "T searching"), never as a number: a
 *    colour code of 0 is a fact and "not decoded yet" is not, and a screen that
 *    prints 0 for the second one lies to the operator;
 *  - an identifier that is not known is "-", never a zero for the same reason;
 *  - the text is short on purpose (UI_MODE_STATE_LEN including the
 *    terminator): the main screen measures it before drawing and shortens it
 *    rather than let it run off the 160 px panel, so what is here has to be the
 *    whole information, not a paragraph.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui_mode_state.h"
#include "ui/ui_modes.h"
#include "core/state.h"
#include "rtx/rtx.h"

#ifdef UI_MODE_DMR
#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrStatus.h"
#endif

#ifdef UI_MODE_NXDN
#include "protocols/NXDN/NxdnStatus.h"
#endif

#ifdef UI_MODE_P25
#include "protocols/P25/P25Status.h"
#endif

#ifdef UI_MODE_DSTAR
#include "protocols/DSTAR/DStarStatus.h"
#endif

#ifdef UI_MODE_TETRA
#include "protocols/TETRA/TetraStatus.h"
#endif

#ifdef UI_MODE_DPMR
#include "protocols/DPMR/DpmrStatus.h"
#endif

/* --------------------------------------------------------------- helpers -- */

static void setText(char *dst, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(dst, UI_MODE_STATE_LEN, fmt, ap);
    va_end(ap);
}

/**
 * What an identifier that the receiver has not decoded yet reads as.
 */
#define UNKNOWN "-"

/* ------------------------------------------------------------------- DMR -- */

#ifdef UI_MODE_DMR
static void dmrState(ui_mode_state_t *out)
{
    struct dmrStatus ds;

    dmr_getStatus(&ds);
    out->sync = ds.sync;

    if (!ds.sync) {
        setText(out->id, "DMR searching");
        return;
    }

    /* The colour code and timeslot the last burst carried */
    if (ds.colourCode >= 0)
        setText(out->id, "CC%u TS%c", (unsigned)ds.colourCode,
                (ds.timeslot > 0) ? (char)('0' + ds.timeslot) : '-');
    else
        setText(out->id, "CC" UNKNOWN " TS%c",
                (ds.timeslot > 0) ? (char)('0' + ds.timeslot) : '-');

    /* Who is calling whom, once a link control has been decoded */
    if (ds.idsValid) {
        out->call = true;
        if (ds.group)
            setText(out->callTxt, "%lu>%lu", (unsigned long)ds.src,
                    (unsigned long)ds.dst);
        else
            setText(out->callTxt, "%lu>%lu P", (unsigned long)ds.src,
                    (unsigned long)ds.dst);
    }

    /*
     * The link-quality figure: the share of the embedded link controls of the
     * last voice bursts that decoded, which is what this receiver can publish
     * (it has no per-bit EVM). "LC100%" is a clean slot.
     */
    if (ds.voice && (ds.evm > 0))
        setText(out->qual, "LC%u%%", (unsigned)(ds.evm / 10));

    if (!ds.voice && (ds.lastType >= 0))
        setText(out->note, "%s", dmr_data_type_name((uint8_t)ds.lastType));
}
#endif /* UI_MODE_DMR */

/* ------------------------------------------------------------------ NXDN -- */

#ifdef UI_MODE_NXDN
static const char *nxdnMessageName(uint8_t type)
{
    switch (type) {
        case 0x01: return "VCALL";
        case 0x08: return "TX_REL";
        case 0x11: return "DISC";
        default:   return NULL;
    }
}

static void nxdnState(ui_mode_state_t *out)
{
    struct nxdnStatus st;

    nxdn_getStatus(&st);
    out->sync = st.sync;

    if (!st.sync) {
        setText(out->id, "NXDN SRCH");
        return;
    }

    /* The RAN comes from the SACCH, so it trails the frame sync by a moment */
    if (st.ran >= 0)
        setText(out->id, "RAN%u%s", (unsigned)st.ran, st.voice ? " VOX" : "");
    else
        setText(out->id, "SYNC%s", st.voice ? " VOX" : "");

    /* The call the SACCH carried: who talks to which group */
    if (st.message.valid) {
        out->call = true;
        setText(out->callTxt, "%u>%u", (unsigned)st.message.source,
                (unsigned)st.message.dest);

        const char *name = nxdnMessageName(st.message.type);
        if (name != NULL)
            setText(out->note, "%s", name);
    }

    setText(out->qual, "e%u%% f%lu", (unsigned)(st.evm / 10),
            (unsigned long)st.frames);
}
#endif /* UI_MODE_NXDN */

/* ------------------------------------------------------------------ dPMR -- */

#ifdef UI_MODE_DPMR
static void dpmrState(ui_mode_state_t *out)
{
    struct dpmrStatus st;

    dpmr_getStatus(&st);
    out->sync = st.sync;

    if (!st.sync) {
        setText(out->id, "dPMR SRCH");
        return;
    }

    /*
     * The channel code of clause 6.1.5 is dPMR's colour code: it opens frames 2
     * and 4 of a superframe, so it trails the first header by a frame and is
     * unknown ("dPMR SYNC") until one of them closes its CCH CRC. "RX" is the
     * squelch -- the frame grid being locked right now -- and not the same
     * thing: the identifiers stay readable after the transmission ends, which
     * is when the END status of a call is worth reading.
     */
    const char *rx = st.receiving ? " RX" : "";

    if (st.channelCode >= 0)
        setText(out->id, "CC%03X%s", (unsigned)st.channelCode, rx);
    else
        setText(out->id, "dPMR%s%s", st.receiving ? "" : " SYNC", rx);

    /* Who calls whom: the two 24 bit addresses the header frame carried */
    if (st.idsValid) {
        out->call = true;
        setText(out->callTxt, "%06lX>%06lX", (unsigned long)st.source,
                (unsigned long)st.dest);
    }

    /* What the call is doing, and the status message of an END that carried one */
    const char *name = dpmrStatus_callName(st.callType);

    if (name != NULL) {
        if (st.endHasStatus && (st.callType == DPMR_CALLTYPE_TX_REL)) {
            setText(out->note, "%s S%u", name, (unsigned)st.endStatus);
            /* The type alone when the status does not fit beside the code */
            setText(out->noteShort, "%s", name);
        } else {
            setText(out->note, "%s", name);
        }
    }

    /* Quality: the demodulator's level error and the frames it has decoded */
    setText(out->qual, "e%u%% f%lu", (unsigned)(st.evm / 10),
            (unsigned long)st.frames);
}
#endif /* UI_MODE_DPMR */

/* ------------------------------------------------------------------- P25 -- */

#ifdef UI_MODE_P25
static void p25State(ui_mode_state_t *out)
{
    struct p25Status st;

    p25Status_get(&st);
    out->sync = st.sync;

    if (!st.sync) {
        setText(out->id, "P25 SRCH");
        return;
    }

    if (st.nac >= 0)
        setText(out->id, "NAC%03X%s", (unsigned)st.nac, st.voice ? " VOX" : " CC");
    else
        setText(out->id, "P25 SYNC%s", st.voice ? " VOX" : "");

    if ((st.source >= 0) || (st.talkgroup >= 0)) {
        out->call = true;
        if (st.source >= 0)
            setText(out->callTxt, "%ld>%d", (long)st.source, (int)st.talkgroup);
        else
            setText(out->callTxt, "G%u>%d", (unsigned)st.source,
                    (int)st.talkgroup);
    }

    /*
     * Quality as the IMBE FEC sees it: the share of voice frames that needed
     * no uncorrectable repair, plus how many frames have been decoded.
     */
    if (st.imbe > 0)
        setText(out->qual, "ok%u%% f%lu",
                (unsigned)(100U - (st.imbeBad * 100U) / st.imbe),
                (unsigned long)st.frames);

    if (st.duid >= 0)
        setText(out->note, "DUID%X", (unsigned)st.duid);
}
#endif /* UI_MODE_P25 */

/* ------------------------------------------------------------------- YSF -- */

#ifdef UI_MODE_YSF
static void ysfState(ui_mode_state_t *out)
{
    /* System Fusion publishes only "a carrier is being tracked" (the squelch) */
    out->sync = rtx_rxSquelchOpen();
    setText(out->id, out->sync ? "YSF RX" : "SRCH");
}
#endif /* UI_MODE_YSF */

/* ----------------------------------------------------------------- DSTAR -- */

#ifdef UI_MODE_DSTAR
static void dstarState(ui_mode_state_t *out)
{
    struct dstarStatus st;

    dstar_getStatus(&st);
    out->sync = st.sync;

    if (!st.sync) {
        setText(out->id, "DSTAR SRCH");
        return;
    }

    /* Who is talking to whom: the header of the last frame */
    if ((st.my[0] != '\0') && (st.ur[0] != '\0')) {
        if (st.sfx[0] != '\0')
            setText(out->id, "%s/%s>%s", st.my, st.sfx, st.ur);
        else
            setText(out->id, "%s>%s", st.my, st.ur);
    } else {
        setText(out->id, "DV SYNC");
    }

    /*
     * The quality figure of this mode is the Viterbi's work: how many channel
     * bits the last header needed corrected. The access repeater goes with it
     * because it is what the operator writes down.
     */
    if (st.rpt1[0] != '\0')
        setText(out->qual, "b%d %s", (int)st.viterbi, st.rpt1);
    else
        setText(out->qual, "b%d", (int)st.viterbi);

    if (st.text[0] != '\0')
        setText(out->note, "%s", st.text);
}
#endif /* UI_MODE_DSTAR */

/* ----------------------------------------------------------------- TETRA -- */

#ifdef UI_MODE_TETRA
static void tetraState(ui_mode_state_t *out)
{
    struct tetraStatus ts;

    tetra_getStatus(&ts);
    out->sync = ts.sync;

    if (!ts.sync) {
        setText(out->id, "T searching");
        return;
    }

    if (ts.cell) {
        /*
         * Cell and location area, without the "T " of the mode label: the top
         * bar already says TETRA, and the room the prefix takes is the room the
         * registration note needs on the other half of the line.
         */
        setText(out->id, "%u-%u", (unsigned)ts.mcc, (unsigned)ts.mnc);
        if (ts.sysinfo)
            setText(out->note, "LA%u", (unsigned)ts.la);
        else
            setText(out->note, "CC%u", (unsigned)ts.colourCode);
    } else {
        setText(out->id, "T SYNC");
    }

    /*
     * Registration: what the cell's mobility management says (ACC with its
     * group count, REJ with the cause). This radio is a monitor, so it reports
     * what the network tells the mobiles it can hear. Nothing is drawn before
     * the network says anything, instead of a zero-loaded "ACC G0".
     *
     * The rest of the network broadcast (neighbour count, cell load, network
     * time) does not fit beside the cell on a 160 px line and is not part of
     * the state the operator works with; it stays a diagnostic.
     */
    char reg[12] = "";

    if (ts.registered)
        snprintf(reg, sizeof(reg), "ACC G%u", (unsigned)ts.groups);
    else if (ts.mmCause != 0)
        snprintf(reg, sizeof(reg), "REJ %u", (unsigned)ts.mmCause);

    if (reg[0] != '\0') {
        char la[12] = "";

        if (out->note[0] != '\0')
            snprintf(la, sizeof(la), "%s", out->note);

        if (la[0] != '\0') {
            /* Both when they fit, the registration alone when they do not */
            setText(out->note, "%s %s", la, reg);
            setText(out->noteShort, "%s", reg);
        } else {
            setText(out->note, "%s", reg);
        }
    }

    /*
     * The demodulator figures and the uplink activity stay on line 1, which
     * _ui_drawTetraLine1() owns for this mode (it also pages the SDS texts):
     * two writers on one line would draw over each other.
     */
}
#endif /* UI_MODE_TETRA */

/* -------------------------------------------------------------- dispatch -- */

void ui_mode_state_read(ui_mode_state_t *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));

    switch (state.channel.mode) {
#ifdef UI_MODE_DMR
        case OPMODE_DMR:
            dmrState(out);
            break;
#endif
#ifdef UI_MODE_NXDN
        case OPMODE_NXDN:
            nxdnState(out);
            break;
#endif
#ifdef UI_MODE_DPMR
        case OPMODE_DPMR:
            dpmrState(out);
            break;
#endif
#ifdef UI_MODE_P25
        case OPMODE_P25:
            p25State(out);
            break;
#endif
#ifdef UI_MODE_YSF
        case OPMODE_YSF:
            ysfState(out);
            break;
#endif
#ifdef UI_MODE_DSTAR
        case OPMODE_DSTAR:
            dstarState(out);
            break;
#endif
#ifdef UI_MODE_TETRA
        case OPMODE_TETRA:
            tetraState(out);
            break;
#endif
        default:
            /*
             * FM, M17, airband and broadcast FM draw their own lines: FM shows
             * its bandwidth and tone, M17 the link set-up frame (source,
             * destination, reflector, meta text) and the two receivers their
             * tuner state. There is no decoded state in this block for them.
             */
            break;
    }
}
