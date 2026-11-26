/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the binary PC protocol (core/pc_proto.h, docs/PC_PROTO.md).
 *
 * Everything is exercised against the real stores: the channel bank with the
 * same fake flash the channel-store test uses (including a write that dies
 * half way), the contact book, the settings in `state` and the zones.  The
 * platform hooks are the ones below, strong symbols over the weak defaults.
 *
 * Built twice: with -DCONFIG_C62_DEBUG_PROTO and without, so the production
 * behaviour (every debug message answered NOT_ALLOWED, nothing leaked) is a
 * test and not a promise.
 */

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/pc_proto.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/crc.h"
#include "core/messaging.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/zones.h"
}

/* ------------------------------------------------------------------------- */
/* Fake flash, with a write budget so a save can be cut in the middle.       */
/* ------------------------------------------------------------------------- */

namespace {

class FakeFlash
{
public:
    explicit FakeFlash(uint32_t size = CHSTORE_PARTITION_SIZE)
        : mem(size, 0xFF), budget(-1)
    {}

    void cutAfter(int64_t bytes) { budget = bytes; }
    void heal() { budget = -1; }

    int read(uint32_t off, void *buf, size_t len)
    {
        if (off + len > mem.size())
            return -1;
        std::memcpy(buf, &mem[off], len);
        return 0;
    }

    int erase(uint32_t off, size_t len)
    {
        if (off + len > mem.size())
            return -1;
        std::memset(&mem[off], 0xFF, len);
        return 0;
    }

    int write(uint32_t off, const void *buf, size_t len)
    {
        if (off + len > mem.size())
            return -1;
        if (budget >= 0) {
            if ((int64_t)len > budget) {
                /* A dying flash: the bytes that fit land, the rest are lost. */
                std::memcpy(&mem[off], buf, (size_t)budget);
                budget = 0;
                return -1;
            }
            budget -= (int64_t)len;
        }
        std::memcpy(&mem[off], buf, len);
        return 0;
    }

    std::vector<uint8_t> mem;
    int64_t budget;
};

int flashRead(void *ctx, uint32_t off, void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->read(off, buf, len);
}
int flashErase(void *ctx, uint32_t off, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->erase(off, len);
}
int flashWrite(void *ctx, uint32_t off, const void *buf, size_t len)
{
    return static_cast<FakeFlash *>(ctx)->write(off, buf, len);
}

FakeFlash flash;

const struct chstorePort flashPort = {
    .read = flashRead, .erase = flashErase, .write = flashWrite,
    .ctx = &flash, .size = CHSTORE_PARTITION_SIZE,
    .sector = CHSTORE_SECTOR_SIZE
};

/* ------------------------------------------------------------------------- */
/* Port sink and frame parsing                                              */
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
    uint8_t flags;
    std::vector<uint8_t> payload;
};

