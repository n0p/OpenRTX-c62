/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The SDS message path, end to end at the message level.
 *
 * Short data crosses the receive chain in three steps and this file walks all
 * three, each with the real code:
 *
 *  1. the air, or a stand-in for it: on the downlink a D-SDS-DATA /
 *     D-STATUS PDU handed to libtetra's layer 3 (`deliver()`, tests/unit/
 *     tetra_host.h); on the uplink a U-SDS-DATA / U-STATUS encoded by the
 *     firmware's own TetraSdsEnc.c, wrapped in LLC and MAC-DATA by the same
 *     file, and then coded, scrambled and burst-built by libtetra's
 *     transmitter helpers so its real uplink receiver decodes it back;
 *  2. libtetra: `struct tetra_sds`, the same event the radio gets;
 *  3. the firmware: SdsMsg.c turns it into the message model (kind, font-safe
 *     text, status value), SdsHist.c keeps the last eight, and TetraStatus.c
 *     publishes the newest one for the main screen (status.sds / sdsSrc).
 *
 * So a text typed into a radio, encoded by this firmware, decoded by the
 * receiver and shown by SdsMsg.c has to come back identical -- and a message
 * received twice, or a status instead of a text, has to land in the ring as
 * what it is. Nothing here keys the radio: TETRA transmit is not viable on
 * this hardware (constant-envelope PA), so the encoder's output is only ever
 * fed to the receiver.
 *
 * Not covered here: the flash record's write path and the paging line
 * (TetraSds.c, which owns the sectors and the 4 s pager), and the speech
 * decoder.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra.h>

extern "C" {
#include "protocols/TETRA/SdsHist.h"
#include "protocols/TETRA/SdsMsg.h"
#include "protocols/TETRA/TetraSdsEnc.h"
#include "protocols/TETRA/TetraStatus.h"
}

#include "tetra_host.h"

namespace
{

long long now_ms;

} /* namespace */

extern "C" {

long long getTick(void)
{
    return now_ms;
}

void tetraSpeech_traffic(const struct tetra_traffic *t)
{
    (void)t;
}

/*
 * The RAM half of tetraSds_onEvent() (TetraSds.c). The real one decodes, pushes
 * into the ring and marks the record dirty; the flash sectors it then writes
 * are the radio's, so this keeps the first two steps -- which is where a
 * message could be lost or mangled -- and leaves the write out.
 */
int tetraSds_onEvent(const struct tetra_sds *s)
{
    struct sds_msg m;

    if (sds_msg_decode(s, &m) != 0 || !sds_msg_has_text(&m))
        return -1;
    return (sds_hist_push(&m, (uint32_t)getTick()) < 0) ? -1 : 0;
}

} /* extern "C" */

namespace
{

using tetra_test::deliver;
using tetra_test::sdu;
using tetra_test::writer;

/* What arrived, already decoded the way TetraStatus.c decodes it. */
struct message {
    struct tetra_sds sds; /* addressing and type, no payload pointer */
    struct sds_msg msg;   /* the firmware's model: kind, text, status */
};

struct capture {
    struct tetra_tmo_rx rx;
    std::vector<message> got;
    unsigned uplinkEvents = 0;
};

void onEvent(void *user, const struct tetra_event *ev)
{
    capture *c = static_cast<capture *>(user);

    if (ev->type == TETRA_EV_SDS) {
        message m;

        std::memset(&m, 0, sizeof(m));
        m.sds = ev->u.sds;
        m.sds.data = nullptr; /* valid only during the callback */
        if (sds_msg_decode(&ev->u.sds, &m.msg) == 0)
            c->got.push_back(m);
        if (ev->uplink)
            c->uplinkEvents++;
    }
    tetraStatus_onEvent(nullptr, ev);
}

void init(capture &c)
{
    struct tetra_rx_config cfg;

    now_ms = 0;
    c.got.clear();
    c.uplinkEvents = 0;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cb = onEvent;
    cfg.user = &c;
    cfg.promiscuous = true;
    tetra_tmo_init(&c.rx, &cfg);
    tetraStatus_reset();
    sds_hist_reset();
}

struct tetraStatus status()
{
    struct tetraStatus ts;

