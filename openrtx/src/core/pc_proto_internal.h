/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PC_PROTO_INTERNAL_H
#define PC_PROTO_INTERNAL_H

/*
 * Shared by pc_proto.c (framing, dispatch, session) and pc_proto_ops.c (the
 * handlers that read and write the stores).  Not part of the public API.
 */

#include "core/pc_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One dispatched request and the response being built for it. */
struct pcReq
{
    pcProto_t      *p;
    uint8_t         seq;     /**< echo of the request sequence            */
    uint8_t         flags;   /**< request flags                           */
    uint16_t        id;      /**< request identifier                      */
    const uint8_t  *in;      /**< request payload, TLV                    */
    size_t          inLen;
    uint8_t        *out;     /**< response payload, TLV                   */
    size_t          outCap;
    size_t          outLen;
    int             err;     /**< set by a handler to fail the request    */
};

typedef int (*pcProtoHandler)(struct pcReq *r);

/** One row of the message table. */
struct pcProtoEntry
{
    uint16_t       id;
    uint32_t       caps;   /**< 0: always served; else every bit needed  */
    pcProtoHandler fn;
};

/** Build a response or an event frame and hand it to the port. */
void pc_proto_sendResponse(pcProto_t *p, uint8_t seq, uint16_t id,
                           uint8_t flags, const uint8_t *payload, size_t len);
void pc_proto_sendEvent(pcProto_t *p, uint16_t id, const uint8_t *payload,
                        size_t len);
void pc_proto_sendError(pcProto_t *p, uint8_t seq, uint16_t id, int err);

/** Fill a status structure from the platform hook, zeroed when it fails. */
int pc_proto_status(pcProtoStatus_t *out);

/** Append the live-status TLV block to a payload; used by STATUS_GET and by
 *  the periodic event so both always carry the same fields. */
size_t pc_proto_statusTlv(const pcProtoStatus_t *s, uint8_t *buf, size_t cap,
                          size_t off);

/* Core handlers (pc_proto.c) */
int pc_core_ping(struct pcReq *r);
int pc_core_sessionClose(struct pcReq *r);
int pc_core_counters(struct pcReq *r);

/* Store handlers (pc_proto_ops.c) */
int pc_ops_chanCount(struct pcReq *r);
int pc_ops_chanGet(struct pcReq *r);
int pc_ops_chanSet(struct pcReq *r);
int pc_ops_chanAdd(struct pcReq *r);
int pc_ops_chanDelete(struct pcReq *r);
int pc_ops_chanList(struct pcReq *r);
int pc_ops_chanWriteBlock(struct pcReq *r);
int pc_ops_chanLastUsed(struct pcReq *r);

int pc_ops_contactCount(struct pcReq *r);
int pc_ops_contactGet(struct pcReq *r);
int pc_ops_contactSet(struct pcReq *r);
int pc_ops_contactAdd(struct pcReq *r);
int pc_ops_contactDelete(struct pcReq *r);
int pc_ops_contactList(struct pcReq *r);

int pc_ops_zoneCount(struct pcReq *r);
int pc_ops_zoneGet(struct pcReq *r);
int pc_ops_zoneChannel(struct pcReq *r);
int pc_ops_zoneImport(struct pcReq *r);

int pc_ops_settingsGet(struct pcReq *r);
int pc_ops_settingsSet(struct pcReq *r);
int pc_ops_settingsInfo(struct pcReq *r);

int pc_ops_statusGet(struct pcReq *r);
int pc_ops_statusSubscribe(struct pcReq *r);
int pc_ops_statsGet(struct pcReq *r);

int pc_ops_msgCount(struct pcReq *r);
int pc_ops_msgInbox(struct pcReq *r);
int pc_ops_msgOutbox(struct pcReq *r);
int pc_ops_msgSend(struct pcReq *r);
int pc_ops_msgDelete(struct pcReq *r);
int pc_ops_msgMarkRead(struct pcReq *r);

/** Reserved blocks: served only when the feature is built in. */
int pc_ops_reserved(struct pcReq *r);

/** True when the request carries the session token handed out by HELLO. */
bool pc_proto_tokenOk(const pcProto_t *p, const uint8_t *tlv, size_t len);

/* Debug namespace (pc_proto_dbg.c).  All of them answer PC_ERR_NOT_ALLOWED
 * when the build has no CONFIG_C62_DEBUG_PROTO; the dispatcher refuses them
 * before that on the capability, so both gates have to fail for data to
 * travel. */
int pc_dbg_info(struct pcReq *r);
int pc_dbg_logSub(struct pcReq *r);
int pc_dbg_logDrain(struct pcReq *r);
int pc_dbg_regRead(struct pcReq *r);
int pc_dbg_regWrite(struct pcReq *r);
int pc_dbg_agc(struct pcReq *r);
int pc_dbg_memRead(struct pcReq *r);
int pc_dbg_capStart(struct pcReq *r);
int pc_dbg_capStop(struct pcReq *r);
int pc_dbg_capRead(struct pcReq *r);
int pc_dbg_bb(struct pcReq *r);
int pc_dbg_txTrace(struct pcReq *r);
int pc_dbg_rxCounters(struct pcReq *r);

#ifdef __cplusplus
}
#endif

#endif /* PC_PROTO_INTERNAL_H */
