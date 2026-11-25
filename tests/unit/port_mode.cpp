/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The accessory port: one pad, three modes (core/port_ptt.h, docs/ACCESSORY.md).
 *
 * The C62's accessory jack carries the speaker-microphone and the programming
 * cable at once, and its PTT contact is the same GPIOA_15 that is UART2 RXD.
 * What this suite pins down, against the real parser and the real framing:
 *
 *  - a finger on the switch (50 ms) is a press and a byte is not, including a
 *    0x00 byte at 115200 baud and a short break;
 *  - in "Auto" HELLO frames before, between and after a press are answered, the
 *    bytes that arrive while the switch is held are discarded and counted, the
 *    half frame the parser had at that moment is dropped, and the session
 *    survives;
 *  - the three modes mean what the menu says they mean: Always never reads the
 *    switch, Never never listens to the cable, Auto does both.
 *
 * The simulated line is the same shape as the transport's loop in
 * platform/targets/c62/pc_proto_c62.c: one pass per millisecond, sample the
 * pad, discard or parse.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/pc_proto.h"
#include "core/port_ptt.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/crc.h"
}

/* ------------------------------------------------------------------------- */
/* Platform hooks the protocol calls                                          */
/* ------------------------------------------------------------------------- */

static uint8_t hookPortMode = PORT_MODE_AUTO;
static uint32_t hookPortLost = 0;

extern "C" {

int pc_proto_platformStatus(pcProtoStatus_t *out)
{
    std::memset(out, 0, sizeof(*out));
    out->rxFreq = 145500000;
    out->txFreq = 145500000;
    out->mode = 1;
    out->channelIdx = 0xFFFF;
    out->portMode = hookPortMode;
    out->portLost = hookPortLost;
    return 0;
}

const char *pc_proto_platformModel(void) { return "c62-port-test"; }
const char *pc_proto_platformFirmware(void) { return "test-fw"; }
const char *pc_proto_platformHardware(void) { return "rev 0"; }

} /* extern "C" */

