/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DMR Tier III on the C62 (dmr_trunk.h): the RadioFoundry engine (t3_ms.c,
 * MIT) set up from Ajustes > DMR and fed by dmr_proto. As RadioFoundry's
 * radio core does it (docs/spec/dmr-trunking.md section 11): every CSBK,
 * MBC and UDT block heard goes to t3_ms_rx(), every burst on a slot is
 * activity, each CACH says which inbound slot it announces; the engine's
 * TUNE actions say where to listen, its SEND actions what to transmit.
 * Listening (C62_T3_LISTEN) the engine is set listen only and sends
 * nothing; registered (C62_T3_REG) OpMode_DMR_RX sends its bursts in the
 * slot they name, aligned to the system's grid as a repeater call.
 */

#include <string.h>
#include "interfaces/delays.h"
#include "dmr_port.h"
/*
 * Ours, not the tree this file comes from: the settings records it reads
 * (c62_dmr, c62_t3) live in dmr_glue.h here, backed by the channel, the
 * settings record and the quick menu.  The only edit to the package.
 */
#include "dmr_glue.h"
#include "t3_ms.h"
#include "dmr_trunk.h"

uint16_t t3_crc_ccitt(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(buf[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

/* The engine and its set-up, about 2 KB: in PSRAM, as dmr_proto's tables.
 * PSRAM is not cleared at start: t3_ms_start() clears the engine, and the
 * set-up is written whole before it */
static EXT_RAM_BSS_ATTR t3_ms_t s_ms;
static EXT_RAM_BSS_ATTR t3_cfg_t s_cfg;
static bool s_running;
static uint32_t s_ctrlHz;
static uint32_t s_tunedHz;
static uint8_t s_slot;

/* Bursts the engine asked to send, oldest first: a CSBK, or a UDT header
 * and its blocks, which go in consecutive frames of one slot */
#define SEND_MAX 8
typedef struct {
    uint8_t slot;
    bool payload;
    uint8_t dt;
    uint8_t b[12];
} sendItem_t;
static sendItem_t s_send[SEND_MAX];
static uint8_t s_nsend;
/* The PTT asked for a call that is not granted yet */
static bool s_pttWait, s_pttWas;

#define DT_CSBK 3u
#define DT_DATA_HEADER 6u
#define DT_RATE_12 7u

static uint32_t nowMs(void)
{
    return (uint32_t)getTick();
}

static void addCtrl(uint32_t hz, int32_t txOff)
{
    if (hz == 0 || s_cfg.nctrl >= T3_CTRL_MAX)
        return;
    for (uint8_t i = 0; i < s_cfg.nctrl; i++)
        if (s_cfg.ctrl[i].rx_hz == hz)
            return;
    t3_chan_t *c = &s_cfg.ctrl[s_cfg.nctrl++];
    c->lcn = 0;
    c->cc = T3_CC_ANY;
    c->rx_hz = hz;
    c->tx_hz = (uint32_t)((int32_t)hz + txOff);
}

void dmrTrunk_start(uint32_t ctrlHz, uint32_t ctrlTxHz)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.own_id = c62_dmr.id;
    /* Any system: the first one heard is taken as ours (model and network),
     * whatever control category its PAR serves */
    s_cfg.auth.category = T3_PAR_AB;
    /* Never: stun, kill, Ambient Listening (the radio would transmit with
     * no one at the PTT), roaming between sites */
    s_cfg.opts = T3_OPT_LEARN_SYSCODE | T3_OPT_ALLCALL
               | T3_OPT_EMERGENCY_ANY | T3_OPT_PRIVATE | T3_OPT_NO_ROAM;
    if (c62_t3.mode != C62_T3_REG || c62_dmr.id == 0)
        s_cfg.opts |= T3_OPT_LISTEN_ONLY;
    if (c62_t3.followAll)
        s_cfg.opts |= T3_OPT_MONITOR;
    if (c62_dmr.tg != 0 && !c62_dmr.privateCall) {
        s_cfg.tg[0].id = c62_dmr.tg;
        s_cfg.tg[0].flags = T3_TG_LISTEN | T3_TG_TALK;
        s_cfg.ntg = 1;
    }
    /* Channel plans: what the system broadcasts, then the fixed formula */
    s_cfg.plan.learn = true;
    s_cfg.plan.fixed.base_hz = c62_t3.baseHz;
    s_cfg.plan.fixed.first = c62_t3.first ? c62_t3.first : 1u;
    s_cfg.plan.fixed.sep = (uint8_t)(c62_t3.sep & 7u);
    s_cfg.plan.fixed.tx_low = c62_t3.txLow != 0;
    s_cfg.plan.fixed.split_hz = c62_t3.splitHz;
    /* The channel's frequency first, then the others set, with its duplex
     * offset */
    const int32_t txOff = (int32_t)(ctrlTxHz - ctrlHz);
    addCtrl(ctrlHz, txOff);
    for (unsigned i = 0; i < C62_T3_CTRL_MORE; i++)
        addCtrl(c62_t3.ctrlHz[i], txOff);
    s_nsend = 0;
    s_pttWait = false;

    s_ctrlHz = ctrlHz;
    s_tunedHz = ctrlHz;
    s_slot = 0;
    t3_ms_start(&s_ms, &s_cfg, nowMs());
    s_running = true;
}

void dmrTrunk_stop(void)
{
    if (s_running)
        t3_ms_stop(&s_ms);
    s_running = false;
    s_nsend = 0;
    s_pttWait = false;
}

bool dmrTrunk_running(void)
{
    return s_running;
}

void dmrTrunk_block(int slotIdx, int dt, int cc, const uint8_t bytes[12])
{
    if (!s_running)
        return;
    t3_ms_rx(&s_ms, nowMs(), (uint8_t)(slotIdx + 1), (uint8_t)dt,
             (uint8_t)cc, bytes);
    t3_ms_activity(&s_ms, nowMs(), (uint8_t)(slotIdx + 1));
}

void dmrTrunk_voice(int slotIdx, int cc)
{
    if (!s_running)
        return;
    const uint32_t now = nowMs();
    t3_ms_activity(&s_ms, now, (uint8_t)(slotIdx + 1));
    if (cc >= 0)
        t3_ms_call_burst(&s_ms, now, (uint8_t)(slotIdx + 1), (uint8_t)cc);
}

void dmrTrunk_activity(int slotIdx)
{
    if (s_running)
        t3_ms_activity(&s_ms, nowMs(), (uint8_t)(slotIdx + 1));
}

void dmrTrunk_cach(int slotIdx, bool atBusy)
{
    if (s_running)
        t3_ms_slot(&s_ms, nowMs(), (uint8_t)(slotIdx + 1), true, atBusy);
}

void dmrTrunk_level(int dbm)
{
    if (s_running && dbm != 0)
        t3_ms_level(&s_ms, nowMs(), (int16_t)dbm);
}

static void pushSend(uint8_t slot, bool payload, uint8_t dt,
                     const uint8_t *b)
{
    if (s_nsend >= SEND_MAX)
        return;
    sendItem_t *e = &s_send[s_nsend++];
    e->slot = slot;
    e->payload = payload;
    e->dt = dt;
    memcpy(e->b, b, 12);
}

/* The engine's actions: where to listen, what to send */
static void drain(void)
{
    t3_act_t act;
    while (t3_ms_poll(&s_ms, nowMs(), &act)) {
        if (act.type == T3_ACT_TUNE && act.chan.rx_hz != 0) {
            s_tunedHz = act.chan.rx_hz;
            s_slot = act.slot;
        } else if (act.type == T3_ACT_SEND && c62_t3.mode == C62_T3_REG) {
            /* The blocks are the engine's until its next call: copied */
            pushSend(act.slot, act.payload,
                     act.udt ? DT_DATA_HEADER : DT_CSBK, act.burst);
            for (uint8_t i = 0; act.udt && act.blocks != NULL
                                && i < act.nblocks; i++)
                pushSend(act.slot, act.payload, DT_RATE_12, act.blocks[i]);
        }
    }
}

bool dmrTrunk_poll(uint32_t *hz, uint8_t *slot)
{
    if (!s_running)
        return false;
    drain();
    *hz = s_tunedHz;
    *slot = s_slot;
    return true;
}

void dmrTrunk_getStatus(dmrTrunkStatus_t *out)
{
    memset(out, 0, sizeof(*out));
    out->running = s_running;
    if (!s_running)
        return;
    out->state = (uint8_t)s_ms.state;
    out->reg = (uint8_t)s_ms.reg;
    out->confirmed = s_ms.confirmed;
    out->tsccHz = s_ms.tscc.rx_hz;
    out->tsccCc = s_ms.tscc.cc;
    out->syscode = s_ms.syscode;
    out->tunedHz = s_tunedHz;
    out->slot = s_slot;
    out->inCall = s_ms.state == T3_FOLLOW;
    out->dst = s_ms.call.dst;
    out->src = s_ms.call.src;
    out->group = s_ms.call.group;
    out->emergency = s_ms.call.emergency;
    out->lcn = s_ms.call.chan.lcn;
    out->callHz = s_ms.call.chan.rx_hz;
    out->csbk = s_ms.st.csbk;
    out->grants = s_ms.st.grants;
    out->followed = s_ms.st.followed;
    out->unmapped = s_ms.st.unmapped;
    out->hunts = s_ms.st.hunts;
    out->lastUnmapped = s_ms.st.last_unmapped;
}

/* The colour code of a channel: its own, else the control channel's */
static uint8_t chanCc(const t3_chan_t *c)
{
    if (c->cc != T3_CC_ANY)
        return c->cc;
    if (s_ms.tscc.cc != T3_CC_ANY)
        return s_ms.tscc.cc;
    return c62_dmr.cc;
}

bool dmrTrunk_sendPending(uint32_t *txHz, uint8_t *slot, uint8_t *cc)
{
    if (!s_running || s_nsend == 0)
        return false;
    const t3_chan_t *c = s_send[0].payload ? &s_ms.call.chan : &s_ms.tscc;
    if (c->tx_hz == 0) {
        s_nsend = 0;
        t3_ms_sent(&s_ms, nowMs(), false);
        return false;
    }
    *txHz = c->tx_hz;
    *slot = s_send[0].slot;
    *cc = chanCc(c);
    return true;
}

bool dmrTrunk_sendNext(uint8_t slot, uint8_t *dt, uint8_t bytes[12])
{
    if (!s_running)
        return false;
    /* This frame's inbound slot: what the engine sends in it (P_MAINT's
     * repeats). No CACH is heard while transmitting: the AT bit unknown */
    t3_ms_slot(&s_ms, nowMs(), slot, false, false);
    drain();
    for (uint8_t i = 0; i < s_nsend; i++) {
        if (s_send[i].slot != slot)
            continue;
        *dt = s_send[i].dt;
        memcpy(bytes, s_send[i].b, 12);
        memmove(&s_send[i], &s_send[i + 1],
                (size_t)(s_nsend - i - 1) * sizeof(s_send[0]));
        s_nsend--;
        return true;
    }
    return false;
}

void dmrTrunk_sendDone(bool ok)
{
    if (!s_running)
        return;
    if (!ok)
        s_nsend = 0;
    t3_ms_sent(&s_ms, nowMs(), ok);
}

void dmrTrunk_ptt(bool down)
{
    if (!s_running || c62_t3.mode != C62_T3_REG) {
        s_pttWas = down;
        s_pttWait = false;
        return;
    }
    const uint32_t now = nowMs();
    if (down && !s_pttWas) {
        /* Camped: ask for a call (talking on a call followed needs none) */
        if (!t3_ms_can_talk(&s_ms) && s_ms.state == T3_CAMPED
            && c62_dmr.tg != 0)
            s_pttWait = t3_ms_request(&s_ms, now, c62_dmr.tg,
                                      !c62_dmr.privateCall, false);
    } else if (!down && s_pttWas) {
        /* Released before the grant: withdrawn (RadioFoundry's t3_ptt_up) */
        if (s_pttWait && s_ms.ax.state != T3_AX_IDLE)
            t3_ms_cancel(&s_ms, now);
        s_pttWait = false;
    }
    if (s_pttWait && t3_ms_can_talk(&s_ms))
        s_pttWait = false;
    s_pttWas = down;
}

bool dmrTrunk_talk(uint32_t *txHz, uint8_t *slot, uint8_t *cc,
                   uint32_t *dst, bool *group)
{
    if (!s_running || c62_t3.mode != C62_T3_REG || !t3_ms_can_talk(&s_ms))
        return false;
    const t3_call_t *c = &s_ms.call;
    if (c->chan.tx_hz == 0 || c->slot < 1 || c->slot > 2)
        return false;
    *txHz = c->chan.tx_hz;
    *slot = c->slot;
    *cc = chanCc(&c->chan);
    *group = c->group;
    /* A group call to its group; a private one to the other party */
    *dst = (c->group || c->ours) ? c->dst : c->src;
    return true;
}

const char *dmrTrunk_accessText(void)
{
    if (!s_running || c62_t3.mode != C62_T3_REG)
        return NULL;
    if (s_ms.reg == T3_REG_NEEDED)
        return "T3 registrando";
    if (s_ms.reg == T3_REG_DENIED)
        return "T3 registro denegado";
    switch (s_ms.ax.result) {
        case T3_RES_PENDING:     return "T3 pidiendo canal";
        case T3_RES_REFUSED:     return "T3 llamada rechazada";
        case T3_RES_NO_ANSWER:   return "T3 sin respuesta";
        case T3_RES_TIMEOUT:     return "T3 tiempo agotado";
        case T3_RES_NOT_ALLOWED: return "T3 no permitido";
        case T3_RES_LOST:        return "T3 control perdido";
        default:                 return NULL;
    }
}