/** Split the wire bytes into frames, exactly as a client has to. */
std::vector<Frame> takeFrames()
{
    std::vector<Frame> frames;
    std::vector<uint8_t> body;
    bool inFrame = false;
    bool esc = false;
    bool bad = false;

    for (size_t i = 0; i < wire.size(); i++) {
        uint8_t b = wire[i];
        if (b == PC_PROTO_END) {
            if (inFrame && !bad && body.size() >= 10) {
                uint16_t plen = (uint16_t)(body[6] | (body[7] << 8));
                uint16_t crc = (uint16_t)(body[body.size() - 2] |
                                          (body[body.size() - 1] << 8));
                if (body[0] == PC_PROTO_FRAME_VERSION &&
                    plen == body.size() - 10 &&
                    crc_ccitt_update(0xFFFF, body.data(), body.size() - 2) == crc) {
                    Frame f;
                    f.type = body[1];
                    f.seq = body[2];
                    f.id = (uint16_t)(body[3] | (body[4] << 8));
                    f.flags = body[5];
                    f.payload.assign(body.begin() + 8, body.end() - 2);
                    frames.push_back(f);
                }
            }
            body.clear();
            inFrame = true;
            esc = false;
            bad = false;
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

/* TLV building, the same bytes the CLI sends. */
void putU8(std::vector<uint8_t> &p, uint16_t tag, uint8_t v)
{
    p.push_back(tag & 0xFF);
    p.push_back(tag >> 8);
    p.push_back(1);
    p.push_back(0);
    p.push_back(v);
}

void putU16(std::vector<uint8_t> &p, uint16_t tag, uint16_t v)
{
    p.push_back(tag & 0xFF);
    p.push_back(tag >> 8);
    p.push_back(2);
    p.push_back(0);
    p.push_back(v & 0xFF);
    p.push_back(v >> 8);
}

void putU32(std::vector<uint8_t> &p, uint16_t tag, uint32_t v)
{
    p.push_back(tag & 0xFF);
    p.push_back(tag >> 8);
    p.push_back(4);
    p.push_back(0);
    for (int i = 0; i < 4; i++)
        p.push_back((v >> (8 * i)) & 0xFF);
}

void putRaw(std::vector<uint8_t> &p, uint16_t tag, const void *data,
            size_t len)
{
    p.push_back(tag & 0xFF);
    p.push_back(tag >> 8);
    p.push_back((uint8_t)(len & 0xFF));
    p.push_back((uint8_t)(len >> 8));
    const uint8_t *b = static_cast<const uint8_t *>(data);
    p.insert(p.end(), b, b + len);
}

void putStr(std::vector<uint8_t> &p, uint16_t tag, const std::string &s)
{
    putRaw(p, tag, s.data(), s.size());
}

bool getU8(const std::vector<uint8_t> &p, uint16_t tag, uint8_t &out)
{
    return pc_tlv_get_u8(p.data(), p.size(), tag, &out);
}
bool getU16(const std::vector<uint8_t> &p, uint16_t tag, uint16_t &out)
{
    return pc_tlv_get_u16(p.data(), p.size(), tag, &out);
}
bool getU32(const std::vector<uint8_t> &p, uint16_t tag, uint32_t &out)
{
    return pc_tlv_get_u32(p.data(), p.size(), tag, &out);
}
bool getRaw(const std::vector<uint8_t> &p, uint16_t tag,
            std::vector<uint8_t> &out)
{
    const uint8_t *v;
    uint16_t len;
    if (!pc_tlv_find(p.data(), p.size(), tag, &v, &len))
        return false;
    out.assign(v, v + len);
    return true;
}
bool getStr(const std::vector<uint8_t> &p, uint16_t tag, std::string &out)
{
    std::vector<uint8_t> raw;
    if (!getRaw(p, tag, raw))
        return false;
    out.assign(raw.begin(), raw.end());
    return true;
}

/* ------------------------------------------------------------------------- */
/* Platform hooks the protocol calls                                          */
/* ------------------------------------------------------------------------- */

uint16_t regs4819[128];
uint16_t regs1080[0x22];
bool regWriteAllowed = true;

} // namespace

/* The one address range the platform above hands over. */
uint8_t memAllowed[256];

extern "C" {

int pc_proto_platformStatus(pcProtoStatus_t *out)
{
    std::memset(out, 0, sizeof(*out));
    out->rxFreq = 145500000;
    out->txFreq = 145500000;
    out->mode = 1;
    out->rssi = -73;
    out->channelIdx = 3;
    out->batteryMv = 7400;
    out->txState = 1;
    std::strcpy(out->channelName, "R0");
    return 0;
}

const char *pc_proto_platformModel(void) { return "c62-test"; }
const char *pc_proto_platformFirmware(void) { return "test-fw"; }
const char *pc_proto_platformHardware(void) { return "rev 0"; }

/* Set by the memory test: a platform that can measure nothing (a build without
 * the heap statistics, the case of the radio) must send no memory figure. */
bool diagZeroed = false;

int pc_proto_platformDiag(pcProtoDiag_t *out)
{
    std::memset(out, 0, sizeof(*out));
    out->resetCause = 0x42;
    if (diagZeroed) {
        out->wdtResets = 2;
        out->taskCount = 1;
        std::strcpy(out->tasks[0].name, "main");
        out->tasks[0].unused = 1536;
        out->tasks[0].size = 2048;
        return 0;
    }
    out->wdtResets = 2;
    out->heapFree = 1234;
    out->heapTotal = 40960;
    out->heapKnown = 1;
    out->psramFree = 500000;
    out->psramTotal = 4194304;
    out->psramKnown = 1;
    out->flashUsed = 600000;
    out->flashTotal = 4194304;
    out->ramUsed = 200000;
    out->ramTotal = 262144;
    out->taskCount = 1;
    std::strcpy(out->tasks[0].name, "main");
    out->tasks[0].unused = 1536;
    out->tasks[0].size = 2048;
    /* Two regions, measured: the table the client prints (docs/PC_PROTO.md) */
    std::strcpy(out->regions[0].name, "SRAM");
    out->regions[0].used = 200000;
    out->regions[0].total = 262144;
    out->regions[0].known = 1;
    std::strcpy(out->regions[1].name, "DTCM");
    out->regions[1].used = 12640;
    out->regions[1].total = 16384;
    out->regions[1].known = 1;
    out->regionCount = 2;
    return 0;
}

int pc_proto_platformReg(int chip, uint8_t reg, uint16_t *value)
{
    if (chip == 0 && reg < 0x80) {
        *value = regs4819[reg];
        return 0;
    }
    if (chip == 1 && reg < 0x22) {
        *value = regs1080[reg];
        return 0;
    }
    return -1;
}

int pc_proto_platformRegWrite(int chip, uint8_t reg, uint16_t value)
{
    if (!regWriteAllowed)
        return -1;
    if (chip == 0 && reg < 0x80) {
        regs4819[reg] = value;
        return 0;
    }
    if (chip == 1 && reg < 0x22) {
        regs1080[reg] = value;
        return 0;
    }
    return -1;
}

int pc_proto_platformMem(uint32_t addr, uint8_t *buf, uint16_t len)
{
    /* One range, as a real platform would have: the buffer below. */
    extern uint8_t memAllowed[256];
    if (addr != (uint32_t)(uintptr_t)memAllowed || len > sizeof(memAllowed))
        return -1;
    std::memcpy(buf, memAllowed, len);
    return 0;
}

int pc_proto_platformCapture(int op, pcProtoCapture_t *st, uint8_t *buf,
                             uint32_t off, uint32_t count)
{
    static int16_t samples[400];
    static pcProtoCapture_t cap;

    switch (op) {
        case PC_CAP_OP_START:
            cap.capacity = st->capacity;
            cap.written = 0;
            cap.read = 0;
            cap.rate = 48000;
            cap.running = 1;
            break;
        case PC_CAP_OP_STOP:
            cap.running = 0;
            break;
        case PC_CAP_OP_STATUS:
            *st = cap;
            return 0;
        case PC_CAP_OP_READ:
            if (off + count > cap.written)
                return -1;
            std::memcpy(buf, &samples[off], (size_t)count * 2);
            return (int)count;
        default:
            return -1;
    }
    return 0;
}

int pc_proto_platformBbDump(int op, uint8_t *state)
{
    *state = (op == PC_BB_OP_STATUS) ? 0 : 1;
    return 0;
}

int pc_proto_platformTxTrace(char *buf, size_t cap)
{
    std::snprintf(buf, cap, "tx idle (none)");
    return 0;
}

} // extern "C"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------- */

namespace {

/* One radio instance per test, with fresh stores. */
struct Device
{
    pcProto_t proto;

    Device()
    {
        wire.clear();
        flash.mem.assign(CHSTORE_PARTITION_SIZE, 0xFF);
        flash.heal();
        chstore_bind(&flashPort);
        chstore_init();
        pthread_mutex_init(&state_mutex, nullptr);
        state.settings = default_settings;
        pc_proto_init(&proto, &sink);
        pc_proto_tick(&proto, 1000);
    }
};

/* Send a request and return its response. */
Frame request(Device &d, uint16_t id, uint8_t seq,
              const std::vector<uint8_t> &payload = {})
{
    uint8_t buf[512];
    size_t n = pc_proto_encode(PC_TYPE_REQUEST, seq, id, 0,
                               payload.empty() ? nullptr : payload.data(),
                               payload.size(), buf, sizeof(buf));
    REQUIRE(n > 0);
    pc_proto_rx(&d.proto, buf, n);
    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    return frames[0];
}

Frame hello(Device &d, uint16_t version = PC_PROTO_VERSION,
            uint32_t caps = PC_CAP_EVENTS)
{
    std::vector<uint8_t> p;
    putU16(p, PC_TAG_VERSION, version);
    putU32(p, PC_TAG_CAPS, caps);
    return request(d, PC_MSG_HELLO, 1, p);
}

chstore_channel_t makeChannel(const char *name, uint32_t rx)
{
    chstore_channel_t ch;
    std::memset(&ch, 0, sizeof(ch));
    std::snprintf(ch.name, sizeof(ch.name), "%s", name);
    ch.rx_freq = rx;
    ch.tx_freq = rx;
    ch.power = 1000;
    ch.mode = 1;
    ch.bandwidth = 0;
    return ch;
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Framing                                                                  */
/* ------------------------------------------------------------------------- */

TEST_CASE("Framing: escapes, delimiters and CRC round trip", "[pcproto]")
{
    /* Bytes that collide with the framing: END inside the payload, ESC, and a
     * run of both. */
    std::vector<uint8_t> payload;
    putU32(payload, PC_TAG_UPTIME_MS, 0x00C0DB00);
    putStr(payload, PC_TAG_TEXT, std::string("\xc0\xdb\xc0\xdb\xc0", 5));

    uint8_t buf[256];
    size_t n = pc_proto_encode(PC_TYPE_REQUEST, 7, PC_MSG_PING, 0,
                              payload.data(), payload.size(), buf, sizeof(buf));
    REQUIRE(n > 0);

    /* The wire must carry no END between the two that delimit the frame. */
    for (size_t i = 1; i + 1 < n; i++)
        REQUIRE(buf[i] != PC_PROTO_END);
    REQUIRE(buf[0] == PC_PROTO_END);
    REQUIRE(buf[n - 1] == PC_PROTO_END);

    wire.assign(buf, buf + n);
    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].id == PC_MSG_PING);
    REQUIRE(frames[0].payload == payload);
}

TEST_CASE("Framing: garbage and a truncated frame resynchronise", "[pcproto]")
{
    Device d;
    REQUIRE(hello(d).id == PC_MSG_HELLO);
    takeFrames();

    /* Boot noise, a frame cut in the middle, more noise, then a good PING. */
    const uint8_t junk[] = { 0x41, 0x00, 0xFF, 0x7E, 0xC0 };
    pc_proto_rx(&d.proto, junk, sizeof(junk));

    uint8_t buf[256];
    size_t n = pc_proto_encode(PC_TYPE_REQUEST, 9, PC_MSG_PING, 0, nullptr, 0,
                               buf, sizeof(buf));
    REQUIRE(n > 6);
    pc_proto_rx(&d.proto, buf, n / 2); /* cut before the closing END */
    pc_proto_rx(&d.proto, junk, sizeof(junk));

    REQUIRE(d.proto.stats.crcErrors + d.proto.stats.formErrors > 0);

    Frame f = request(d, PC_MSG_PING, 10);
    REQUIRE(f.id == PC_MSG_PING);
    REQUIRE(f.seq == 10);
    REQUIRE(d.proto.stats.frames >= 2);
}

TEST_CASE("Framing: a corrupted CRC is dropped and counted, no reply",
          "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    uint8_t buf[64];
    size_t n = pc_proto_encode(PC_TYPE_REQUEST, 1, PC_MSG_PING, 0, nullptr, 0,
                               buf, sizeof(buf));
    /* Flip a bit inside the body: the CRC has to catch it. */
    buf[3] ^= 0x40;
    pc_proto_rx(&d.proto, buf, n);

    REQUIRE(takeFrames().empty());
    REQUIRE(d.proto.stats.crcErrors == 1);
}

/* ------------------------------------------------------------------------- */
/* Session                                                                  */
/* ------------------------------------------------------------------------- */

TEST_CASE("HELLO answers version, capabilities and store sizes", "[pcproto]")
{
    Device d;
    d.proto.stats.frames = 0;
    Frame f = hello(d);
    REQUIRE(f.type == PC_TYPE_RESPONSE);
    REQUIRE(f.id == PC_MSG_HELLO);

    uint8_t status;
    uint16_t version, chcount, chver, setver;
    uint32_t caps;
    std::string model, fw;

    REQUIRE(getU8(f.payload, PC_TAG_STATUS, status));
    REQUIRE(status == PC_OK);
    REQUIRE(getU16(f.payload, PC_TAG_VERSION, version));
    REQUIRE(version == PC_PROTO_VERSION);
    REQUIRE(getU32(f.payload, PC_TAG_CAPS, caps));
    REQUIRE((caps & PC_CAP_CHANNELS) != 0);
    REQUIRE((caps & PC_CAP_EVENTS) != 0);
    REQUIRE(getStr(f.payload, PC_TAG_MODEL, model));
    REQUIRE(model == "c62-test");
    REQUIRE(getStr(f.payload, PC_TAG_FIRMWARE, fw));
    REQUIRE(fw == "test-fw");
    REQUIRE(getU16(f.payload, PC_TAG_CHCOUNT, chcount));
    REQUIRE(chcount == 0);
    REQUIRE(getU16(f.payload, PC_TAG_CHSTORE_VER, chver));
    REQUIRE(chver == CHSTORE_VERSION);
    REQUIRE(getU16(f.payload, PC_TAG_SETTINGS_VER, setver));
    REQUIRE(setver == SETTINGS_VERSION);
    REQUIRE(pc_proto_linked(&d.proto));
}

TEST_CASE("HELLO from a client below the minimum degrades, it does not hang",
          "[pcproto]")
{
    Device d;
    Frame f = hello(d, 0);
    REQUIRE(f.id == PC_MSG_HELLO);

    uint8_t status;
    uint16_t min;
    REQUIRE(getU8(f.payload, PC_TAG_STATUS, status));
    REQUIRE(status == PC_ERR_VERSION);
    REQUIRE(getU16(f.payload, PC_TAG_VERSION_MIN, min));
    REQUIRE(min == PC_PROTO_VERSION_MIN);

    /* The identity still travels, so the client can say why it stops. */
    std::string model;
    REQUIRE(getStr(f.payload, PC_TAG_MODEL, model));

    /* And no events are sent to a client that could not understand them. */
    REQUIRE_FALSE(pc_proto_linked(&d.proto));
    takeFrames();
    pc_proto_eventChannel(&d.proto, 1, "R1");
    REQUIRE(takeFrames().empty());
}

TEST_CASE("An unknown message and a known but absent one answer differently",
          "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    Frame unknown = request(d, 0x0EEE, 3);
    REQUIRE(unknown.id == PC_MSG_ERROR);
    uint8_t err;
    uint16_t id;
    REQUIRE(getU8(unknown.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_UNKNOWN_MSG);
    REQUIRE(getU16(unknown.payload, PC_TAG_MSG_ID, id));
    REQUIRE(id == 0x0EEE);

    Frame reserved = request(d, PC_MSG_PM_TX, 4);
    REQUIRE(getU8(reserved.payload, PC_TAG_ERROR, err));
    /* No packet modem in this build: it is a reserved block, not a typo. */
    REQUIRE(err == PC_ERR_NOT_SUPPORTED);
}

TEST_CASE("Every malformed request is refused without a crash", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    std::vector<uint8_t> bad;
    putU8(bad, PC_TAG_INDEX, 0);          /* wrong width for the record */
    Frame f = request(d, PC_MSG_CHAN_GET, 5, bad);
    uint8_t err;
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_BAD_REQUEST);

    /* A TLV whose length runs past the payload. */
    std::vector<uint8_t> shortTlv = { 0x40, 0x00, 0xFF, 0x00, 0x01 };
    f = request(d, PC_MSG_CHAN_SET, 6, shortTlv);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_BAD_REQUEST);
}

/* ------------------------------------------------------------------------- */
/* Channels against the real bank                                            */
/* ------------------------------------------------------------------------- */

TEST_CASE("A channel written by the PC reads back from the store", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    chstore_channel_t ch = makeChannel("CALL1", 145500000);
    int slot = chstore_add(&ch);
    REQUIRE(slot == 0);

    std::vector<uint8_t> req;
    putU16(req, PC_TAG_INDEX, 0);
    Frame f = request(d, PC_MSG_CHAN_GET, 11, req);
    REQUIRE(f.id == PC_MSG_CHAN_GET);

    std::vector<uint8_t> rec;
    REQUIRE(getRaw(f.payload, PC_TAG_CHANNEL, rec));
    REQUIRE(rec.size() == sizeof(chstore_channel_t));
    REQUIRE(std::memcmp(rec.data(), &ch, sizeof(ch)) == 0);

    /* And the other way: what the PC writes is what the bank holds. */
    chstore_channel_t edited = makeChannel("EDITED", 433500000);
    edited.dmr_cc = 5;
    edited.dmr_ts = 1;

    std::vector<uint8_t> set;
    putU16(set, PC_TAG_INDEX, 0);
    putRaw(set, PC_TAG_CHANNEL, &edited, sizeof(edited));
    f = request(d, PC_MSG_CHAN_SET, 12, set);
    REQUIRE(f.id == PC_MSG_CHAN_SET);

    uint8_t verify;
    REQUIRE(getU8(f.payload, PC_TAG_VERIFY, verify) == false); /* record reply */
    REQUIRE(getRaw(f.payload, PC_TAG_CHANNEL, rec));
    REQUIRE(std::memcmp(rec.data(), &edited, sizeof(edited)) == 0);

    chstore_channel_t back;
    REQUIRE(chstore_get(0, &back) == 0);
    REQUIRE(std::memcmp(&back, &edited, sizeof(back)) == 0);
}

TEST_CASE("A write that the flash cuts in half is reported and the bank "
          "survives", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    chstore_channel_t first = makeChannel("GOOD", 145500000);
    REQUIRE(chstore_add(&first) == 0);
    chstore_channel_t before;
    REQUIRE(chstore_get(0, &before) == 0);

    /* The radio dies in the middle of the save. */
    flash.cutAfter(64);

    chstore_channel_t edited = makeChannel("BROKEN", 433500000);
    std::vector<uint8_t> set;
    putU16(set, PC_TAG_INDEX, 0);
    putRaw(set, PC_TAG_CHANNEL, &edited, sizeof(edited));
    Frame f = request(d, PC_MSG_CHAN_SET, 21, set);

    REQUIRE(f.id == PC_MSG_ERROR);
    uint8_t err;
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_IO);

    /* The previous bank is still the valid one: the store's ping-pong did its
     * job and the protocol did not claim success. */
    flash.heal();
    chstore_bind(&flashPort);
    chstore_init();
    chstore_channel_t after;
    REQUIRE(chstore_get(0, &after) == 0);
    REQUIRE(std::memcmp(&after, &before, sizeof(after)) == 0);
}

