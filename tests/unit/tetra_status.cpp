/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * What the TETRA mode publishes, and what the main screen makes of it.
 *
 * The mode has a receive chain that ends in two places: libtetra's event
 * callback, and `struct tetraStatus` (TetraStatus.c), which the UI reads. On
 * the radio the events come from the air; on the host ui_mode_status.c answers
 * "nothing decoded" so the simulator's screens have something to draw, and
 * every state the operator actually works with -- the cell, its MCC-MNC, the
 * location area, what mobility management said, the network broadcast, the
 * uplink -- has no writer and no test.
 *
 * This file is that stand-in. A real `tetra_tmo_rx` is fed with real PDUs:
 *
 *  - the downlink recording libtetra ships as tests/vectors/tmo_call.dibits
 *    (235-78, colour code 1, LA 1, group call 8 from 320026 to 9001), pushed
 *    through the library's own L1/L2 as it is on the air;
 *  - the D-LOCATION-UPDATE-ACCEPT and D-ATTACH/DETACH GROUP IDENTITY bits
 *    libtetra's layer 3 tests replay from its MorganBTS capture;
 *  - a D-NWRK-BROADCAST built to EN 300 392-2 clause 18.4.1.4.1 with the
 *    field layout libtetra's own test uses, since the recording carries no
 *    broadcast;
 *  - an uplink slot built from a U-SDS-DATA encoded by the firmware's own
 *    encoder (TetraSdsEnc.c), scrambled and coded by the library's transmitter
 *    helpers and decoded back by its uplink receiver.
 *
 * Behind them sit the firmware's real translation units -- TetraStatus.c and
 * ui_mode_state.c -- so the assertions are on what the UI would read, not on a
 * copy of the logic: `ui_mode_state_read()` is exactly what the main screen
 * calls, and the strings it returns are what it draws ("T searching",
 * "235-78", "LA1", "ACC G1", "REJ 3").
 *
 * Not covered here: the pixels (the layout is ui_main.c's and the simulator
 * tour's business), the call screen's own lines, which ui_main.c formats from
 * the same status fields asserted below, and the flash record of SDS messages.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

#include <tetra/tetra.h>

extern "C" {
#include "core/state.h"
#include "protocols/TETRA/TetraStatus.h"
#include "protocols/TETRA/TetraSdsEnc.h"
#include "ui/ui_mode_state.h"
}

#include "tetra_host.h"

/* ui_mode_state_read() switches on the current mode: the state the UI keeps */
extern "C" {
state_t state;
}

namespace
{

/*
 * ---- the neighbours of TetraStatus.c that belong to the radio ------------
 *
 * getTick() comes from the platform (interfaces/delays.h); the other two are
 * the speech decoder (OpenACELP) and the SDS store (flash), neither of which
 * is part of "what the mode publishes". The SDS store is driven for real by
 * tests/unit/tetra_sds.cpp.
 */
long long now_ms;

int sdsEvents;
unsigned speechSlots;

} /* namespace */

extern "C" {

long long getTick(void)
{
    return now_ms;
}

void tetraSpeech_traffic(const struct tetra_traffic *t)
{
    (void)t;
    speechSlots++;
}

int tetraSds_onEvent(const struct tetra_sds *s)
{
    (void)s;
    sdsEvents++;
    return -1;
}

} /* extern "C" */

namespace
{

using tetra_test::deliver;
using tetra_test::readDibits;
using tetra_test::sdu;
using tetra_test::writer;

/* A receiver wired the way OpMode_TETRA.cpp wires it: its callback is the
 * status layer's, so every event takes the production path. */
struct standIn {
    struct tetra_tmo_rx rx;
    unsigned events = 0;
    unsigned calls = 0;
    unsigned mm = 0;
    unsigned networks = 0;
    unsigned syncs = 0;
};

void onEvent(void *user, const struct tetra_event *ev)
{
    standIn *s = static_cast<standIn *>(user);

    s->events++;
    switch (ev->type) {
        case TETRA_EV_CALL:
            s->calls++;
            break;
        case TETRA_EV_MM:
            s->mm++;
            break;
        case TETRA_EV_NETWORK:
            s->networks++;
            break;
        case TETRA_EV_SYNC:
            s->syncs++;
            break;
        default:
            break;
    }
    tetraStatus_onEvent(nullptr, ev);
}

void init(standIn &s, uint32_t issi = 0)
{
    struct tetra_rx_config cfg;

    now_ms = 0;
    sdsEvents = 0;
    speechSlots = 0;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cb = onEvent;
    cfg.user = &s;
    cfg.issi = issi;
    cfg.promiscuous = (issi == 0);
    tetra_tmo_init(&s.rx, &cfg);
    tetraStatus_reset();
}

struct tetraStatus status()
{
    struct tetraStatus ts;

    tetra_getStatus(&ts);
    return ts;
}

/* Lock to the recorded downlink, as the receiver does when the carrier is on. */
void lockToRecording(standIn &s)
{
    std::vector<uint8_t> dibits = readDibits(TETRA_VECTOR_DIR
                                             "/tmo_call.dibits");

    REQUIRE(dibits.size() > 0);
    tetra_tmo_push_dibits(&s.rx, dibits.data(), dibits.size());
    REQUIRE(s.syncs >= 1);
    REQUIRE(tetraStatus_sync());
}

/*
 * The same, with the recording's call gone: it ends by timeout, since its
 * release is not in the excerpt. What follows (a location update, a network
 * broadcast) happens with the cell locked and the state block -- not the call
 * screen -- on the display.
 */
void quietCell(standIn &s)
{
    lockToRecording(s);
    now_ms += 10000;
    REQUIRE_FALSE(status().call);
    /* The recording's own events are behind us: what the test delivers next
     * is what the counters should hold */
    s.calls = 0;
    s.mm = 0;
    s.networks = 0;
}

/* What the main screen would draw for the current mode. */
ui_mode_state_t screen()
{
    ui_mode_state_t st;

    state.channel.mode = OPMODE_TETRA;
    ui_mode_state_read(&st);
    return st;
}

/*
 * The D-LOCATION-UPDATE-ACCEPT libtetra's capture replay uses (its
 * tests/unit/test_tmo_l3.cpp), addressed to the same MS as the recording's
 * caller: an ITSI attach that registers and attaches GSSI 9001.
 */
const char *mmLuAccept =
    "0101011110000010011100010000110100000101010000011101000110111000001001"
    "100000010111000000000000001000110010100100";

} /* namespace */

TEST_CASE("A mode that has decoded nothing says so, never zero",
          "[tetra][status][ui]")
{
    static standIn s;

    init(s);

    struct tetraStatus ts = status();
    REQUIRE_FALSE(ts.sync);
    REQUIRE_FALSE(ts.cell);
    REQUIRE_FALSE(ts.sysinfo);
    REQUIRE_FALSE(ts.call);
    REQUIRE_FALSE(ts.registered);
    REQUIRE_FALSE(ts.haveNet);
    REQUIRE_FALSE(ts.uplink);

    /* The screen: "T searching", and no registration note to fill with a
     * zero-loaded "ACC G0" (ui_mode_state.c). */
    ui_mode_state_t st = screen();
    REQUIRE_FALSE(st.sync);
    REQUIRE(std::string(st.id) == "T searching");
    REQUIRE(std::string(st.note) == "");
    REQUIRE(std::string(st.noteShort) == "");
    REQUIRE(std::string(st.callTxt) == "");
}

TEST_CASE("A recorded downlink names the cell and the location area",
          "[tetra][status][ui]")
{
    static standIn s;

    init(s);
    lockToRecording(s);

    struct tetraStatus ts = status();
    REQUIRE(ts.sync);
    REQUIRE(ts.cell);
    REQUIRE(ts.mcc == 235);
    REQUIRE(ts.mnc == 78);
    REQUIRE(ts.colourCode == 1);
    REQUIRE(ts.sysinfo);
    REQUIRE(ts.la == 1);
    REQUIRE(ts.dlFreq == 392512500u);
    /* The SYSINFO published the uplink of the cell (400 MHz, 10 MHz below) */
    REQUIRE(ts.ulFreq == 382512500u);
    /* No mobility management was heard: not registered, no reject either */
    REQUIRE_FALSE(ts.registered);
    REQUIRE(ts.mmCause == 0);

    ui_mode_state_t st = screen();
    REQUIRE(st.sync);
    REQUIRE(std::string(st.id) == "235-78");
    REQUIRE(std::string(st.note) == "LA1");
    REQUIRE(std::string(st.noteShort) == "");
}

TEST_CASE("Registration is what mobility management last said",
          "[tetra][status][ui]")
{
    static standIn s;

    SECTION("A D-LOCATION-UPDATE-ACCEPT registers and counts the groups")
    {
        init(s, 320026);
        quietCell(s);
        deliver(&s.rx, sdu(TETRA_PROTO_MM).bits(mmLuAccept), 320026);
        REQUIRE(s.mm == 1);

        struct tetraStatus ts = status();
        REQUIRE(ts.registered);
        REQUIRE(ts.mmCause == 0);
        REQUIRE(ts.groups == 1);

        /* The note is the registration beside the location area; the short
         * form is the registration alone, for a line where LA1 does not fit */
        ui_mode_state_t st = screen();
        REQUIRE(std::string(st.id) == "235-78");
        REQUIRE(std::string(st.note) == "LA1 ACC G1");
        REQUIRE(std::string(st.noteShort) == "ACC G1");
        REQUIRE(std::strlen(st.note) < UI_MODE_STATE_LEN);
    }

    SECTION("A D-LOCATION-UPDATE-REJECT unregisters and shows its cause")
    {
        const uint8_t cause = 3; /* roaming, LA not allowed, no ciphering */

        init(s, 1234);
        quietCell(s);
        /* Reject to our own SSI: see libtetra's test_tmo_l3.cpp */
        writer rej =
            sdu(TETRA_PROTO_MM).put(7, 4).put(0, 3).put(3, 5).put(0, 2);

        deliver(&s.rx, rej, 1234);
        REQUIRE(s.mm == 1);

        struct tetraStatus ts = status();
        REQUIRE_FALSE(ts.registered);
        REQUIRE(ts.mmCause == cause);

        ui_mode_state_t st = screen();
        REQUIRE(std::string(st.note) == "LA1 REJ 3");
        REQUIRE(std::string(st.noteShort) == "REJ 3");
    }

    SECTION("An accepted location update clears an older reject")
    {
        init(s, 320026);
        quietCell(s);
        deliver(&s.rx,
                sdu(TETRA_PROTO_MM).put(7, 4).put(0, 3).put(3, 5).put(0, 2),
                320026);
        REQUIRE(status().mmCause == 3);
        REQUIRE(std::string(screen().noteShort) == "REJ 3");

        deliver(&s.rx, sdu(TETRA_PROTO_MM).bits(mmLuAccept), 320026);

        struct tetraStatus ts = status();
        REQUIRE(ts.registered);
        REQUIRE(ts.mmCause == 0);
        REQUIRE(std::string(screen().noteShort) == "ACC G1");
    }

    SECTION("A group detach-all replaces the attach and counts what is left")
    {
        init(s, 320026);
        quietCell(s);
        deliver(&s.rx, sdu(TETRA_PROTO_MM).bits(mmLuAccept), 320026);
        REQUIRE(status().registered);
        REQUIRE(status().groups == 1);

        /* D-ATTACH/DETACH GROUP IDENTITY: detach all, then attach GSSI 77
         * with an address extension (libtetra's test_tmo_l3.cpp) */
        writer det = sdu(TETRA_PROTO_MM)
                         .put(10, 4)
                         .put(0, 1)
                         .put(0, 1)
                         .put(1, 1) /* detach all */
                         .put(1, 1) /* O-bit */
                         .put(1, 1) /* M-bit */
                         .put(7, 4)
                         .put(6 + 1 + 5 + 2 + 48, 11)
                         .put(1, 6)
                         .put(0, 1)
                         .put(1, 2)
                         .put(2, 3)
                         .put(1, 2) /* GSSI + extension */
                         .put(77, 24)
                         .put(0x0abcde, 24)
                         .put(0, 1);

        deliver(&s.rx, det, 320026);

        struct tetraStatus ts = status();
        REQUIRE(ts.registered); /* the attach does not unregister by itself */
        REQUIRE(ts.detachAll);
        REQUIRE(ts.groups == 1);
        REQUIRE(std::string(screen().noteShort) == "ACC G1");
    }
}

TEST_CASE("The network broadcast is published beside the cell line",
          "[tetra][status][ui]")
{
    static standIn s;

    init(s, 320026);
    quietCell(s);
    deliver(&s.rx, sdu(TETRA_PROTO_MM).bits(mmLuAccept), 320026);

    /*
     * D-NWRK-BROADCAST, the field layout of libtetra's own
     * test_tmo_l3.cpp: cell load 2, network time 2026 with UTC 2*0x00a000
     * and -1 h of local offset, two neighbours (cell 3 with a carrier
     * extension, LA 7 and a TDMA offset, cell 4 mandatory part only).
     */
    writer nw = sdu(TETRA_PROTO_MLE);

    nw.put(MLE_D_NWRK_BROADCAST, 3).put(0xaa65, 16).put(2, 2);
    nw.put(1, 1);                                     /* O-bit */
    nw.put(1, 1).put(0x00a000, 24);                   /* network time: UTC */
    nw.put(1, 1).put(4, 6).put(26, 6).put(0x7ff, 11); /* -1 h, 2026 */
    nw.put(1, 1).put(2, 3);                           /* two neighbour cells */
    nw.put(3, 5).put(2, 2).put(1, 1).put(1, 2).put(1234, 12);
    nw.put(1, 1);                                     /* O-bit */
    nw.put(1, 1).put(3, 4).put(1, 2).put(0, 3).put(0, 1); /* extension */
    nw.put(0, 1).put(0, 1);                               /* MCC, MNC */
    nw.put(1, 1).put(7, 14);                              /* LA */
    nw.put(0, 5);                                         /* not present */
    nw.put(1, 1).put(5, 6);                               /* frame offset */
    nw.put(4, 5).put(0, 2).put(0, 1).put(3, 2).put(1300, 12).put(0, 1);

    deliver(&s.rx, nw, 0xffffff);
    REQUIRE(s.networks == 1);

    struct tetraStatus ts = status();
    REQUIRE(ts.haveNet);
    REQUIRE(ts.netLoad == 2);
    REQUIRE(ts.neighbours == 2);
    REQUIRE(ts.netTime);
    REQUIRE(ts.netYear == 2026);
    REQUIRE(ts.netUtc == 2 * 0x00a000);
    REQUIRE(ts.netOffsetMin == -60);

    /*
     * The operator's line does not grow a neighbour count: it does not fit
     * beside the cell (ui_mode_state.c) and stays a diagnostic. What the
     * screen shows is still the cell and the registration.
     */
    ui_mode_state_t st = screen();
    REQUIRE(std::string(st.id) == "235-78");
    REQUIRE(std::string(st.note) == "LA1 ACC G1");
    REQUIRE(std::strlen(st.note) < UI_MODE_STATE_LEN);
}

TEST_CASE("A call takes the screen over and times out", "[tetra][status][ui]")
{
    static standIn s;

    init(s);
    lockToRecording(s);

    struct tetraStatus ts = status();
    /* D-CALL PROCEEDING, D-SETUP (from 320026 to group 9001) and traffic */
    REQUIRE(ts.call);
    REQUIRE(ts.callId == 8);
    REQUIRE(ts.callDst == 9001);
    REQUIRE(ts.callGroup);
    REQUIRE(ts.talker == 320026);
    REQUIRE_FALSE(ts.callEncrypted);
    REQUIRE(ts.voice);
    REQUIRE((ts.tn >= 1 && ts.tn <= 4));

    /*
     * The call screen owns the four lines while sync and call are set
     * (ui_main.c, _ui_tetraCallShown), and it formats these fields:
     * "320026", "G 9001", "235-78 LA 1" and "Voice, call 8".
     */
    REQUIRE(ts.sync);
    ui_mode_state_t st = screen();
    REQUIRE(st.sync);

    /* Ten seconds without a PDU and the call is gone from the screen, with
     * no release heard (CALL_TIMEOUT_MS) */
    now_ms += 9999;
    REQUIRE(status().call);
    now_ms += 1;
    REQUIRE_FALSE(status().call);
    REQUIRE_FALSE(status().voice);
    /* And the timeslot goes with it: no call, no slot to point at */
    REQUIRE(status().tn == 0);
}

TEST_CASE("An uplink transmission is published and expires",
          "[tetra][status][ui]")
{
    static standIn s;
    const uint16_t mcc = 235, mnc = 78;
    const uint8_t cc = 1;
    const uint32_t ms = 2001;

    init(s);
    tetra_tmo_set_uplink_cell(&s.rx, mcc, mnc, cc);

    /* What a radio's message looks like on the air: the firmware's own
     * encoder, the library's SCH coding and scrambling, one uplink slot */
    struct sds_tx cmce;
    struct sds_tx llc;
    uint8_t mac[268];

    REQUIRE(tetraSds_encodeText(&cmce, 3001, "HOLA", false) == 0);
    REQUIRE(sds_tx_wrap_llc(&llc, &cmce) == 0);
    REQUIRE(sds_tx_mac_data(mac, ms, &llc) == 0);

    tetra_test::uplink ul(mcc, mnc, cc);
    ul.normal(std::vector<uint8_t>(mac, mac + 268));
    ul.push(&s.rx);

    struct tetraStatus ts = status();
    REQUIRE(ts.uplink);
    REQUIRE(ts.uplinkSs == ms); /* the sending MS, not the addressed one */
    REQUIRE(sdsEvents == 1);

    /* Three seconds after the last uplink PDU (UPLINK_TIMEOUT_MS) the flag
     * goes away, so the screen stops saying "UL 2001" */
    now_ms += 2999;
    REQUIRE(status().uplink);
    now_ms += 1;
    REQUIRE_FALSE(status().uplink);
}

TEST_CASE("Losing sync clears the call and keeps the cell",
          "[tetra][status][ui]")
{
    static standIn s;

    init(s);
    lockToRecording(s);
    REQUIRE(status().call);

    /* What libtetra reports when the burst synchronisation goes away */
    struct tetra_event ev;

    std::memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_SYNC;
    ev.u.sync.locked = false;
    tetraStatus_onEvent(nullptr, &ev);

    struct tetraStatus ts = status();
    REQUIRE_FALSE(ts.sync);
    REQUIRE_FALSE(ts.call);
    /* The cell information stays: a receiver that lost the slot grid still
     * knows which cell it was listening to (TetraStatus.h) */
    REQUIRE(ts.cell);
    REQUIRE(ts.mcc == 235);

    ui_mode_state_t st = screen();
    REQUIRE_FALSE(st.sync);
    REQUIRE(std::string(st.id) == "T searching");

    /* The same from the demodulator's side, with its counters */
    tetraStatus_lostSync(true);
    ts = status();
    REQUIRE_FALSE(ts.sync);
    REQUIRE(ts.resyncs == 1);
    REQUIRE(ts.overruns == 1);
}
