/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * What the dPMR mode publishes, and what the main screen makes of it.
 *
 * The finding this file answers: dPMR decoded the header's addresses, the
 * CCH's mode and address slices, the channel code and the END status into
 * accessors of OpMode_DPMR that nothing read, so the operator's screen could
 * only say "dPMR RX" or "SRCH". NXDN had solved that with NxdnStatus.c; dPMR
 * now has DpmrStatus.c, and this is its stand-in on the host, the same shape as
 * tests/unit/nxdn_tx.cpp's [status] group and tests/unit/tetra_status.cpp:
 *
 *  - a call is built by **this tree's own transmitter** (DpmrTx.c: preamble,
 *    header frame, superframes of four payload frames, END frame), shaped by
 *    the 4FSK modulator the mode uses and pushed through the **real
 *    demodulator** (dpmrPhy_push());
 *  - every decoded frame goes through the **real** DpmrStatus.c;
 *  - and the assertions are on two things at once: the struct the mode
 *    publishes (dpmr_getStatus()) and the strings ui_mode_state_read() returns,
 *    which is exactly what ui_main.c draws on lines 1 and 2 of the main screen.
 *
 * Nothing here needs a radio, and nothing here is a copy of the logic: the
 * three translation units under test are the ones the firmware compiles.
 *
 * The empty state is part of the contract (docs/TEST_PLAN.md section 3): a
 * receiver that has decoded nothing reads as searching, and one that is locked
 * but has not decoded the channel code yet says so without inventing a zero.
 * The simulator's own build checks the same strings on the drawn screen with
 * tests/unit/ui_empty_state.cpp.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/state.h"
#include "protocols/DPMR/DpmrDefs.h"
#include "protocols/DPMR/DpmrPhy.h"
#include "protocols/DPMR/DpmrStatus.h"
#include "protocols/DPMR/DpmrTx.h"
#include "protocols/Fsk4Tx.h"
#include "ui/ui_mode_state.h"
}

/* ui_mode_state_read() switches on the current mode: the state the UI keeps */
extern "C" {
state_t state;
}

namespace {

/** Outer deviation +-1050 Hz, what dPMR asks for on a 12.5 kHz channel. */
constexpr float LEVEL_12K5 = 3189.0f;

/** The call a stream carries. */
struct Call {
    uint32_t own;
    uint32_t called;
    uint16_t channelCode;
    uint8_t mode;
    uint8_t ht;
    uint16_t ci;
};

/** A transmission, from the transmitter's levels to 48 kHz samples. */
struct Air {
    std::vector<int16_t> samples;
    int frames = 0;     ///< Payload frames it carries
};

/**
 * Build a call with the tree's transmitter: two preamble frames (as
 * OpMode_DPMR sends), the header frame, `frames` payload frames and, when
 * asked for, the END frame with its 5 bit status (-1 for end type 0).
 */
Air buildCall(const Call &call, int frames, bool endFrame, int endStatus)
{
    dpmr_tx_t tx;
    fsk4_mod_t mod;

    dpmrTx_setup(&tx, call.own, call.called, call.channelCode);
    dpmrTx_setCallType(&tx, call.mode, 0, DPMR_FMT_PEER);
    fsk4Mod_init(&mod, DPMR_TX_SPS, LEVEL_12K5, false, false);

    Air out;
    std::vector<int16_t> block(DPMR_TX_FRAME_SAMPLES);
    int8_t levels[DPMR_FRAME_SYMBOLS];

    auto append = [&out](const std::vector<int16_t> &b, size_t n) {
        out.samples.insert(out.samples.end(), b.begin(), b.begin() + (long)n);
    };

    for (int p = 0; p < 2; p++) {
        dpmrTx_preamble(levels);
        fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, block.data());
        append(block, block.size());
    }

