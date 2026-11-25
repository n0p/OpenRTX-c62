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
 *  - a finger on the switch (50 ms of a held line) is a press, a clean byte is
 *    not (0x00 at 115200 baud included: it has a stop bit), and neither is a
 *    short break;
 *  - a line that is already low when the detector starts is *unknown*, not
 *    pressed -- the radio does not key itself and the client is answered;
 *  - in "Auto" HELLO frames before, between and after a press are answered, the
 *    bytes that arrive while the switch is held are discarded and counted, the
 *    half frame the parser had at that moment is dropped, and the session
 *    survives;
 *  - the three modes mean what the menu says they mean, and a change of mode
 *    cannot carry a press across it.
 *
 * The line is modelled the way the transport sees it in
 * platform/targets/c62/pc_proto_c62.c: one pass per millisecond, and per pass
 * the characters the receiver framed since the last one, each carrying whether
 * it was a break or had no stop bit (LSR.FE/BI).  That flag is the press
 * evidence; the GPIO level is not modelled because it is not used.
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

/** Characters a line held low produces per millisecond: 115200/10/1000. */
constexpr uint32_t BREAK_CHARS_PER_MS = 11;

/*
 * One pass per millisecond of platform/targets/c62/pc_proto_c62.c:
 *
 *   1. the characters the receiver framed since the last pass arrive, each one
 *      flagged if it was a break or had no stop bit;
 *   2. that flag is the press evidence and goes to the detector;
 *   3. on an edge, the half frame the parser had is dropped;
 *   4. while the switch is held, the characters are counted and never parsed;
 *   5. otherwise they go to the real parser.
 */
struct Sim
{
    pcProto_t proto;
    portPttState_t ptt;
    uint32_t now = 0;
    uint32_t lost = 0;   /**< bytes thrown away, as the transport counts them */
    bool held = false;
    std::vector<uint8_t> rx; /**< characters framed since the last pass     */
    uint32_t bad = 0;        /**< ... of which this many were break/no stop */

    Sim()
    {
        port_ptt_reset(&ptt);
        pc_proto_init(&proto, &sink);
    }

    void step()
    {
        now++;

        const bool changed = port_ptt_update(&ptt, bad > 0, now);

        held = port_ptt_pressed(&ptt);

        /* Both edges: what the parser had on either side of the switch is not
         * a frame anybody sent. */
        if (changed)
            pc_proto_rxResync(&proto);

        if (held) {
            lost += (uint32_t)rx.size();
            port_ptt_noteLost(&ptt, (uint32_t)rx.size());
        } else if (!rx.empty()) {
            pc_proto_rx(&proto, rx.data(), rx.size());
        }

        rx.clear();
        bad = 0;
    }

    /** One character as the receiver framed it. */
    void ch(uint8_t b, bool noStopBit = false)
    {
        rx.push_back(b);
        if (noStopBit)
            bad++;
    }

    /** Bytes from the client: clean characters, they always have a stop bit. */
    void send(const std::vector<uint8_t> &bytes)
    {
        for (uint8_t b : bytes)
            ch(b, false);
    }

    /** Clean 0x00 bytes: low line, but framed, so not evidence of anything. */
    void zeroes(uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
            ch(0x00, false);
    }

    /** The switch grounding the line for ms: breaks at the character rate. */
    void press(uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; i++) {
            for (uint32_t k = 0; k < BREAK_CHARS_PER_MS; k++)
                ch(0x00, true);
            step();
        }
    }

    /** The line idle for ms: no characters at all. */
    void idle(uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; i++)
            step();
    }
};

} // namespace

/* ------------------------------------------------------------------------- */
/* The discriminator                                                         */
/* ------------------------------------------------------------------------- */

