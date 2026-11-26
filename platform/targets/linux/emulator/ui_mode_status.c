/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file ui_mode_status.c
 * @brief The services the host has no hardware for, as the UI sees them.
 *
 * Three things the UI reads are owned by a translation unit that needs the
 * radio or the kernel, and the host emulator has neither:
 *
 *  - DMR and TETRA keep what their receiver decoded in DmrStatus.c and
 *    TetraStatus.c, and those two include the demodulator's own headers;
 *  - the packet modem page (ui_packet.c) reads core/packet_service.c, which
 *    owns an audio stream, its own thread and a PSRAM queue.
 *
 * The *screens* of all three modes/pages still have to be built and walked by
 * the UI tests (docs/UI.md lists them), so this file answers their getters with
 * "nothing decoded, nothing on the air": sync false, colour code and timeslot
 * unknown (-1), no cell, no call, no frames. That is also the state the
 * operator sees on a radio that has not locked yet, which is what the UI is
 * supposed to render as "-", "CC-" and "SRCH" instead of a zero that reads like
 * an identifier - tests/unit/ui_menu_audit.cpp checks exactly that, so this
 * stub is the fixture of that rule and not a placeholder that hides a defect.
 *
 * Only the linux target compiles this file; the firmware uses the real
 * translation units.
 */

#include <string.h>

#include "core/packet_service.h"
#include "protocols/KISS/kiss.h"
#include "core/settings.h"
#include "core/state.h"
#include "protocols/DMR/DmrStatus.h"
#include "protocols/TETRA/TetraStatus.h"
#include "emulator.h"

/* ------------------------------------------------------------------- DMR -- */

/*
 * The host decodes nothing, so dmr_getStatus() answers "nothing decoded" until
 * a script fills this in. The simulator's `inject dmr` command and the audit
 * test do that, which is the only way to check the main screen's state block
 * (colour code, timeslot, call, link quality) without a radio.
 */
static struct dmrStatus simDmr;

void dmr_sim_clear(void)
{
    memset(&simDmr, 0, sizeof(simDmr));
    simDmr.colourCode = -1;
    simDmr.timeslot   = 0;
    simDmr.lastType   = -1;
}

void dmr_sim_setStatus(bool sync, int cc, int ts, bool voice, unsigned evm,
                       bool idsValid, bool group, uint32_t src, uint32_t dst,
                       int lastType)
{
    dmr_sim_clear();
    simDmr.sync       = sync;
    simDmr.voice      = voice;
    simDmr.colourCode = (cc >= 0) ? (int8_t)cc : -1;
    simDmr.timeslot   = (int8_t)ts;
    simDmr.evm        = (uint16_t)evm;
    simDmr.idsValid   = idsValid;
    simDmr.group      = group;
    simDmr.src        = src;
    simDmr.dst        = dst;
    simDmr.lastType   = (int8_t)lastType;
}

void dmr_sim_setLan(bool sync, int cc, int ts)
{
    simDmr.sync       = sync;
    simDmr.colourCode = (cc >= 0) ? (int8_t)cc : -1;
    simDmr.timeslot   = (int8_t)ts;
}

void dmr_getStatus(struct dmrStatus *out)
{
    if (out == NULL)
        return;

    *out = simDmr;
}

void dmrStatus_reset(void)
{
}

void dmrStatus_clear(void)
{
    dmr_sim_clear();
}

void dmrStatus_update(void)
{
}

/* ----------------------------------------------------------------- TETRA -- */

/*
 * The host decodes nothing, but the UI reads what a receiver would hold, and
 * the "Cell uplink" row of the TETRA quick menu has to be walked both with a
 * cell whose uplink is known and with none.  Nothing is injected by default:
 * this answers "nothing decoded", which is what the screens must render as
 * "-" and "T searching".  The simulator's `inject tetra-cell` command
 * (sim_control.c) and the tests fill it through tetra_sim_setCell().
 */
static struct tetraStatus simTetra;

void tetra_sim_clearCell(void)
{
    memset(&simTetra, 0, sizeof(simTetra));
}