TEST_CASE("A block of channels reads and writes with the MORE flag", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    for (int i = 0; i < 12; i++) {
        char name[8];
        std::snprintf(name, sizeof(name), "CH%d", i);
        chstore_channel_t ch = makeChannel(name, 145000000 + 12500u * i);
        REQUIRE(chstore_add(&ch) == i);
    }

    std::vector<uint8_t> req;
    putU16(req, PC_TAG_FIRST, 0);
    putU16(req, PC_TAG_COUNT, 20); /* more than one frame holds */
    Frame f = request(d, PC_MSG_CHAN_LIST, 31, req);
    REQUIRE(f.id == PC_MSG_CHAN_LIST);
    REQUIRE((f.flags & PC_PROTO_FLAG_MORE) != 0);

    std::vector<uint8_t> list;
    REQUIRE(getRaw(f.payload, PC_TAG_CHANNEL_LIST, list));
    size_t perFrame = list.size() / sizeof(chstore_channel_t);
    REQUIRE(perFrame >= 4);
    REQUIRE(perFrame < 12);

    uint16_t got;
    REQUIRE(getU16(f.payload, PC_TAG_COUNT, got));
    REQUIRE(got == perFrame);

    /* Write them all back with one name changed, in blocks. */
    for (size_t start = 0; start < 12; start += perFrame) {
        std::vector<uint8_t> block;
        size_t n = (start + perFrame <= 12) ? perFrame : 12 - start;
        for (size_t i = 0; i < n; i++) {
            chstore_channel_t ch;
            REQUIRE(chstore_get((uint16_t)(start + i), &ch) == 0);
            std::snprintf(ch.name, sizeof(ch.name), "W%zu", start + i);
            block.insert(block.end(), (uint8_t *)&ch,
                         (uint8_t *)&ch + sizeof(ch));
        }
        std::vector<uint8_t> wr;
        putU16(wr, PC_TAG_FIRST, (uint16_t)start);
        putRaw(wr, PC_TAG_CHANNEL_LIST, block.data(), block.size());
        Frame r = request(d, PC_MSG_CHAN_WRITE_BLOCK, (uint8_t)(40 + start), wr);
        REQUIRE(r.id == PC_MSG_CHAN_WRITE_BLOCK);
        uint8_t verify;
        REQUIRE(getU8(r.payload, PC_TAG_VERIFY, verify));
        REQUIRE(verify == 1);
    }

    chstore_channel_t ch;
    REQUIRE(chstore_get(11, &ch) == 0);
    REQUIRE(std::string(ch.name) == "W11");
}