TEST_CASE("A press is a held line longer than 30 ms", "[port]")
{
    portPttState_t s;

    /* Armed by one released sample: the line has to be seen high first. */
    port_ptt_reset(&s);
    REQUIRE_FALSE(port_ptt_armed(&s));
    REQUIRE_FALSE(port_ptt_update(&s, false, 1));
    REQUIRE(port_ptt_armed(&s));

    /* 50 ms with the line held: one press, declared at the threshold. */
    bool declared = false;
    uint32_t declaredAt = 0;
    for (uint32_t t = 2; t <= 51; t++) {
        if (port_ptt_update(&s, true, t))
            declared = true;
        if (declared && declaredAt == 0)
            declaredAt = t;
    }
    REQUIRE(declared);
    REQUIRE(declaredAt == 2 + PORT_PTT_HOLD_MS + 1); /* 33: 31 ms from the edge */
    REQUIRE(port_ptt_pressed(&s));
    REQUIRE(s.presses == 1);
    REQUIRE_FALSE(s.unknown);

    /* Release is immediate, and the duration is measured from the edge. */
    REQUIRE(port_ptt_update(&s, false, 52));
    REQUIRE_FALSE(port_ptt_pressed(&s));
    REQUIRE(s.lastPressMs == 50);

    SECTION("Exactly the threshold is not a press")
    {
        port_ptt_reset(&s);
        port_ptt_update(&s, false, 1); /* armed */
        for (uint32_t t = 2; t <= PORT_PTT_HOLD_MS + 1; t++)
            REQUIRE_FALSE(port_ptt_update(&s, true, t));
        REQUIRE_FALSE(port_ptt_pressed(&s));
        REQUIRE_FALSE(port_ptt_update(&s, false, PORT_PTT_HOLD_MS + 2));
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches == 1);
    }

    SECTION("A press held across a long time is declared once")
    {
        port_ptt_reset(&s);
        port_ptt_update(&s, false, 1); /* armed */
        uint16_t edges = 0;
        for (uint32_t t = 2; t <= 5001; t++) {
            if (port_ptt_update(&s, true, t))
                edges++;
        }
        REQUIRE(edges == 1);
        REQUIRE(s.presses == 1);
    }
}

TEST_CASE("A line that is already low at start-up is not a press", "[port]")
{
    /*
     * This is the hardware failure the GPIO-based detector had: the pad reads
     * low from the first sample (a stuck input, a switch held during boot, a
     * short), and a level with no release behind it keyed the radio and made
     * the transport throw away every byte the client sent.
     */
    portPttState_t s;

    port_ptt_reset(&s);
    for (uint32_t t = 1; t <= 2000; t++) {
        REQUIRE_FALSE(port_ptt_update(&s, true, t));
        REQUIRE_FALSE(port_ptt_pressed(&s));
    }
    REQUIRE(s.presses == 0);
    REQUIRE_FALSE(port_ptt_armed(&s));
    REQUIRE(s.unknown);
    /* Not even a glitch: nothing was pressed, nothing was released. */
    REQUIRE(s.glitches == 0);

    /* ...and while it stays like that, a press is still not declared: the
     * 30 ms are only counted from the first sample of an armed hold. */
    for (uint32_t t = 2001; t <= 2500; t++)
        port_ptt_update(&s, true, t);
    REQUIRE(s.presses == 0);

    /* One released sample arms it, and the next hold is a press. */
    REQUIRE_FALSE(port_ptt_update(&s, false, 2501));
    REQUIRE_FALSE(s.unknown);
    for (uint32_t t = 2502; t <= 2552; t++)
        port_ptt_update(&s, true, t);
    REQUIRE(port_ptt_pressed(&s));
    REQUIRE(s.presses == 1);
}