namespace {

/* ------------------------------------------------------------------------- */
/* Wire and frames, exactly as a client reads them                            */
/* ------------------------------------------------------------------------- */

std::vector<uint8_t> wire;

int sinkWrite(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    wire.insert(wire.end(), buf, buf + len);
    return (int)len;
}

const struct pcProtoPort sink = { .write = sinkWrite, .ctx = nullptr };

struct Frame
{
    uint8_t type;
    uint8_t seq;
    uint16_t id;
    std::vector<uint8_t> payload;
};

std::vector<Frame> takeFrames()
{
    std::vector<Frame> frames;
    std::vector<uint8_t> body;
    bool inFrame = false;
    bool esc = false;

    for (uint8_t b : wire) {
        if (b == PC_PROTO_END) {
            if (inFrame && body.size() >= 10) {
                uint16_t plen = (uint16_t)(body[6] | (body[7] << 8));
                uint16_t crc = (uint16_t)(body[body.size() - 2] |
                                          (body[body.size() - 1] << 8));

                if (body[0] == PC_PROTO_FRAME_VERSION &&
                    plen == body.size() - 10 &&
                    crc_ccitt_update(0xFFFF, body.data(), body.size() - 2) ==
                        crc) {
                    Frame f;
                    f.type = body[1];
                    f.seq = body[2];
                    f.id = (uint16_t)(body[3] | (body[4] << 8));
                    f.payload.assign(body.begin() + 8, body.end() - 2);
                    frames.push_back(f);
                }
            }
            body.clear();
            inFrame = true;
            esc = false;
            continue;
        }
        if (!inFrame)
            continue;
        if (esc) {
            esc = false;
            body.push_back(b == PC_PROTO_ESC_END ? PC_PROTO_END
                          : b == PC_PROTO_ESC_ESC ? PC_PROTO_ESC : b);
        } else if (b == PC_PROTO_ESC) {
            esc = true;
        } else {
            body.push_back(b);
        }
    }
    wire.clear();
    return frames;
}

std::vector<uint8_t> encode(uint16_t id, uint8_t seq,
                            const std::vector<uint8_t> &payload = {})
{
    uint8_t buf[256];
    size_t n = pc_proto_encode(PC_TYPE_REQUEST, seq, id, 0,
                               payload.empty() ? nullptr : payload.data(),
                               payload.size(), buf, sizeof(buf));

    REQUIRE(n > 0);
    return std::vector<uint8_t>(buf, buf + n);
}

void putU16(std::vector<uint8_t> &p, uint16_t tag, uint16_t v)
{
    p.push_back((uint8_t)(tag & 0xFF));
    p.push_back((uint8_t)(tag >> 8));
    p.push_back(2);
    p.push_back(0);
    p.push_back((uint8_t)(v & 0xFF));
    p.push_back((uint8_t)(v >> 8));
}

void putU32(std::vector<uint8_t> &p, uint16_t tag, uint32_t v)
{
    p.push_back((uint8_t)(tag & 0xFF));
    p.push_back((uint8_t)(tag >> 8));
    p.push_back(4);
    p.push_back(0);
    for (int i = 0; i < 4; i++)
        p.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

/* A HELLO a client would send: the version it speaks and what it implements. */
std::vector<uint8_t> helloFrame(uint8_t seq = 1)
{
    std::vector<uint8_t> p;

    putU16(p, PC_TAG_VERSION, PC_PROTO_VERSION);
    putU32(p, PC_TAG_CAPS, PC_CAP_EVENTS);
    return encode(PC_MSG_HELLO, seq, p);
}

/* ------------------------------------------------------------------------- */
/* The simulated transport                                                    */
/* ------------------------------------------------------------------------- */

/*
 * One pass per millisecond of platform/targets/c62/pc_proto_c62.c:
 *
 *   1. sample the pad into the detector;
 *   2. on the press edge, drop the half frame the parser had;
 *   3. while the switch is held, drain the bytes and count them instead of
 *      parsing them;
 *   4. on the release edge, drop again (the release itself is a framing mess)
 *      and go back to parsing.
 */
struct Sim
{
    pcProto_t proto;
    portPttState_t ptt;
    uint32_t now = 0;
    uint32_t lost = 0;   /**< bytes thrown away, as the transport counts them */
    bool held = false;
    std::vector<uint8_t> line; /**< bytes the UART would hand over, in order */
    size_t readIdx = 0;

    Sim()
    {
        port_ptt_reset(&ptt);
        pc_proto_init(&proto, &sink);
    }

    void step(bool low)
    {
        now++;

        const bool changed = port_ptt_update(&ptt, low, now);

        held = port_ptt_pressed(&ptt);

        if (changed && !held)
            pc_proto_rxResync(&proto);
        if (changed && held)
            pc_proto_rxResync(&proto);

        if (held) {
            uint32_t dropped = (uint32_t)(line.size() - readIdx);
            readIdx = line.size();
            lost += dropped;
            port_ptt_noteLost(&ptt, dropped);
        } else if (readIdx < line.size()) {
            pc_proto_rx(&proto, line.data() + readIdx, line.size() - readIdx);
            readIdx = line.size();
        }
    }

    /** Idle for ms with the line high. */
    void idle(uint32_t ms, bool low = false)
    {
        for (uint32_t i = 0; i < ms; i++)
            step(low);
    }

    /** A press: the line is low for ms, the switch grounding it. */
    void press(uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; i++)
            step(true);
    }

    void send(const std::vector<uint8_t> &bytes)
    {
        line.insert(line.end(), bytes.begin(), bytes.end());
    }
};

} // namespace

/* ------------------------------------------------------------------------- */
/* The discriminator                                                         */
/* ------------------------------------------------------------------------- */