TEST_CASE("A block with one bad record writes nothing at all", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    chstore_channel_t a = makeChannel("A", 145500000);
    chstore_channel_t b = makeChannel("B", 145512500);
    REQUIRE(chstore_add(&a) == 0);
    REQUIRE(chstore_add(&b) == 1);

    chstore_channel_t edited = makeChannel("EDIT", 433500000);
    chstore_channel_t invalid = makeChannel("BAD", 5); /* 5 Hz: refused */

    std::vector<uint8_t> block;
    block.insert(block.end(), (uint8_t *)&edited,
                 (uint8_t *)&edited + sizeof(edited));
    block.insert(block.end(), (uint8_t *)&invalid,
                 (uint8_t *)&invalid + sizeof(invalid));

    std::vector<uint8_t> wr;
    putU16(wr, PC_TAG_FIRST, 0);
    putRaw(wr, PC_TAG_CHANNEL_LIST, block.data(), block.size());
    Frame r = request(d, PC_MSG_CHAN_WRITE_BLOCK, 51, wr);

    uint8_t err;
    REQUIRE(getU8(r.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_INVALID_VALUE);

    chstore_channel_t back;
    REQUIRE(chstore_get(0, &back) == 0);
    REQUIRE(std::string(back.name) == "A");
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                 */
/* ------------------------------------------------------------------------- */

TEST_CASE("Settings read back whole, with their version and CRC", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    Frame f = request(d, PC_MSG_SETTINGS_GET, 61);
    REQUIRE(f.id == PC_MSG_SETTINGS_GET);

    uint16_t version, size, crc;
    std::vector<uint8_t> raw;
    REQUIRE(getU16(f.payload, PC_TAG_SETTINGS_VER, version));
    REQUIRE(version == SETTINGS_VERSION);
    REQUIRE(getU16(f.payload, PC_TAG_SETTINGS_SIZE, size));
    REQUIRE(size == sizeof(settings_t));
    REQUIRE(getRaw(f.payload, PC_TAG_SETTINGS, raw));
    REQUIRE(raw.size() == sizeof(settings_t));
    REQUIRE(getU16(f.payload, PC_TAG_SETTINGS_CRC, crc));
    REQUIRE(crc == crc_ccitt_update(0xFFFF, raw.data(), raw.size()));
}

TEST_CASE("A settings field is written, verified and read back", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    std::vector<uint8_t> set;
    putStr(set, PC_TAG_SET_CALLSIGN, "EA5TEST");
    putU8(set, PC_TAG_SET_SQLLEVEL, 7);
    putU32(set, PC_TAG_SET_DMR_ID, 2143001);
    Frame f = request(d, PC_MSG_SETTINGS_SET, 62, set);
    REQUIRE(f.id == PC_MSG_SETTINGS_SET);

    uint8_t verify;
    REQUIRE(getU8(f.payload, PC_TAG_VERIFY, verify));
    REQUIRE(verify == 1);
    REQUIRE(std::string(state.settings.callsign) == "EA5TEST");
    REQUIRE(state.settings.sqlLevel == 7);
    REQUIRE(state.settings.dmr_id == 2143001);

    std::vector<uint8_t> get;
    putU8(get, PC_TAG_SET_CALLSIGN, 0);
    putU8(get, PC_TAG_SET_DMR_ID, 0);
    f = request(d, PC_MSG_SETTINGS_GET, 63, get);
    std::string call;
    uint32_t dmrId;
    REQUIRE(getStr(f.payload, PC_TAG_SET_CALLSIGN, call));
    REQUIRE(call == "EA5TEST");
    REQUIRE(getU32(f.payload, PC_TAG_SET_DMR_ID, dmrId));
    REQUIRE(dmrId == 2143001);
}

TEST_CASE("A settings value out of range is refused and nothing changes",
          "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    std::string before(state.settings.callsign);
    std::vector<uint8_t> set;
    putStr(set, PC_TAG_SET_CALLSIGN, "OK");
    putU8(set, PC_TAG_SET_BRIGHTNESS, 200); /* 0..100 */
    Frame f = request(d, PC_MSG_SETTINGS_SET, 64, set);

    uint8_t err;
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_INVALID_VALUE);
    REQUIRE(std::string(state.settings.callsign) == before);
}

/* ------------------------------------------------------------------------- */
/* Events                                                                   */
/* ------------------------------------------------------------------------- */

TEST_CASE("Events flow only to a client that asked for them", "[pcproto]")
{
    Device d;

    /* Before HELLO: nothing leaves. */
    pc_proto_eventChannel(&d.proto, 2, "R2");
    REQUIRE(takeFrames().empty());

    /* HELLO without the events capability: still nothing. */
    hello(d, PC_PROTO_VERSION, 0);
    takeFrames();
    pc_proto_eventChannel(&d.proto, 2, "R2");
    REQUIRE(takeFrames().empty());

    /* With it: one frame, with the channel and the live status in it. */
    hello(d, PC_PROTO_VERSION, PC_CAP_EVENTS);
    takeFrames();
    pc_proto_eventChannel(&d.proto, 2, "R2");
    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].type == PC_TYPE_EVENT);
    REQUIRE(frames[0].id == PC_EVT_CHANNEL);

    uint16_t index;
    std::string name;
    int16_t rssi;
    REQUIRE(getU16(frames[0].payload, PC_TAG_CHANNEL_IDX, index));
    REQUIRE(index == 2);
    REQUIRE(getStr(frames[0].payload, PC_TAG_CHANNEL_NAME, name));
    REQUIRE(name == "R2");
    REQUIRE(pc_tlv_get_i16(frames[0].payload.data(), frames[0].payload.size(),
                           PC_TAG_RSSI, &rssi));
    REQUIRE(rssi == -73);

    /* PTT and a message too. */
    pc_proto_eventPtt(&d.proto, 2);
    pc_proto_eventMessage(&d.proto, 1, 2143001, "hola", 3);
    frames = takeFrames();
    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].id == PC_EVT_PTT);
    REQUIRE(frames[1].id == PC_EVT_MESSAGE);
    std::string text;
    REQUIRE(getStr(frames[1].payload, PC_TAG_MSG_TEXT, text));
    REQUIRE(text == "hola");
}

