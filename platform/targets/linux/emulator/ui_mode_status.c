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
#include "core/settings.h"
#include "protocols/DMR/DmrStatus.h"
#include "protocols/TETRA/TetraStatus.h"

/* ------------------------------------------------------------------- DMR -- */

void dmr_getStatus(struct dmrStatus *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));
    out->colourCode = -1;
    out->timeslot   = 0;     /* 0 is the struct's own "unknown" */
    out->lastType   = -1;
}

void dmrStatus_reset(void)
{
}

void dmrStatus_clear(void)
{
}

void dmrStatus_update(void)
{
}

/* ----------------------------------------------------------------- TETRA -- */

void tetra_getStatus(struct tetraStatus *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));
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
    pkt.enabled = on;
}

bool packet_service_setModem(uint8_t modem)
{
    /* The real service bounds-checks against the modem registry; the host has
       no registry, and the page only walks 0..PKT_MODEM_COUNT-1 */
    pkt.modem = modem;
    return true;
}

void packet_service_setParam(uint8_t cmd, uint8_t value)
{
    if (cmd < sizeof(pkt.params))
        pkt.params[cmd] = value;
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
