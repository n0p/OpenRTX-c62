// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 OpenRTX Contributors (C62 port)
//
// DMR text messages (SMS), after RadioFoundry's core/dmr/dmrData.c and
// docs/spec/dmr-data.md section 5 (MIT, see src/protocols/DMR/trunk/LICENSE.radiofoundry): the three
// formats, received into an inbox and sent as a list of bursts the DMR mode
// transmits.
//
//  Motorola TMS     UDP 4007, UTF-16LE with the TMS header; the TMS ACK
//  ETSI IP text     UDP 5016, UTF-16BE (TS 102 361-3 5.7.1)
//  ETSI short data  DPF 13, SAP 10, ISO 8859-1 (TS 102 361-1 9.2.12)
//
// What goes out is unconfirmed, rate 1/2 blocks after 16 preamble CSBKs (a
// MOTOTRBO radio sends 15 or 16; BrandMeister answers only with 12 or
// more). Confirmed data to our ID is answered with the ETSI response (ACK)
// and a TMS asking for it with the TMS ACK.
#ifndef DMR_SMS_H
#define DMR_SMS_H

#include <stdbool.h>
#include <stdint.h>

#include "dmr_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DMR_SMS_TEXT_MAX 140 /* characters sent, and kept of a received one */
#define DMR_SMS_INBOX 8      /* messages kept, the newest first */

enum dmrSmsFormat {
    DMR_SMS_TMS = 0,     /* Motorola TMS */
    DMR_SMS_ETSI_IP = 1, /* ETSI IP text */
    DMR_SMS_ETSI_DD = 2, /* ETSI defined short data */
    DMR_SMS_FORMATS
};

struct dmrSmsMsg {
    uint32_t src, dst;
    bool group;
    bool unread;
    uint8_t format;                   /* enum dmrSmsFormat */
    char text[DMR_SMS_TEXT_MAX + 1];  /* ASCII (accents taken off) */
    uint32_t t_ms;
};

/* The state of our last message */
enum dmrSmsTx {
    DMR_SMS_TX_NONE = 0,
    DMR_SMS_TX_QUEUED,  /* waiting for the channel */
    DMR_SMS_TX_SENT,    /* on the air (unconfirmed: nothing more is known) */
    DMR_SMS_TX_FAILED,  /* never went out */
    DMR_SMS_TX_ACKED    /* its TMS ACK came back */
};

/** Our radio ID: messages to it are answered, our own ACKs come from it */
void dmrSms_setOwnId(uint32_t id);

/**
 * A packet the data layer put together (dmr_proto.c). A text goes into the
 * inbox; to our ID, the answers it asks for are queued.
 * @return true if it was a text message (or a TMS ACK).
 */
bool dmrSms_rxPacket(const dmr_packet_t *pk, uint32_t t_ms);

/* Inbox */
unsigned dmrSms_count(void);
unsigned dmrSms_unread(void);
bool dmrSms_get(unsigned i, struct dmrSmsMsg *out); /* i 0: the newest */
void dmrSms_markRead(unsigned i);
void dmrSms_delete(unsigned i);
uint32_t dmrSms_serial(void); /* changes with every change, for the UI */

/**
 * Queue our message.
 * @param src: own radio ID (the sender).
 * @return false if the text is empty or too long for a packet, or another
 * message of ours is still waiting.
 */
bool dmrSms_send(uint32_t dst, bool group, uint8_t format, const char *text,
                 uint32_t src);
/** Our last message: its state (enum dmrSmsTx) and destination */
uint8_t dmrSms_txState(uint32_t *dst, bool *group);

/* For the DMR mode */
bool dmrSms_pending(void);                       /* a list of bursts waits */
bool dmrSms_txNext(uint8_t *dt, uint8_t b[12]);  /* the next of the list */
void dmrSms_txDone(bool ok);                     /* the list went out */
/** A list waiting so long it is given up (now: tick in ms) */
void dmrSms_tick(uint32_t now_ms);

/* For tests: the bursts of a message, built as they go out */
int dmrSms_build(uint32_t dst, bool group, uint8_t format, const char *text,
                 uint32_t src, uint8_t dt[], uint8_t b[][12], int max);

#ifdef __cplusplus
}
#endif

#endif /* DMR_SMS_H */