TEST_CASE("A status subscription emits on its period and not before",
          "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    std::vector<uint8_t> sub;
    putU16(sub, PC_TAG_PERIOD_MS, 200);
    REQUIRE(request(d, PC_MSG_STATUS_SUBSCRIBE, 71, sub).id ==
            PC_MSG_STATUS_SUBSCRIBE);
    takeFrames();

    pc_proto_tick(&d.proto, 1100); /* 100 ms after the subscribe */
    REQUIRE(takeFrames().empty());

    pc_proto_tick(&d.proto, 1300);
    auto frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].id == PC_EVT_STATUS);

    uint8_t txState;
    uint32_t rx;
    REQUIRE(getU8(frames[0].payload, PC_TAG_TX_STATE, txState));
    REQUIRE(txState == 1);
    REQUIRE(getU32(frames[0].payload, PC_TAG_RX_FREQ, rx));
    REQUIRE(rx == 145500000);
}

/* ------------------------------------------------------------------------- */
/* Zones                                                                    */
/* ------------------------------------------------------------------------- */

TEST_CASE("A zone channel can be imported into the bank", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    Frame count = request(d, PC_MSG_ZONE_COUNT, 80);
    uint16_t zones;
    REQUIRE(getU16(count.payload, PC_TAG_ZONE_COUNT, zones));
    REQUIRE(zones == ZONE_COUNT);

    std::vector<uint8_t> req;
    putU8(req, PC_TAG_ZONE, ZONE_PMR446);
    putU16(req, PC_TAG_INDEX, 0);
    Frame fn = request(d, PC_MSG_ZONE_CHANNEL, 81, req);
    std::vector<uint8_t> zc;
    REQUIRE(getRaw(fn.payload, PC_TAG_ZONE_CHANNEL, zc));
    REQUIRE(zc.size() == 28);

    Frame imp = request(d, PC_MSG_ZONE_IMPORT, 82, req);
    REQUIRE(imp.id == PC_MSG_ZONE_IMPORT);
    std::vector<uint8_t> rec;
    REQUIRE(getRaw(imp.payload, PC_TAG_CHANNEL, rec));
    REQUIRE(chstore_count() == 1);
}