TEST_CASE("A clean byte and a short break are not a press", "[port]")
{
    /*
     * The classifier the transport applies to every character it takes.  Only
     * the receiver's own line status counts: a stop bit that was sampled low, or
     * a break.  The value of the byte never enters the decision, and a 0x00 is
     * the case that matters -- it holds the line low 78 us out of 87, but it is
     * a start bit, eight zeroes and a stop bit, and it frames cleanly.
     */
    REQUIRE_FALSE(port_ptt_held_char(false, false)); /* the 0x00 byte */
    REQUIRE(port_ptt_held_char(true, false));        /* no stop bit    */
    REQUIRE(port_ptt_held_char(false, true));        /* break          */
    REQUIRE(port_ptt_held_char(true, true));

    portPttState_t s;

    /*
     * ...so a flood of 0x00 at 115200 baud produces no evidence at all, and not
     * even a glitch: the oldest hole in the sustained-level rule (write 345
     * zeroes and the radio keys) is closed.
     */
    Sim sim;
    sim.idle(2);
    sim.zeroes(120);
    sim.idle(2000);
    REQUIRE(sim.ptt.presses == 0);
    REQUIRE(sim.ptt.glitches == 0);
    REQUIRE_FALSE(port_ptt_pressed(&sim.ptt));
    REQUIRE(sim.lost == 0);

    SECTION("A 1 ms break")
    {
        port_ptt_reset(&s);
        port_ptt_update(&s, false, 1);
        port_ptt_update(&s, true, 10);
        port_ptt_update(&s, false, 11);
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches == 1);
    }

    SECTION("A 3 ms break, as a host produces when it hangs up")
    {
        port_ptt_reset(&s);
        port_ptt_update(&s, false, 1);
        for (uint32_t t = 2; t <= 4; t++)
            port_ptt_update(&s, true, t);
        port_ptt_update(&s, false, 5);
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches == 1);
    }

    SECTION("A bouncing contact shorter than the threshold")
    {
        port_ptt_reset(&s);
        for (uint32_t t = 1; t <= 40; t++)
            port_ptt_update(&s, (t % 5) == 0, t); /* 1 ms held every 5 */
        REQUIRE(s.presses == 0);
        REQUIRE(s.glitches > 0);
    }
}

/* ------------------------------------------------------------------------- */
/* Auto: the two of them on one pad                                          */
/* ------------------------------------------------------------------------- */

TEST_CASE("Auto: the boot line reads low and the protocol still talks", "[port]")
{
    /*
     * The hardware case: from the first sample the line looks held (broken
     * cable, switch held at power-on, a pad with no pull-up).  The detector
     * must not key, and -- the other half of the same bug -- the transport must
     * keep parsing, because nothing has been pressed.
     */
    Sim sim;

    sim.press(200);
    REQUIRE_FALSE(sim.held);
    REQUIRE(sim.ptt.presses == 0);
    REQUIRE(sim.ptt.unknown);

    sim.send(helloFrame(1));
    sim.idle(3);

    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].id == PC_MSG_HELLO);
    REQUIRE_FALSE(sim.held);
    REQUIRE(sim.lost == 0);
}

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
     * the line is grounded and the client's bytes arrive into a UART that
     * frames nothing but breaks.  The press is declared 31 ms in, so the
     * characters before that are the ones the parser gets -- and from then on
     * everything is discarded, never parsed.
     */
    sim.press(PORT_PTT_HOLD_MS + 5); /* declared */
    REQUIRE(sim.held);
    REQUIRE(sim.ptt.presses == 1);

    const uint32_t lostAtPress = sim.lost;

    /* The client keeps writing into a grounded line: those characters are
     * breaks, and they are thrown away rather than parsed. */
    sim.press(15);
    sim.idle(1); /* release */

    REQUIRE_FALSE(sim.held);
    REQUIRE(sim.lost > lostAtPress);
    REQUIRE(takeFrames().empty());

    /* Release, then the client asks something: still linked, answered. */
    sim.idle(5);
    sim.send(encode(PC_MSG_PING, 2));
    sim.idle(2);
    frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].id == PC_MSG_PING);
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

    /* Something that is not a frame, and a hold exactly at the threshold. */
    sim.zeroes(120);
    sim.idle(1);
    sim.press(PORT_PTT_HOLD_MS); /* exactly the threshold: not a press */
    sim.idle(5);
    REQUIRE(sim.ptt.presses == 0);
    REQUIRE_FALSE(sim.held);

    /* 120 clean zeroes are the framing's business, and the switch was never
     * pressed, so the detector counts neither. */
    REQUIRE(sim.lost == 0);
    REQUIRE(sim.ptt.glitches == 1); /* ... except the 30 ms hold that ended */

    /* A real press, with the client still writing: those bytes are lost. */
    sim.press(PORT_PTT_HOLD_MS + 15); /* 45 ms: declared at 31 */
    REQUIRE(sim.held);

    const uint32_t before = sim.lost;
    sim.press(5);
    sim.idle(2);

    REQUIRE(sim.ptt.presses == 1);
    REQUIRE(sim.lost > before);
    REQUIRE(sim.ptt.lost == sim.lost);
    /* The detector reports the press with its real length, from the edge. */
    REQUIRE(sim.ptt.lastPressMs == 50);
}