void tetra_sim_setCell(uint16_t mcc, uint16_t mnc, uint8_t colourCode,
                       uint32_t dlHz, uint32_t ulHz)
{
    memset(&simTetra, 0, sizeof(simTetra));
    simTetra.sync = true;
    simTetra.cell = true;
    simTetra.mcc = mcc;
    simTetra.mnc = mnc;
    simTetra.colourCode = colourCode;
    simTetra.sysinfo = (dlHz != 0);
    simTetra.la = 1;
    simTetra.dlFreq = dlHz;
    simTetra.ulFreq = ulHz;
    /* band and duplex spacing are left 0: the injected uplink is explicit */
}

/*
 * Mobility management and the network broadcast, which the main screen's state
 * block draws on line 4 (registration and the neighbour list). A real receiver
 * fills these from D-LOCATION-UPDATE-ACCEPT and D-NWRK-BROADCAST; the host has
 * no receiver, so a script says what it heard.
 */
void tetra_sim_setRegistration(bool accepted, uint8_t groups, uint8_t mmCause)
{
    simTetra.registered = accepted;
    simTetra.groups = groups;
    simTetra.mmCause = mmCause;
}

void tetra_sim_setCall(bool call, bool group, uint32_t dst, uint32_t talker,
                       uint16_t callId, bool voice, bool encrypted)
{
    simTetra.call = call;
    simTetra.callGroup = group;
    simTetra.callDst = dst;
    simTetra.talker = talker;
    simTetra.callId = callId;
    simTetra.voice = voice;
    simTetra.callEncrypted = encrypted;
}

void tetra_sim_setUplink(bool uplink, uint32_t ssi)
{
    simTetra.uplink = uplink;
    simTetra.uplinkSs = ssi;
}

void tetra_getStatus(struct tetraStatus *out)
{
    if (out == NULL)
        return;

    *out = simTetra;
}

void tetraStatus_reset(void)
{
    tetra_sim_clearCell();
}

/* ---------------------------------------------------------------- packet -- */

static struct
{
    bool    enabled;
    uint8_t modem;
    uint8_t params[16];
    uint32_t sent;
} pkt;

void packet_service_init(void)
{
    memset(&pkt, 0, sizeof(pkt));
}

void packet_service_bind(const kiss_port_t *port)
{
    (void)port;
}

bool packet_service_enabled(void)
{
    return pkt.enabled;
}

void packet_service_setEnabled(bool on)
{
    /* The page reads settings_t, so the double has to write it: the real
       service (core/packet_service.c) does the same, and a stub that only
       keeps its own copy makes the page look broken on the host. */
    pkt.enabled = on;
#ifdef CONFIG_KISS
    state.settings.packet_enable = on;
#endif
}

bool packet_service_setModem(uint8_t modem)
{
    /* The real service bounds-checks against the modem registry; the host has
       no registry, and the page only walks 0..PKT_MODEM_COUNT-1 */
    if (modem >= PKT_MODEM_COUNT)
        return false;

    pkt.modem = modem;
#ifdef CONFIG_KISS
    state.settings.packet_modem = modem;
#endif
    return true;
}

void packet_service_setParam(uint8_t cmd, uint8_t value)
{
    if (cmd < sizeof(pkt.params))
        pkt.params[cmd] = value;

#ifdef CONFIG_KISS
    switch (cmd)
    {
        case KISS_CMD_TXDELAY:
            state.settings.packet_txdelay_ms = (uint16_t)value * 10u;
            break;
        case KISS_CMD_PERSIST:
            state.settings.packet_persist = value;
            break;
        case KISS_CMD_SLOTTIME:
            state.settings.packet_slottime_ms = (uint16_t)value * 10u;
            break;
        case KISS_CMD_TXTAIL:
            state.settings.packet_txtail_ms = (uint16_t)value * 10u;
            break;
        case KISS_CMD_FULLDUP:
            state.settings.packet_fulldup = (value != 0) ? 1 : 0;
            break;
        default:
            break;
    }
#endif
}

void packet_service_status(packet_status_t *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));
    out->enabled = pkt.enabled;
    out->txFrames = pkt.sent;
}

void packet_service_clearCounters(void)
{
    pkt.sent = 0;
}

bool packet_service_sendNow(const uint8_t *payload, size_t len)
{
    if ((payload == NULL) || (len == 0))
        return false;

    pkt.sent++;
    return true;
}

void packet_service_nxdnPayload(const uint8_t *l3, size_t len)
{
    (void)l3;
    (void)len;
}

void packet_service_dmrPayload(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}