/* ------------------------------------------------------------------------- */
/* Debug namespace                                                          */
/* ------------------------------------------------------------------------- */

#ifdef CONFIG_C62_DEBUG_PROTO

TEST_CASE("Debug: diag carries every mandatory field", "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    Frame f = request(d, PC_MSG_DBG_INFO, 90);
    REQUIRE(f.id == PC_MSG_DBG_INFO);

    uint16_t version, min;
    std::string fw, model;
    uint32_t uptime, cap, cause, wdt, heap, heapTotal, psram, flash, ram;
    uint8_t agc;

    REQUIRE(getU16(f.payload, PC_TAG_VERSION, version));
    REQUIRE(version == PC_PROTO_VERSION);
    REQUIRE(getU16(f.payload, PC_TAG_VERSION_MIN, min));
    REQUIRE(getStr(f.payload, PC_TAG_FIRMWARE, fw));
    REQUIRE(fw == "test-fw");
    REQUIRE(getStr(f.payload, PC_TAG_MODEL, model));
    REQUIRE(getU32(f.payload, PC_TAG_UPTIME_MS, uptime));
    REQUIRE(uptime == 1000);
    REQUIRE(getU32(f.payload, PC_TAG_CAPS, cap));
    REQUIRE((cap & PC_CAP_DEBUG) != 0);
    REQUIRE(getU32(f.payload, PC_TAG_RESET_CAUSE, cause));
    REQUIRE(cause == 0x42);
    REQUIRE(getU32(f.payload, PC_TAG_WDT_RESETS, wdt));
    REQUIRE(wdt == 2);
    REQUIRE(getU32(f.payload, PC_TAG_HEAP_FREE, heap));
    REQUIRE(heap == 1234);
    REQUIRE(getU32(f.payload, PC_TAG_HEAP_TOTAL, heapTotal));
    REQUIRE(getU32(f.payload, PC_TAG_PSRAM_FREE, psram));
    REQUIRE(getU32(f.payload, PC_TAG_FLASH_USED, flash));
    REQUIRE(getU32(f.payload, PC_TAG_RAM_USED, ram));
    REQUIRE(ram == 200000);
    REQUIRE(getU8(f.payload, PC_TAG_AGC, agc));

    /* The stack array travels as packed records, one per task. */
    std::vector<uint8_t> stacks;
    REQUIRE(getRaw(f.payload, PC_TAG_TASK_STACK, stacks));
    REQUIRE(stacks.size() % sizeof(pcProtoTaskStack_t) == 0);
    REQUIRE(stacks.size() / sizeof(pcProtoTaskStack_t) == 1);
    pcProtoTaskStack_t task;
    std::memcpy(&task, stacks.data(), sizeof(task));
    REQUIRE(std::string(task.name) == "main");
    REQUIRE(task.unused == 1536);
}

/*
 * The memory figures, and the rule that makes them worth believing: a platform
 * that cannot measure one does not send it. The old answer said "0 of 40960
 * bytes free" for a heap it had never asked about (docs/PC_PROTO.md).
 */
TEST_CASE("Debug: diag sends memory it measured and omits memory it cannot",
          "[pcproto][memory]")
{
    Device d;
    hello(d);
    takeFrames();

    /* The platform of this harness measures everything it fills in */
    Frame f = request(d, PC_MSG_DBG_INFO, 92);
    uint32_t heap = 0;
    REQUIRE(getU32(f.payload, PC_TAG_HEAP_FREE, heap));
    REQUIRE(heap == 1234);

    std::vector<uint8_t> regions;
    REQUIRE(getRaw(f.payload, PC_TAG_MEM_REGION, regions));
    REQUIRE(regions.size() == 2 * sizeof(pcProtoRegion_t));

    pcProtoRegion_t r[2];
    std::memcpy(r, regions.data(), sizeof(r));
    REQUIRE(std::string(r[0].name) == "SRAM");
    REQUIRE(r[0].used == 200000);
    REQUIRE(r[0].total == 262144);
    REQUIRE(r[0].known == 1);
    REQUIRE(std::string(r[1].name) == "DTCM");
    REQUIRE(r[1].used == 12640);
    REQUIRE(r[1].total == 16384);

    /*
     * And now a platform that can measure nothing: no heap, no PSRAM free, no
     * region. The answer has to say nothing at all about them rather than
     * zeros, which is what a client would print as a full heap.
     */
    diagZeroed = true;
    f = request(d, PC_MSG_DBG_INFO, 93);
    REQUIRE_FALSE(getU32(f.payload, PC_TAG_HEAP_FREE, heap));
    REQUIRE_FALSE(getU32(f.payload, PC_TAG_HEAP_TOTAL, heap));
    REQUIRE_FALSE(getU32(f.payload, PC_TAG_PSRAM_FREE, heap));
    REQUIRE_FALSE(getU32(f.payload, PC_TAG_RAM_TOTAL, heap));
    REQUIRE_FALSE(getU32(f.payload, PC_TAG_FLASH_TOTAL, heap));
    REQUIRE_FALSE(getRaw(f.payload, PC_TAG_MEM_REGION, regions));

    /* The rest of the answer is still there: it is the memory that is unknown,
     * not the radio */
    REQUIRE(getU32(f.payload, PC_TAG_RESET_CAUSE, heap));
    REQUIRE(heap == 0x42);
    diagZeroed = false;
}