    dpmrTx_header(&tx, call.ht, call.ci, levels);
    fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, block.data());
    append(block, block.size());

    for (int n = 0; n < frames; n++) {
        uint8_t ambe[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES];

        /* A payload the mode can recognise: the bytes are not silence */
        for (int v = 0; v < DPMR_AMBE_FRAMES; v++)
            for (int b = 0; b < DPMR_AMBE_BYTES; b++)
                ambe[v][b] = (uint8_t)((n * 73 + v * 29 + b * 11 + 0x35) & 0xFF);

        dpmrTx_frame(&tx, ambe, levels);
        fsk4Mod_levels(&mod, levels, DPMR_FRAME_SYMBOLS, block.data());
        append(block, block.size());
        out.frames++;
    }

    if (endFrame) {
        dpmrTx_end(&tx, endStatus, levels);
        fsk4Mod_levels(&mod, levels, DPMR_TX_END_SYMBOLS, block.data());
        append(block, DPMR_TX_END_SAMPLES);
    }

    /*
     * The tail: the receiver decodes a frame only when a whole 192 symbol frame
     * of history is in its ring, and the END frame is 48 symbols long, so the
     * rest of that frame is silence, as the PA ramp-down of a real
     * transmission is.
     */
    std::vector<int16_t> tail(DPMR_TX_FRAME_SAMPLES, 0);

    fsk4Mod_levels(&mod, nullptr, DPMR_FRAME_SYMBOLS, tail.data());
    append(tail, tail.size());

    return out;
}

/** Frames decoded in a run, and the same frames fed to the status module. */
unsigned decoded;

void onFrame(const struct dpmrFrame *frame, void *user)
{
    (void)user;

    decoded++;
    dpmrStatus_frame(frame);
}

/** Push a stream in the 20 ms blocks the mode's audio path delivers. */
unsigned pushStream(const Air &air)
{
    std::vector<int16_t> block(960);

    dpmrPhy_init();
    decoded = 0;

    size_t off = 0;

    while (off < air.samples.size()) {
        size_t n = air.samples.size() - off;

        if (n > block.size())
            n = block.size();

        memcpy(block.data(), &air.samples[off], n * sizeof(int16_t));
        dpmrPhy_push(block.data(), n, onFrame, nullptr);
        off += n;
    }

    return decoded;
}

/** The block the main screen would draw for the dPMR mode. */
ui_mode_state_t screen()
{
    ui_mode_state_t st;

    state.channel.mode = OPMODE_DPMR;
    ui_mode_state_read(&st);

    return st;
}

const Call voiceCall = { 0x000135, 0x00393D, 0x0DD, DPMR_MODE_VOICE,
                         DPMR_HT_START, 0x123 };

} /* namespace */

TEST_CASE("dPMR publishes nothing until a burst arrives", "[dpmr][status][ui]")
{
    dpmrStatus_clear();

    struct dpmrStatus st;

    dpmr_getStatus(&st);

    REQUIRE_FALSE(st.sync);
    REQUIRE_FALSE(st.receiving);
    REQUIRE_FALSE(st.voice);
    REQUIRE(st.channelCode == -1);
    REQUIRE(st.callType == DPMR_CALLTYPE_NONE);
    REQUIRE_FALSE(st.idsValid);
    REQUIRE_FALSE(st.endSeen);
    REQUIRE(st.mode == 0xFF);
    REQUIRE(st.ht == 0xFF);
    REQUIRE(st.frames == 0);
    REQUIRE(st.ambeFrames == 0);

    /*
     * And the screen says it is searching: no identifier, no call, no quality
     * and no secondary text. This is the section 3 rule of docs/TEST_PLAN.md
     * ("sin sincronismo se lee SRCH, nunca un cero") applied to dPMR.
     */
    ui_mode_state_t ui = screen();

    INFO("dPMR with nothing decoded: id=\"" << ui.id << "\"");
    REQUIRE_FALSE(ui.sync);
    REQUIRE(std::string(ui.id) == "dPMR SRCH");
    REQUIRE(ui.callTxt[0] == '\0');
    REQUIRE(ui.qual[0] == '\0');
    REQUIRE(ui.note[0] == '\0');
    REQUIRE_FALSE(ui.call);
}