TEST_CASE("A press is a low level longer than 30 ms", "[port]")
{
    portPttState_t s;

    /* 50 ms of a finger on the switch: one press, declared at the threshold */
    port_ptt_reset(&s);
    bool declared = false;
    uint32_t declaredAt = 0;
    for (uint32_t t = 0; t < 50; t++) {
        if (port_ptt_update(&s, true, t)) {
            declared = true;
            declaredAt = t;
        }
    }
    REQUIRE(declared);
    REQUIRE(declaredAt == PORT_PTT_HOLD_MS + 1); /* 31: from the first low sample */
    REQUIRE(port_ptt_pressed(&s));
    REQUIRE(s.presses == 1);

    /* Release is immediate, and the duration is measured from the edge */
    REQUIRE(port_ptt_update(&s, false, 50));
    REQUIRE_FALSE(port_ptt_pressed(&s));
    REQUIRE(s.lastPressMs == 50);

    SECTION("Exactly the threshold is not a press")
    {
        port_ptt_reset(&s);
        for (uint32_t t = 1; t <= PORT_PTT_HOLD_MS; t++)
            REQUIRE_FALSE(port_ptt_update(&s, true, t));
        REQUIRE_FALSE(port_ptt_pressed(&s));
        REQUIRE_FALSE(port_ptt_update(&s, false, PORT_PTT_HOLD_MS + 1));
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches == 1);
    }

    SECTION("A press held across a long time is declared once")
    {
        port_ptt_reset(&s);
        uint16_t edges = 0;
        for (uint32_t t = 1; t <= 5000; t++) {
            if (port_ptt_update(&s, true, t))
                edges++;
        }
        REQUIRE(edges == 1);
        REQUIRE(s.presses == 1);
    }
}

TEST_CASE("A byte and a short break are not a press", "[port]")
{
    portPttState_t s;

    /*
     * 0x00 at 115200 baud: start bit plus eight zero data bits is 78.1 us of
     * low, then the stop bit.  A 1 ms sampler can catch it once per byte and
     * never twice, so a stream of them can never satisfy the 30 ms rule -- and
     * that is the whole point: the client is allowed to talk while somebody
     * rests a finger near the switch.
     */
    port_ptt_reset(&s);
    for (uint32_t t = 1; t <= 2000; t++) {
        /* one byte every 87 us is 11 bytes per millisecond: the line is low
         * most of the time, but never continuously for a whole millisecond in a
         * way that repeats without a stop bit.  Model the worst case: sampled
         * low on the millisecond, high on the next. */
        port_ptt_update(&s, (t % 2) == 0, t);
    }
    REQUIRE(s.presses == 0);
    REQUIRE_FALSE(port_ptt_pressed(&s));

    SECTION("A 1 ms break")
    {
        port_ptt_reset(&s);
        port_ptt_update(&s, true, 10);
        port_ptt_update(&s, false, 11);
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches == 1);
    }

    SECTION("A 3 ms break, as a host produces when it hangs up")
    {
        port_ptt_reset(&s);
        for (uint32_t t = 1; t <= 3; t++)
            port_ptt_update(&s, true, t);
        port_ptt_update(&s, false, 4);
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches == 1);
    }

    SECTION("A bouncing contact shorter than the threshold")
    {
        port_ptt_reset(&s);
        for (uint32_t t = 1; t <= 40; t++)
            port_ptt_update(&s, (t % 5) == 0, t); /* 1 ms low every 5 */
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches > 0);
    }
}

/* ------------------------------------------------------------------------- */
/* Auto: the two of them on one pad                                          */
/* ------------------------------------------------------------------------- */