    tetra_getStatus(&ts);
    return ts;
}

/*
 * TM-SDU of a D-SDS-DATA with an SDS-TL text, and of a D-STATUS. The field
 * layout is the one libtetra's own test_tmo.cpp writes (cmce_sds_text,
 * cmce_status); the text goes GSM 7-bit with protocol id 0x82 (SDS-TRANSFER).
 */
writer cmceSdsText(uint32_t src, const char *text)
{
    const unsigned len = (unsigned)std::strlen(text);
    const unsigned octets = (7 * len + 7) / 8;
    uint8_t packed[200];

    std::memset(packed, 0, sizeof(packed));
    for (unsigned i = 0; i < len; i++) {
        unsigned v = (unsigned)text[i] & 0x7f, bit = 7 * i;

        packed[bit / 8] |= (uint8_t)(v << (bit % 8));
        if (bit % 8 > 1)
            packed[bit / 8 + 1] |= (uint8_t)(v >> (8 - bit % 8));
    }

    writer p = sdu(TETRA_PROTO_CMCE);

    p.put(15, 5)               /* D-SDS-DATA */
        .put(1, 2)             /* calling party: SSI */
        .put(src, 24)
        .put(3, 2)             /* short data type: UDD4 */
        .put(8 + 4 + 2 + 1 + 1 + 8 + 1 + 7 + 8 * octets, 11)
        .put(0x82, 8)          /* SDS-TRANSFER */
        .put(0, 4 + 2 + 1 + 1) /* no receipt, no storage */
        .put(7, 8)             /* message reference */
        .put(0, 1)             /* no time stamp */
        .put(TETRA_TCS_GSM7, 7);
    for (unsigned i = 0; i < octets; i++)
        p.put(packed[i], 8);
    return p.put(0, 1); /* O-bit */
}

writer cmceStatus(uint32_t src, uint16_t status)
{
    writer p = sdu(TETRA_PROTO_CMCE);

    return p
        .put(8, 5) /* D-STATUS */
        .put(1, 2) /* calling party: SSI */
        .put(src, 24)
        .put(status, 16)
        .put(0, 1); /* O-bit */
}

/* An uplink slot carrying one CMCE PDU, the way the encoder's output goes. */
void sendUplink(capture &c, uint16_t mcc, uint16_t mnc, uint8_t cc,
                uint32_t from, const struct sds_tx &cmce)
{
    struct sds_tx llc;
    uint8_t mac[268];

    tetra_tmo_set_uplink_cell(&c.rx, mcc, mnc, cc);
    REQUIRE(sds_tx_wrap_llc(&llc, &cmce) == 0);
    REQUIRE(sds_tx_mac_data(mac, from, &llc) == 0);

    tetra_test::uplink ul(mcc, mnc, cc);

    ul.normal(std::vector<uint8_t>(mac, mac + 268));
    ul.push(&c.rx);
}

} /* namespace */

TEST_CASE("A D-SDS-DATA arrives as the text the operator reads", "[tetra][sds]")
{
    static capture c;

    init(c);
    /* A group message from 320026 to the talkgroup 9001 (the MAC address) */
    deliver(&c.rx, cmceSdsText(320026, "HOLA"), 9001);

    REQUIRE(c.got.size() == 1);
    const message &m = c.got[0];
    REQUIRE(m.sds.type == TETRA_SDS_UDD4);
    REQUIRE(m.sds.complete);
    REQUIRE(m.sds.src_ssi == 320026);
    REQUIRE(m.sds.dst_ssi == 9001);

    /* The message model: a text, SDS-TL, GSM 7-bit, second message reference */
    REQUIRE(m.msg.kind == SDS_KIND_TEXT);
    REQUIRE(m.msg.pid == 0x82);
    REQUIRE(m.msg.sdstl);
    REQUIRE(m.msg.coding == TETRA_TCS_GSM7);
    REQUIRE(m.msg.msg_ref == 7);
    REQUIRE_FALSE(m.msg.has_timestamp);
    REQUIRE_FALSE(m.msg.truncated);
    REQUIRE(std::string(m.msg.text) == "HOLA");

    /* One line for the list, sender first */
    char line[64];
    REQUIRE(sds_msg_format(&m.msg, line, sizeof(line)) > 0);
    REQUIRE(std::string(line) == "320026: HOLA");

    /* The ring got it, unread, and the main screen's status too */
    REQUIRE(sds_hist_count() == 1);
    const struct sds_hist_entry *e = sds_hist_get(0);
    REQUIRE(e != nullptr);
    REQUIRE(std::string(e->text) == "HOLA");
    REQUIRE(e->src_ssi == 320026);
    REQUIRE(e->dst_ssi == 9001);
    REQUIRE((e->flags & SDS_HIST_UNREAD) != 0);

    struct tetraStatus ts = status();
    REQUIRE(std::string(ts.sds) == "HOLA");
    REQUIRE(ts.sdsSrc == 320026);
    REQUIRE(ts.sdsTime == now_ms);
}