TEST_CASE("dPMR publishes the call its header and END frame carry",
          "[dpmr][status][ui]")
{
    /*
     * The whole path the UI reads, minus the radio: a call of this tree's
     * transmitter through the real demodulator and the real status module.
     * The header names the two stations, the CC frames carry the channel code,
     * the END frame closes the transmission with the status message of clause
     * 5.15, and all of it has to reach the main screen.
     */
    Air air = buildCall(voiceCall, 6, true, 0x15);

    dpmrStatus_clear();
    unsigned frames = pushStream(air);

    REQUIRE(frames >= 5);

    struct dpmrStatus st;

    dpmr_getStatus(&st);

    /* Sync and the channel code of the carrier */
    REQUIRE(st.sync);
    REQUIRE(st.voice);
    REQUIRE(st.channelCode == 0x0DD);
    REQUIRE(st.evm > 0);

    /* The call: who calls whom, the mode and the type of the header */
    REQUIRE(st.idsValid);
    REQUIRE(st.source == voiceCall.own);
    REQUIRE(st.dest == voiceCall.called);
    REQUIRE(st.mode == DPMR_MODE_VOICE);
    REQUIRE(st.ht == DPMR_HT_START);

    /* The END frame: released, with the status it carried */
    REQUIRE(st.endSeen);
    REQUIRE(st.endHasStatus);
    REQUIRE(st.endStatus == 0x15);
    REQUIRE(st.callType == DPMR_CALLTYPE_TX_REL);
    REQUIRE(std::string(dpmrStatus_callName(st.callType)) == "TX_REL");

    /* Counters: every frame, the header once, the voice frames and their AMBE */
    REQUIRE(st.frames == frames);
    REQUIRE(st.cchErrors == 0);
    REQUIRE(st.headers == 1);
    REQUIRE(st.ends >= 1);
    REQUIRE(st.voiceFrames >= 4);
    REQUIRE(st.ambeFrames == 4 * st.voiceFrames);

    /* And the screen, from the same numbers */
    ui_mode_state_t ui = screen();

    INFO("dPMR screen: id=\"" << ui.id << "\" call=\"" << ui.callTxt
                              << "\" note=\"" << ui.note << "\"");
    REQUIRE(ui.sync);
    REQUIRE(std::string(ui.id) == "CC0DD");
    REQUIRE(ui.call);
    REQUIRE(std::string(ui.callTxt) == "000135>00393D");
    REQUIRE(std::string(ui.note) == "TX_REL S21");
    /* The status needs the room the identifier does not give it on every row */
    REQUIRE(std::string(ui.noteShort) == "TX_REL");
    REQUIRE(ui.qual[0] == 'e');             /* EVM, in per cent */
    REQUIRE(std::string(ui.qual).find(" f") != std::string::npos);

    /*
     * A new frequency or a mode change: the state goes, the counters stay (they
     * are what the diagnostics read), which is what dpmrStatus_reset() is for.
     */
    dpmrStatus_reset();
    dpmr_getStatus(&st);

    REQUIRE_FALSE(st.sync);
    REQUIRE(st.channelCode == -1);
    REQUIRE(st.callType == DPMR_CALLTYPE_NONE);
    REQUIRE_FALSE(st.idsValid);
    REQUIRE_FALSE(st.endSeen);
    REQUIRE(st.frames == frames);
    REQUIRE(st.headers == 1);

    ui = screen();
    REQUIRE(std::string(ui.id) == "dPMR SRCH");

    /* Leaving the mode: nothing is left of it, counters included */
    dpmrStatus_clear();
    dpmr_getStatus(&st);

    REQUIRE(st.frames == 0);
    REQUIRE(st.voiceFrames == 0);
    REQUIRE(st.ambeFrames == 0);
    REQUIRE(st.ends == 0);
    REQUIRE(st.channelCode == -1);
    REQUIRE(st.evm == 0);
}