TEST_CASE("Auto: HELLO is answered before, between and after a press", "[port]")
{
    Sim sim;
    sim.idle(5);

    sim.send(helloFrame(1));
    sim.idle(2);
    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].id == PC_MSG_HELLO);
    REQUIRE(sim.lost == 0);

    /*
     * The operator keys the speaker-microphone while the client keeps talking:
     * the line is grounded and the client's bytes arrive into a UART that sees
     * nothing but a break.  The press is declared 31 ms in, so the bytes sent
     * before that are the ones the parser gets -- and from then on everything
     * is discarded, never parsed.
     */
    sim.press(PORT_PTT_HOLD_MS + 5); /* declared */
    REQUIRE(sim.held);
    REQUIRE(sim.ptt.presses == 1);

    sim.send(std::vector<uint8_t>(40, 0x00));
    sim.press(15);
    sim.idle(1); /* release */

    REQUIRE_FALSE(sim.held);
    REQUIRE(sim.lost == 40);
    REQUIRE(takeFrames().empty());

    /* Release, then the client asks something: still linked, answered. */
    sim.idle(5);
    sim.send(encode(PC_MSG_PING, 2));
    sim.idle(2);
    frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].id == PC_MSG_PING);
    REQUIRE(sim.lost == 40);
}

TEST_CASE("Auto: a press drops the half frame the parser was building",
       "[port]")
{
    Sim sim;
    sim.idle(2);
    sim.send(helloFrame(1));
    sim.idle(2);
    REQUIRE(takeFrames().size() == 1);

    /*
     * Half a frame, then the switch: the rest of that frame arrives during the
     * press and the tail after it.  Joining the two halves would be a frame
     * nobody sent, so the parser must be resynced and refuse the join.
     */
    auto frame = encode(PC_MSG_PING, 7);
    std::vector<uint8_t> head(frame.begin(), frame.begin() + frame.size() / 2);
    std::vector<uint8_t> tail(frame.begin() + frame.size() / 2, frame.end());

    sim.send(head);
    sim.idle(1);
    sim.press(40);
    sim.send(tail);
    sim.idle(3);

    REQUIRE(takeFrames().empty());

    /* And the next whole frame is answered: the parser found its delimiter. */
    sim.send(encode(PC_MSG_PING, 8));
    sim.idle(2);
    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].seq == 8);
}

TEST_CASE("Auto: the counters say how much the press cost the cable", "[port]")
{
    Sim sim;
    sim.idle(2);
    sim.send(helloFrame(1));
    sim.idle(2);
    REQUIRE(takeFrames().size() == 1);

    sim.send(std::vector<uint8_t>(120, 0xAA));
    sim.press(PORT_PTT_HOLD_MS); /* exactly the threshold: not a press */
    sim.idle(5);
    const uint32_t afterShort = sim.lost;
    REQUIRE(sim.ptt.presses == 0);

    /* 120 bytes of 0xAA with no delimiter are garbage before the first END,
     * which the framing already throws away: the detector's counter stays
     * where it was, because the switch was never pressed. */
    REQUIRE(afterShort == 0);
    REQUIRE(sim.proto.stats.crcErrors == 0);

    /* A real press, with the client still sending: those bytes are lost. */
    sim.press(PORT_PTT_HOLD_MS + 15); /* 45 ms: declared at 31 */
    REQUIRE(sim.held);
    sim.send(std::vector<uint8_t>(60, 0x00));
    sim.press(5);
    sim.idle(2);

    REQUIRE(sim.ptt.presses == 1);
    REQUIRE(sim.lost == 60);
    /* The detector reports the press with its real length, from the edge. */
    REQUIRE(sim.ptt.lastPressMs == 50);
}

/* ------------------------------------------------------------------------- */
/* The three modes                                                           */
/* ------------------------------------------------------------------------- */