/* ------------------------------------------------------------------------- */
/* The three modes                                                           */
/* ------------------------------------------------------------------------- */

TEST_CASE("The three port modes mean what the menu says", "[port]")
{
    SECTION("Auto: UART and the sustained-break rule")
    {
        const portPttPlan_t p = port_ptt_plan(PORT_MODE_AUTO);

        REQUIRE(p.padIsUart);
        REQUIRE(p.useProtocol);
        REQUIRE(p.useDetector);
        REQUIRE_FALSE(p.useButton);

        /* The keying comes from the detector, never from the raw level: with
         * the pad on UART2 RXD that level is not even the line's. */
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

TEST_CASE("A change of mode cannot carry a press across it", "[port]")
{
    /*
     * c62_portPadToUart() resets the detector whenever the pad changes owner,
     * which is the same discipline the key gate applies to a mode change: the
     * new owner has to see the line released before anything may key.
     */
    portPttState_t s;

    SECTION("Auto -> Always: the detector is out of the path")
    {
        port_ptt_reset(&s);
        port_ptt_update(&s, false, 1);
        for (uint32_t t = 2; t <= 60; t++)
            port_ptt_update(&s, true, t);
        REQUIRE(port_ptt_pressed(&s)); /* keyed in Auto */

        const portPttPlan_t plan = port_ptt_plan(PORT_MODE_ALWAYS);
        REQUIRE_FALSE(plan.useDetector);
        /* Even with the detector saying pressed, Always does not key. */
        REQUIRE_FALSE(port_ptt_keyed(PORT_MODE_ALWAYS, port_ptt_pressed(&s),
                                     false));
    }

    SECTION("Always -> Auto: the pad changes owner and the hold starts over")
    {
        port_ptt_reset(&s); /* what the pad change does */
        REQUIRE_FALSE(port_ptt_armed(&s));

        /* The switch is still held while the mode changes: unknown, not a
         * press, however long it lasts. */
        for (uint32_t t = 1; t <= 200; t++)
            REQUIRE_FALSE(port_ptt_update(&s, true, t));
        REQUIRE(s.unknown);

        /* Released, then held again: the first press of the new mode. */
        port_ptt_update(&s, false, 201);
        for (uint32_t t = 202; t <= 250; t++)
            port_ptt_update(&s, true, t);
        REQUIRE(port_ptt_pressed(&s));
        REQUIRE(port_ptt_keyed(PORT_MODE_AUTO, port_ptt_pressed(&s), false));
    }

    SECTION("Auto -> Never: the level is the answer, and it still needs a "
            "release before it keys")
    {
        port_ptt_reset(&s);
        port_ptt_update(&s, false, 1);

        const portPttPlan_t plan = port_ptt_plan(PORT_MODE_NEVER);
        REQUIRE(plan.useButton);
        REQUIRE(port_ptt_keyed(PORT_MODE_NEVER, false, true));  /* grounded */
        REQUIRE_FALSE(port_ptt_keyed(PORT_MODE_NEVER, false, false));
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