TEST_CASE("dPMR publishes a call in progress, and its release", "[dpmr][status][ui]")
{
    /*
     * A transmission with no END frame has not been released: the screen has to
     * say VCALL and keep the call readable, and the "RX" marker follows the
     * frame grid (dpmrStatus_receiving()), not the fact that something was
     * decoded. The same call with its END frame then reads TX_REL.
     */
    Air air = buildCall(voiceCall, 4, false, -1);

    dpmrStatus_clear();
    pushStream(air);

    struct dpmrStatus st;

    dpmr_getStatus(&st);
    REQUIRE(st.sync);
    REQUIRE(st.channelCode == 0x0DD);
    REQUIRE(st.idsValid);
    REQUIRE(st.callType == DPMR_CALLTYPE_VCALL);
    REQUIRE_FALSE(st.endSeen);
    REQUIRE_FALSE(st.endHasStatus);

    /* The grid was locked while the burst arrived; it is not now */
    dpmrStatus_receiving(true);

    ui_mode_state_t ui = screen();

    INFO("dPMR mid-call: id=\"" << ui.id << "\" note=\"" << ui.note << "\"");
    REQUIRE(std::string(ui.id) == "CC0DD RX");
    REQUIRE(std::string(ui.callTxt) == "000135>00393D");
    REQUIRE(std::string(ui.note) == "VCALL");

    dpmrStatus_receiving(false);
    ui = screen();
    REQUIRE(std::string(ui.id) == "CC0DD");

    /* The END frame of the same call closes it: TX_REL, without a status */
    Air released = buildCall(voiceCall, 4, true, -1);

    dpmrStatus_clear();
    pushStream(released);
    dpmr_getStatus(&st);

    REQUIRE(st.callType == DPMR_CALLTYPE_TX_REL);
    REQUIRE(st.endSeen);
    REQUIRE_FALSE(st.endHasStatus);     /* ET = 00 carries no status */

    ui = screen();
    REQUIRE(std::string(ui.note) == "TX_REL");
}

TEST_CASE("dPMR publishes a disconnect request as DISC", "[dpmr][status][ui]")
{
    /*
     * A header of type disconnect (clause 5.11) tears the call down and is
     * followed by an END frame. The END only carries it out: the screen has to
     * keep the reason the call ended, so the release must not overwrite DISC.
     */
    Call disc = voiceCall;

    disc.ht = DPMR_HT_DISCONNECT;

    dpmrStatus_clear();
    pushStream(buildCall(disc, 2, true, -1));

    struct dpmrStatus st;

    dpmr_getStatus(&st);
    REQUIRE(st.ht == DPMR_HT_DISCONNECT);
    REQUIRE(st.idsValid);
    REQUIRE(st.endSeen);
    REQUIRE(st.callType == DPMR_CALLTYPE_DISC);
    REQUIRE(std::string(dpmrStatus_callName(st.callType)) == "DISC");

    ui_mode_state_t ui = screen();

    INFO("dPMR disconnect: note=\"" << ui.note << "\"");
    REQUIRE(std::string(ui.note) == "DISC");
    REQUIRE(std::string(ui.callTxt) == "000135>00393D");
}

TEST_CASE("dPMR locked without a channel code says so, never CC000",
          "[dpmr][status][ui]")
{
    /*
     * A header frame alone: the receiver is locked and knows who is calling,
     * but the channel code travels in the frames that open with it (2 and 4 of
     * a superframe) and none of them has arrived. The identifier has to be the
     * absence of a code, not a zero that reads like one.
     */
    dpmrStatus_clear();
    pushStream(buildCall(voiceCall, 0, false, -1));

    struct dpmrStatus st;

    dpmr_getStatus(&st);
    REQUIRE(st.sync);
    REQUIRE(st.channelCode == -1);
    REQUIRE(st.idsValid);
    REQUIRE(st.callType == DPMR_CALLTYPE_VCALL);

    ui_mode_state_t ui = screen();

    INFO("dPMR header only: id=\"" << ui.id << "\"");
    REQUIRE(st.sync);
    REQUIRE(ui.sync);
    REQUIRE(std::string(ui.id) == "dPMR SYNC");
    REQUIRE(std::string(ui.id).find('0') == std::string::npos);
    REQUIRE(std::string(ui.callTxt) == "000135>00393D");
}