TEST_CASE("A D-STATUS is a pre-coded status, not text", "[tetra][sds]")
{
    static capture c;

    init(c);
    deliver(&c.rx, cmceStatus(320026, 7), 9001);

    REQUIRE(c.got.size() == 1);
    const message &m = c.got[0];
    REQUIRE(m.sds.type == TETRA_SDS_STATUS);
    REQUIRE(m.sds.status == 7);
    REQUIRE(m.msg.kind == SDS_KIND_STATUS);
    REQUIRE(m.msg.status == 7);
    REQUIRE(std::string(m.msg.text) == "STATUS 7");

    REQUIRE(sds_hist_count() == 1);
    REQUIRE(sds_hist_get(0)->kind == SDS_KIND_STATUS);
    REQUIRE(std::string(status().sds) == "STATUS 7");
}

TEST_CASE("A text the mode encodes comes back from the receiver unchanged",
          "[tetra][sds]")
{
    static capture c;

    const uint16_t mcc = 235, mnc = 78;
    const uint8_t cc = 1;
    const uint32_t from = 2001, to = 3001;

    SECTION("GSM 7-bit")
    {
        struct sds_tx cmce;

        init(c);
        REQUIRE(tetraSds_encodeText(&cmce, to, "HOLA 123", false) == 0);
        sendUplink(c, mcc, mnc, cc, from, cmce);

        REQUIRE(c.got.size() == 1);
        REQUIRE(c.uplinkEvents == 1);
        /* The sender is the MAC address of the burst, the called party the
         * one in the PDU (a transmitter of the network, heard from the
         * uplink of the cell) */
        REQUIRE(c.got[0].sds.src_ssi == from);
        REQUIRE(c.got[0].sds.dst_ssi == to);
        REQUIRE(c.got[0].msg.kind == SDS_KIND_TEXT);
        REQUIRE(std::string(c.got[0].msg.text) == "HOLA 123");

        /* And the mode publishes it: the uplink, the sender, the text */
        struct tetraStatus ts = status();
        REQUIRE(ts.uplink);
        REQUIRE(ts.uplinkSs == from);
        REQUIRE(std::string(ts.sds) == "HOLA 123");
        REQUIRE(ts.sdsSrc == from);
    }

    SECTION("ISO 8859-1 letters the default alphabet has")
    {
        struct sds_tx cmce;

        init(c);
        /* "Grüße" in ISO 8859-1: ü and ß are septets of the default alphabet
         * (0x7e and 0x1e), so this stays GSM 7-bit, and the message model
         * folds the two letters to the 'u' and 's' the font draws */
        REQUIRE(tetraSds_encodeText(&cmce, to,
                                    "Gr\xfc\xdf"
                                    "e",
                                    false)
                == 0);
        sendUplink(c, mcc, mnc, cc, from, cmce);

        REQUIRE(c.got.size() == 1);
        REQUIRE(c.got[0].msg.kind == SDS_KIND_TEXT);
        REQUIRE(c.got[0].msg.coding == TETRA_TCS_GSM7);
        REQUIRE(std::string(c.got[0].msg.text) == "Gruse");
    }

    SECTION("Characters outside the GSM alphabet go as UCS-2")
    {
        struct sds_tx cmce;

        init(c);
        /* "aça": ç (0xe7) is not a septet of the default alphabet, so the
         * encoder falls back to UCS-2 and the model folds it to 'c' */
        REQUIRE(tetraSds_encodeText(&cmce, to,
                                    "a\xe7"
                                    "a",
                                    false)
                == 0);
        sendUplink(c, mcc, mnc, cc, from, cmce);

        REQUIRE(c.got.size() == 1);
        REQUIRE(c.got[0].msg.kind == SDS_KIND_TEXT);
        REQUIRE(c.got[0].msg.coding == TETRA_TCS_UCS2);
        REQUIRE(std::string(c.got[0].msg.text) == "aca");
    }

    SECTION("An SDS-TL transfer keeps its protocol header")
    {
        struct sds_tx cmce;

        init(c);
        REQUIRE(tetraSds_encodeText(&cmce, to, "OK", true) == 0);
        sendUplink(c, mcc, mnc, cc, from, cmce);

        REQUIRE(c.got.size() == 1);
        REQUIRE(c.got[0].msg.pid == 0x82);
        REQUIRE(c.got[0].msg.sdstl);
        REQUIRE(std::string(c.got[0].msg.text) == "OK");
    }

    SECTION("A U-STATUS carries the pre-coded status")
    {
        struct sds_tx cmce;

        init(c);
        REQUIRE(sds_tx_u_status(&cmce, to, SDS_ENC_AREA_DEFAULT, 7) == 0);
        sendUplink(c, mcc, mnc, cc, from, cmce);

        REQUIRE(c.got.size() == 1);
        REQUIRE(c.got[0].sds.type == TETRA_SDS_STATUS);
        REQUIRE(c.got[0].msg.status == 7);
        REQUIRE(std::string(c.got[0].msg.text) == "STATUS 7");
        REQUIRE(std::string(status().sds) == "STATUS 7");
    }
}