TEST_CASE("Debug: log subscription filters, streams and counts its drops",
          "[pcproto]")
{
    Device d;
    hello(d);
    takeFrames();

    d.proto.stats.frames = 0; /* keep the counters clean for the assertions */

    std::vector<uint8_t> sub;
    putU8(sub, PC_TAG_LEVEL, PC_LOG_INFO);
    putU16(sub, PC_TAG_RATE, 2); /* two lines a second */
    putStr(sub, PC_TAG_MODULE, "net");
    Frame f = request(d, PC_MSG_DBG_LOG_SUB, 91, sub);
    REQUIRE(f.id == PC_MSG_DBG_LOG_SUB);
    uint16_t rate;
    REQUIRE(getU16(f.payload, PC_TAG_RATE, rate));
    REQUIRE(rate == 2);
    takeFrames();

    /* Below the level: not queued at all. */
    pc_proto_logPush(&d.proto, PC_LOG_DEBUG, "net", "debug line");
    /* Another module: filtered out. */
    pc_proto_logPush(&d.proto, PC_LOG_ERROR, "audio", "not for us");
    REQUIRE(takeFrames().empty());
    /* Same instant as the subscription: no credit has accrued yet. */
    pc_proto_tick(&d.proto, 1000);
    REQUIRE(takeFrames().empty());

    /* Four lines queued -- the ring holds four -- and a limiter of two lines a
     * second: the first tick may only send two of them. */
    for (int i = 0; i < 4; i++) {
        char text[16];
        std::snprintf(text, sizeof(text), "line%d", i);
        pc_proto_logPush(&d.proto, (i & 1) ? PC_LOG_WARN : PC_LOG_INFO, "net",
                         text);
    }
    pc_proto_tick(&d.proto, 2000); /* 1 s at 2 lines/s */
    auto frames = takeFrames();
    REQUIRE(frames.size() == 2);

    uint32_t seq;
    std::string text;
    REQUIRE(getU32(frames[0].payload, PC_TAG_LOG_SEQ, seq));
    REQUIRE(seq == 3); /* the first queued line, numbered from the push */
    REQUIRE(getStr(frames[0].payload, PC_TAG_TEXT, text));
    REQUIRE(text == "line0");
    REQUIRE(getStr(frames[1].payload, PC_TAG_TEXT, text));
    REQUIRE(text == "line1");

    /* 100 ms later there is not a whole line of credit yet. */
    pc_proto_tick(&d.proto, 2100);
    REQUIRE(takeFrames().empty());

    /* 500 ms later there is. */
    pc_proto_tick(&d.proto, 2600);
    frames = takeFrames();
    REQUIRE(frames.size() == 1);
    REQUIRE(getU32(frames[0].payload, PC_TAG_LOG_SEQ, seq));
    REQUIRE(seq == 5);
    REQUIRE(getStr(frames[0].payload, PC_TAG_TEXT, text));
    REQUIRE(text == "line2");

    /* Now flood it: the ring holds four, the rest are dropped and counted, and
     * the drop is visible to the client in the frame it does get. */
    for (int i = 0; i < 10; i++)
        pc_proto_logPush(&d.proto, PC_LOG_ERROR, "net", "flood");
    REQUIRE(pc_proto_logDropped(&d.proto) > 0);

    /* The limiter never lets a backlog out in one burst: four lines of credit
     * at most, however long the gap. */
    size_t emitted = 0;
    bool sawDropped = false;
    for (uint32_t t = 3000; t <= 3600; t += 100) {
        pc_proto_tick(&d.proto, t);
        auto more = takeFrames();
        emitted += more.size();
        for (auto &fr : more) {
            uint32_t d2 = 0;
            if (getU32(fr.payload, PC_TAG_DROPPED, d2) && d2 > 0)
                sawDropped = true;
        }
    }
    REQUIRE(emitted >= 1);
    REQUIRE(emitted <= PC_PROTO_LOG_LINES);
    /* The client can see the loss in the frame it does get. */
    REQUIRE(sawDropped);
}

TEST_CASE("Debug: a register read needs no token, a write needs all three",
          "[pcproto]")
{
    Device d;
    Frame h = hello(d);
    uint32_t token = 0;
    REQUIRE(getU32(h.payload, PC_TAG_TOKEN, token));
    REQUIRE(token != 0);
    takeFrames();

    regs4819[0x30] = 0x1234;

    /* Read: chip, register, no token needed. */
    std::vector<uint8_t> rd;
    putU8(rd, PC_TAG_CHIP, 0);
    putU8(rd, PC_TAG_REG, 0x30);
    Frame f = request(d, PC_MSG_DBG_REG_READ, 92, rd);
    uint16_t value;
    REQUIRE(f.id == PC_MSG_DBG_REG_READ);
    REQUIRE(getU16(f.payload, PC_TAG_VALUE, value));
    REQUIRE(value == 0x1234);

    /* A register outside the chip's file is refused before the driver sees it. */
    std::vector<uint8_t> bad;
    putU8(bad, PC_TAG_CHIP, 0);
    putU8(bad, PC_TAG_REG, 0x90);
    f = request(d, PC_MSG_DBG_REG_READ, 93, bad);
    uint8_t err;
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_INVALID_VALUE);

    /* Write without the token: not allowed, and nothing written. */
    std::vector<uint8_t> wr;
    putU8(wr, PC_TAG_CHIP, 0);
    putU8(wr, PC_TAG_REG, 0x30);
    putU16(wr, PC_TAG_VALUE, 0x4321);
    putU8(wr, PC_TAG_CONFIRM, 1);
    f = request(d, PC_MSG_DBG_REG_WRITE, 94, wr);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_NOT_ALLOWED);
    REQUIRE(regs4819[0x30] == 0x1234);

    /* With the token but no explicit confirmation: refused too. */
    std::vector<uint8_t> wr2;
    putU8(wr2, PC_TAG_CHIP, 0);
    putU8(wr2, PC_TAG_REG, 0x30);
    putU16(wr2, PC_TAG_VALUE, 0x4321);
    putU8(wr2, PC_TAG_CONFIRM, 0);
    putU32(wr2, PC_TAG_TOKEN, token);
    f = request(d, PC_MSG_DBG_REG_WRITE, 95, wr2);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_BAD_REQUEST);

    /* Token and confirmation: allowed, and read back. */
    std::vector<uint8_t> wr3;
    putU8(wr3, PC_TAG_CHIP, 0);
    putU8(wr3, PC_TAG_REG, 0x30);
    putU16(wr3, PC_TAG_VALUE, 0x4321);
    putU8(wr3, PC_TAG_CONFIRM, 1);
    putU32(wr3, PC_TAG_TOKEN, token);
    f = request(d, PC_MSG_DBG_REG_WRITE, 96, wr3);
    REQUIRE(f.id == PC_MSG_DBG_REG_WRITE);
    uint8_t verify;
    REQUIRE(getU16(f.payload, PC_TAG_VALUE, value));
    REQUIRE(value == 0x4321);
    REQUIRE(getU8(f.payload, PC_TAG_VERIFY, verify));
    REQUIRE(verify == 1);
    REQUIRE(regs4819[0x30] == 0x4321);

    /* A register that is readable but not whitelisted for writing. */
    std::vector<uint8_t> wr4;
    putU8(wr4, PC_TAG_CHIP, 0);
    putU8(wr4, PC_TAG_REG, 0x10);
    putU16(wr4, PC_TAG_VALUE, 0x1111);
    putU8(wr4, PC_TAG_CONFIRM, 1);
    putU32(wr4, PC_TAG_TOKEN, token);
    f = request(d, PC_MSG_DBG_REG_WRITE, 97, wr4);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_INVALID_VALUE);

    /* A second HELLO rotates the token: the old one stops working. */
    hello(d);
    takeFrames();
    f = request(d, PC_MSG_DBG_REG_WRITE, 98, wr3);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_NOT_ALLOWED);
}

