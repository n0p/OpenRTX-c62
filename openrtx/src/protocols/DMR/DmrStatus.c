/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrStatus.c
 * @brief Translation of the package's decoder snapshot into the struct the
 *        UI reads (DmrStatus.h).  Ours: the package's own snapshots
 *        (dmr_proto_get_slot/_get_stats, dmr_demod_get_status) stay as they
 *        are.
 */

#include <string.h>
#include "protocols/DMR/DmrStatus.h"
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_voice.h"

/** A burst this recent means "sync" */
#define SYNC_WINDOW_MS 300
/** A voice burst this recent means "voice" */
#define VOICE_WINDOW_MS 1000

static struct dmrStatus s_status;

/*
 * Link quality, per mille, from the embedded link controls of the voice
 * bursts: the package does not compute an EVM (its slicer carries no
 * per-bit decisions to compare), but the EMB QR word either decodes or it
 * does not, and their ratio is what the screen prints as "e%".
 */
static uint16_t linkQuality(const dmr_proto_stats_t *st)
{
    const uint32_t n = st->emb_lc_ok + st->emb_bad;

    if (n == 0)
        return 0;
    return (uint16_t)((st->emb_lc_ok * 1000u) / n);
}

void dmrStatus_clear(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.colourCode = -1;
    s_status.lastType = -1;
}

void dmrStatus_reset(void)
{
    dmrStatus_clear();
}

void dmrStatus_update(void)
{
    dmr_slot_info_t s0, s1;
    dmr_proto_stats_t st;
    dmr_demod_status_t ds;
    int bsSlot = 0;

    memset(&s0, 0, sizeof(s0));
    memset(&s1, 0, sizeof(s1));
    dmr_proto_get_slot(0, &s0);
    dmr_proto_get_slot(1, &s1);
    dmr_proto_get_stats(&st);
    dmr_demod_get_status(&ds);

    const dmr_slot_info_t *last = (s1.last_ms > s0.last_ms) ? &s1 : &s0;
    const uint32_t now = ds.now_ms;

    s_status.sync = (last->activity != DMR_ACT_NONE)
                 && ((uint32_t)(now - last->last_ms) < SYNC_WINDOW_MS);
    s_status.voice = (last->activity == DMR_ACT_VOICE)
                  && ((uint32_t)(now - last->last_ms) < VOICE_WINDOW_MS);
    s_status.colourCode = (int8_t)((last->cc >= 0) ? last->cc : -1);

    if (dmr_proto_bs_slot(&bsSlot))
        s_status.timeslot = (int8_t)(bsSlot + 1);
    else if (dmr_proto_is_ms_mode())
        s_status.timeslot = 0;

    if (s_status.voice)
        s_status.lastType = -1;
    else if (last->activity == DMR_ACT_CSBK)
        s_status.lastType = 3; /* DMR_DT_CSBK, DmrDefs.h */
    else if (last->activity == DMR_ACT_DATA)
        s_status.lastType = 7; /* DMR_DT_RATE_12 */
    else if (last->activity == DMR_ACT_IDLE)
        s_status.lastType = 9; /* DMR_DT_IDLE */

    s_status.evm = linkQuality(&st);

    /*
     * The call's source and destination, when the last link control on that
     * slot announced them: the main screen shows who is calling whom for as
     * long as the call lasts (the slot keeps ids_valid until the next call).
     */
    s_status.idsValid = last->ids_valid;
    s_status.group = last->group;
    if (last->ids_valid) {
        s_status.src = last->src;
        s_status.dst = last->dst;
    }

    uint32_t fecBits, fecFrames;
    dmr_voice_get_fec(&fecBits, &fecFrames);
    (void)fecBits;
    s_status.bursts = st.frames;
    s_status.voiceBursts = st.voice_bf;
    s_status.frames = fecFrames;
    s_status.crcChecks = st.data_packets + st.data_crc_bad;
    s_status.crcOk = st.data_packets;
}

void dmr_getStatus(struct dmrStatus *out)
{
    if (out == NULL)
        return;
    /*
     * One struct copy: the fields are written by the rtx thread and read by
     * the UI.  The copy may be one burst behind, never wrong on anything the
     * screen shows (a colour code one burst old is the same colour code).
     */
    *out = s_status;
}
