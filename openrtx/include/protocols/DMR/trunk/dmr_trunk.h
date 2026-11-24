/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TRUNK_H
#define DMR_TRUNK_H

/*
 * DMR Tier III (ETSI TS 102 361-4) on the C62: RadioFoundry's mobile station
 * engine (t3_ms.c) fed by our software DMR decoder. The radio finds the
 * system's control channel (TSCC) from the channel's frequency, stays on it
 * and follows the calls its grants announce, to the frequency and slot each
 * names. Listening (C62_T3_LISTEN) it never transmits; registered
 * (C62_T3_REG) it registers, answers the system and asks for calls.
 *
 * Everything runs on the rtx thread (OpMode_DMR_RX and dmr_proto, which it
 * calls); the UI only reads dmrTrunk_getStatus().
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the screen shows */
typedef struct {
    bool running;
    uint8_t state;        /* t3_state_t: 0 off, 1 hunt, 2 camped, 3 follow */
    uint8_t reg;          /* t3_reg_t */
    bool confirmed;       /* a TSCC of the system confirmed */
    uint32_t tsccHz;      /* its frequency (the MS receive one) */
    uint8_t tsccCc;
    uint16_t syscode;     /* C_SYScode of its Alohas */
    uint32_t tunedHz;     /* where the receiver is */
    uint8_t slot;         /* the slot listened to, 0 both */
    /* The call followed, or the last one */
    bool inCall;
    uint32_t dst, src;
    bool group, emergency;
    uint16_t lcn;
    uint32_t callHz;
    /* Counters */
    uint32_t csbk, grants, followed, unmapped, hunts;
    uint16_t lastUnmapped; /* the last LPCN no plan resolved */
} dmrTrunkStatus_t;

/* Start on the control channel ctrlHz (the channel's RX frequency; the MS
 * transmits to it on ctrlTxHz), with Ajustes > DMR (c62_t3). Again with
 * another frequency: starts afresh. */
void dmrTrunk_start(uint32_t ctrlHz, uint32_t ctrlTxHz);
void dmrTrunk_stop(void);
bool dmrTrunk_running(void);

/* From dmr_proto, slots 0 (TS1) and 1 (TS2) */
void dmrTrunk_block(int slotIdx, int dt, int cc, const uint8_t bytes[12]);
void dmrTrunk_voice(int slotIdx, int cc);
void dmrTrunk_activity(int slotIdx);
void dmrTrunk_cach(int slotIdx, bool atBusy);

/* From OpMode_DMR_RX, once a loop: the signal level, then where to listen:
 * the frequency and the slot (0 both, 1 TS1, 2 TS2). False when off. */
void dmrTrunk_level(int dbm);
bool dmrTrunk_poll(uint32_t *hz, uint8_t *slot);

void dmrTrunk_getStatus(dmrTrunkStatus_t *out);

/*
 * Registered (C62_T3_REG): the access side. The engine registers by itself
 * and answers the system; the radio sends what it asks for.
 *
 * dmrTrunk_sendPending(): bursts are waiting: the frequency, slot (1, 2)
 * and colour code to send them on. dmrTrunk_sendNext(): once per frame of
 * that transmission, the next burst for the slot (a CSBK, a UDT header or
 * block), false when there is nothing more: the transmission ends there.
 * dmrTrunk_sendDone(): it went out (ok) or was dropped.
 */
bool dmrTrunk_sendPending(uint32_t *txHz, uint8_t *slot, uint8_t *cc);
bool dmrTrunk_sendNext(uint8_t slot, uint8_t *dt, uint8_t bytes[12]);
void dmrTrunk_sendDone(bool ok);

/* The PTT, once a loop: pressed while camped asks for a call to c62_dmr.tg
 * (a group, or a private call), released before the grant cancels it */
void dmrTrunk_ptt(bool down);
/* May talk now, on the call followed: its frequencies, slot, colour code
 * and target */
bool dmrTrunk_talk(uint32_t *txHz, uint8_t *slot, uint8_t *cc,
                   uint32_t *dst, bool *group);
/* What the screen says while the PTT is down and no call is granted */
const char *dmrTrunk_accessText(void);

#ifdef __cplusplus
}
#endif

#endif