TEST_CASE("A burst the cell's colour code does not scramble reads as nothing",
          "[tetra][sds]")
{
    static capture c;
    struct sds_tx cmce;

    init(c);
    REQUIRE(tetraSds_encodeText(&cmce, 3001, "HOLA", false) == 0);
    /* The same burst heard twice: first with the colour code the cell
     * scrambled it with, then with another one. The second must decode to
     * nothing, or the operator would read other cells' traffic */
    sendUplink(c, 235, 78, 1, 2001, cmce);
    REQUIRE(c.got.size() == 1);

    init(c);
    tetra_tmo_set_uplink_cell(&c.rx, 235, 78, 2); /* not the cell's CC 1 */
    {
        struct sds_tx llc;
        uint8_t mac[268];

        REQUIRE(sds_tx_wrap_llc(&llc, &cmce) == 0);
        REQUIRE(sds_tx_mac_data(mac, 2001, &llc) == 0);

        tetra_test::uplink ul(235, 78, 1); /* scrambled as the cell, not as
                                              the receiver expects */
        ul.normal(std::vector<uint8_t>(mac, mac + 268));
        ul.push(&c.rx);
    }
    REQUIRE(c.got.empty());
    REQUIRE(sds_hist_count() == 0);
}

TEST_CASE("The message ring and its record survive a power cycle",
          "[tetra][sds]")
{
    static capture c;

    init(c);
    deliver(&c.rx, cmceSdsText(320026, "PRIMERO"), 9001);
    deliver(&c.rx, cmceStatus(4242, 12), 9001);
    REQUIRE(sds_hist_count() == 2);

    /* The packed record the firmware writes to its own two sectors */
    uint8_t record[1024];

    std::memset(record, 0, sizeof(record));
    int len = sds_hist_serialise(record, sizeof(record));

    REQUIRE(len > 6);
    REQUIRE((size_t)len == sds_hist_record_size());

    /* Newest first, both unread; then the power cycle */
    REQUIRE(std::string(sds_hist_get(0)->text) == "STATUS 12");
    REQUIRE(std::string(sds_hist_get(1)->text) == "PRIMERO");

    sds_hist_reset();
    REQUIRE(sds_hist_count() == 0);
    REQUIRE(sds_hist_deserialise(record, (size_t)len) == 2);

    const struct sds_hist_entry *e = sds_hist_get(0);

    REQUIRE(e != nullptr);
    REQUIRE(std::string(e->text) == "STATUS 12");
    REQUIRE(e->kind == SDS_KIND_STATUS);
    REQUIRE(e->status == 12);
    REQUIRE(std::string(sds_hist_get(1)->text) == "PRIMERO");
    /* A restored message has been shown already (docs: unread is RAM only) */
    REQUIRE(sds_hist_unread() == 0);

    /* A record cut short in the write is not restored */
    sds_hist_reset();
    REQUIRE(sds_hist_deserialise(record, (size_t)len - 1) < 0);
    REQUIRE(sds_hist_count() == 0);
}