TEST_CASE("Debug: memory reads are bounded and range-checked", "[pcproto]")
{
    Device d;
    Frame h = hello(d);
    uint32_t token;
    REQUIRE(getU32(h.payload, PC_TAG_TOKEN, token));
    takeFrames();

    /* The platform only hands over its own buffer; anything else is refused
     * and the answer carries no data at all. */
    std::vector<uint8_t> req;
    putU32(req, PC_TAG_ADDR, 0x20000000);
    putU16(req, PC_TAG_LENGTH, 16);
    Frame f = request(d, PC_MSG_DBG_MEM_READ, 100, req);
    uint8_t err;
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    /* Without the token it is refused even earlier. */
    REQUIRE(err == PC_ERR_NOT_ALLOWED);

    std::vector<uint8_t> withToken = req;
    putU32(withToken, PC_TAG_TOKEN, token);
    f = request(d, PC_MSG_DBG_MEM_READ, 101, withToken);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_INVALID_VALUE);
    std::vector<uint8_t> data;
    REQUIRE_FALSE(getRaw(f.payload, PC_TAG_DATA, data));

    /* The length is clamped to the hard ceiling, not trusted. */
    std::vector<uint8_t> big;
    putU32(big, PC_TAG_ADDR, (uint32_t)(uintptr_t)memAllowed);
    putU16(big, PC_TAG_LENGTH, 200);
    putU32(big, PC_TAG_TOKEN, token);
    f = request(d, PC_MSG_DBG_MEM_READ, 102, big);
    REQUIRE(f.id == PC_MSG_DBG_MEM_READ);
    uint16_t len;
    REQUIRE(getU16(f.payload, PC_TAG_LENGTH, len));
    REQUIRE(len == 128);
    REQUIRE(getRaw(f.payload, PC_TAG_DATA, data));
    REQUIRE(data.size() == 128);
}

TEST_CASE("Debug: capture, baseband dump and the TX trace answer", "[pcproto]")
{
    Device d;
    Frame h = hello(d);
    uint32_t token;
    REQUIRE(getU32(h.payload, PC_TAG_TOKEN, token));
    takeFrames();

    std::vector<uint8_t> start;
    putU16(start, PC_TAG_LENGTH, 400);
    Frame f = request(d, PC_MSG_DBG_CAP_START, 110, start);
    REQUIRE(f.id == PC_MSG_DBG_CAP_START);
    uint8_t running;
    REQUIRE(getU8(f.payload, PC_TAG_RUNNING, running));
    REQUIRE(running == 1);

    /* Too long a capture is refused rather than allocated. */
    std::vector<uint8_t> tooBig;
    putU16(tooBig, PC_TAG_LENGTH, 60000);
    f = request(d, PC_MSG_DBG_CAP_START, 111, tooBig);
    uint8_t err;
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_INVALID_VALUE);

    f = request(d, PC_MSG_DBG_CAP_STOP, 112);
    REQUIRE(f.id == PC_MSG_DBG_CAP_STOP);

    /* Reading needs the token; the offset is bounded by what was recorded. */
    f = request(d, PC_MSG_DBG_CAP_READ, 113);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_NOT_ALLOWED);

    std::vector<uint8_t> readReq;
    putU32(readReq, PC_TAG_OFFSET, 0);
    putU16(readReq, PC_TAG_LENGTH, 32);
    putU32(readReq, PC_TAG_TOKEN, token);
    f = request(d, PC_MSG_DBG_CAP_READ, 114, readReq);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_BAD_INDEX); /* nothing recorded */

    /* The dump reports its state; arming it needs the token. */
    f = request(d, PC_MSG_DBG_BB, 115);
    uint8_t bbState;
    REQUIRE(f.id == PC_MSG_DBG_BB);
    REQUIRE(getU8(f.payload, PC_TAG_BB_STATE, bbState));

    std::vector<uint8_t> arm;
    putU8(arm, PC_TAG_BB_STATE, PC_BB_OP_ARM);
    f = request(d, PC_MSG_DBG_BB, 116, arm);
    REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
    REQUIRE(err == PC_ERR_NOT_ALLOWED);

    f = request(d, PC_MSG_DBG_TX_TRACE, 117);
    std::string trace;
    REQUIRE(f.id == PC_MSG_DBG_TX_TRACE);
    REQUIRE(getStr(f.payload, PC_TAG_TEXT, trace));
    REQUIRE(trace.find("tx idle") == 0);
}


#else /* !CONFIG_C62_DEBUG_PROTO */

TEST_CASE("Debug: a production build answers NOT_ALLOWED and leaks nothing",
          "[pcproto]")
{
    Device d;
    Frame h = hello(d);
    takeFrames();

    uint32_t caps;
    REQUIRE(getU32(h.payload, PC_TAG_CAPS, caps));
    REQUIRE((caps & PC_CAP_DEBUG) == 0);
    uint32_t token = 0;
    REQUIRE_FALSE(getU32(h.payload, PC_TAG_TOKEN, token));

    const uint16_t ids[] = {
        PC_MSG_DBG_INFO, PC_MSG_DBG_LOG_SUB, PC_MSG_DBG_LOG_DRAIN,
        PC_MSG_DBG_REG_READ, PC_MSG_DBG_REG_WRITE, PC_MSG_DBG_AGC,
        PC_MSG_DBG_MEM_READ, PC_MSG_DBG_CAP_START, PC_MSG_DBG_CAP_STOP,
        PC_MSG_DBG_CAP_READ, PC_MSG_DBG_BB, PC_MSG_DBG_TX_TRACE,
        PC_MSG_DBG_RX_COUNTERS
    };

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        /* A well-formed request for each, with a token-shaped value that must
         * not help. */
        std::vector<uint8_t> req;
        putU16(req, PC_TAG_INDEX, 0);
        putU8(req, PC_TAG_CHIP, 0);
        putU8(req, PC_TAG_REG, 0x30);
        putU16(req, PC_TAG_VALUE, 0x1234);
        putU8(req, PC_TAG_CONFIRM, 1);
        putU32(req, PC_TAG_TOKEN, 0xDEADBEEF);
        putU32(req, PC_TAG_ADDR, 0x20000000);
        putU16(req, PC_TAG_LENGTH, 16);

        Frame f = request(d, ids[i], (uint8_t)(120 + i), req);
        REQUIRE(f.id == PC_MSG_ERROR);
        uint8_t err;
        REQUIRE(getU8(f.payload, PC_TAG_ERROR, err));
        REQUIRE(err == PC_ERR_NOT_ALLOWED);

        /* Nothing of the radio in the refusal: no data, no text, no token. */
        std::vector<uint8_t> data;
        std::string text;
        REQUIRE_FALSE(getRaw(f.payload, PC_TAG_DATA, data));
        REQUIRE_FALSE(getRaw(f.payload, PC_TAG_TASK_STACK, data));
        REQUIRE_FALSE(getStr(f.payload, PC_TAG_TEXT, text));
        REQUIRE_FALSE(getU32(f.payload, PC_TAG_TOKEN, token));
    }

    /* And the stores are still served: only the debug namespace is gone. */
    Frame f = request(d, PC_MSG_CHAN_COUNT, 140);
    REQUIRE(f.id == PC_MSG_CHAN_COUNT);
}

#endif /* CONFIG_C62_DEBUG_PROTO */