TEST_CASE("The three port modes mean what the menu says", "[port]")
{
    SECTION("Auto: UART and the sustained-low rule")
    {
        const portPttPlan_t p = port_ptt_plan(PORT_MODE_AUTO);

        REQUIRE(p.padIsUart);
        REQUIRE(p.useProtocol);
        REQUIRE(p.useDetector);
        REQUIRE_FALSE(p.useButton);

        /* The keying comes from the detector, never from the raw level: the raw
         * level is low for the start bit of every byte. */
        REQUIRE_FALSE(port_ptt_keyed(PORT_MODE_AUTO, false, true));
        REQUIRE(port_ptt_keyed(PORT_MODE_AUTO, true, false));
    }

    SECTION("Always: the cable owns the pad, the switch is not read")
    {
        const portPttPlan_t p = port_ptt_plan(PORT_MODE_ALWAYS);

        REQUIRE(p.padIsUart);
        REQUIRE(p.useProtocol);
        REQUIRE_FALSE(p.useDetector);
        REQUIRE_FALSE(p.useButton);

        /* However the line looks, this mode never keys from it -- that is the
         * behaviour every build had before the setting, now said out loud. */
        REQUIRE_FALSE(port_ptt_keyed(PORT_MODE_ALWAYS, true, true));
        REQUIRE_FALSE(port_ptt_keyed(PORT_MODE_ALWAYS, false, true));
    }

    SECTION("Never: the switch owns the pad, the protocol is off")
    {
        const portPttPlan_t p = port_ptt_plan(PORT_MODE_NEVER);

        REQUIRE_FALSE(p.padIsUart);
        REQUIRE_FALSE(p.useProtocol);
        REQUIRE_FALSE(p.useDetector);
        REQUIRE(p.useButton);

        /* 100 % reliable: no rule, no threshold, the level is the answer. */
        REQUIRE(port_ptt_keyed(PORT_MODE_NEVER, false, true));
        REQUIRE_FALSE(port_ptt_keyed(PORT_MODE_NEVER, true, false));
    }
}

TEST_CASE("The port mode survives the settings record and names its values",
       "[port]")
{
    REQUIRE(default_settings.pc_port == PORT_MODE_AUTO);
    REQUIRE(settings_portModeValid(PORT_MODE_AUTO));
    REQUIRE(settings_portModeValid(PORT_MODE_ALWAYS));
    REQUIRE(settings_portModeValid(PORT_MODE_NEVER));
    REQUIRE_FALSE(settings_portModeValid(PORT_MODE_NEVER + 1));
    REQUIRE_FALSE(settings_portModeValid(0xFF));

    REQUIRE(std::string(settings_portModeName(PORT_MODE_AUTO)) == "Auto");
    REQUIRE(std::string(settings_portModeName(PORT_MODE_ALWAYS)) == "Always");
    REQUIRE(std::string(settings_portModeName(PORT_MODE_NEVER)) == "Never");

    /* An unknown value reads as Auto, which is the safe half of both: the
     * protocol works and the switch still works with the sustained rule. */
    REQUIRE(std::string(settings_portModeName(0x7F)) == "Auto");
}

TEST_CASE("The status carries the port mode and what a press cost", "[port]")
{
    hookPortMode = PORT_MODE_NEVER;
    hookPortLost = 1234;

    pcProto_t proto;
    pc_proto_init(&proto, &sink);
    wire.clear();

    std::vector<uint8_t> req = encode(PC_MSG_STATUS_GET, 3);
    pc_proto_rx(&proto, req.data(), req.size());

    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].id == PC_MSG_STATUS_GET);

    uint8_t mode = 0xFF;
    uint32_t lost = 0;
    REQUIRE(pc_tlv_get_u8(frames[0].payload.data(), frames[0].payload.size(),
                          PC_TAG_PORT_MODE, &mode));
    REQUIRE(pc_tlv_get_u32(frames[0].payload.data(), frames[0].payload.size(),
                           PC_TAG_PORT_LOST, &lost));
    REQUIRE(mode == PORT_MODE_NEVER);
    REQUIRE(lost == 1234);

    hookPortMode = PORT_MODE_AUTO;
    hookPortLost = 0;
}
