/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file messaging_aprs.c
 * @brief The APRS leg of the shared message store.
 *
 * APRS is not an operating mode: packets go out on the FM path, and
 * aprs_service owns the beacon timer, the channel gap and the retry schedule of
 * its mailbox.  This bridge only moves queued messages from the store into
 * that mailbox, so the messaging screen has one place to send from whatever
 * the transport.
 *
 * Called from the FM mode's update loop, which is where the APRS service is
 * pumped today.
 */

#include "core/messaging.h"
#include "core/aprs_service.h"

void aprs_pumpMessaging(uint32_t now)
{
    /*
     * Bounded by the size of the outbox: each turn moves one message, and a
     * mailbox that refuses the message stops the loop instead of spinning.
     */
    for (int guard = 0; guard < MSG_OUTBOX_LEN; guard++) {
        const struct message *msg = messaging_head(MSG_TRANSPORT_APRS);
        unsigned id;

        if (msg == NULL)
            return;
        id = msg->id;

        if (msg->peer[0] == '\0') {
            /* APRS needs a callsign and this message has none: give up. */
            messaging_fail((int)id, now);
            continue;
        }

        int begun = messaging_begin(MSG_TRANSPORT_APRS);

        if (begun < 0)
            return;
        /*
         * The mailbox keeps the text and its own retries; from the store's
         * point of view the message is sent as soon as it is in the queue.
         */
        if (!aprs_sendMessage(msg->peer, msg->text, now)) {
            messaging_done(begun, false, 0, now);
            return;
        }
        messaging_done(begun, true, 0, now);
    }
}
