/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "protocols/DMR/DmrTxFsm.h"

/*
 * The transmit stage machine of the DMR mode, on the host.
 *
 * The bug this test is built around: the stages used to decide on their own
 * when to leave, and one of them -- "wait for the first vocoder frame" -- only
 * left when a condition that two different inputs control (PTT down *and* a
 * frame queued) became true.  Release the PTT before the first frame and the
 * machine never left: the PA stayed keyed, the radio never returned to
 * receive.  Every combination of stage, PTT and queue content is walked here,
 * and the rule checked is that the machine always ends.
 */

namespace
{

constexpr unsigned SLOT_LIMIT = 64; ///< slots any transmission may last here

struct Slot
{
    dmrTxStep step;
    dmrTxStage stage; ///< stage after the slot
};

/// Render `slots` timeslots, PTT and queue content given per slot.
std::vector<Slot> run(dmrTxFsm &fsm, const std::vector<bool> &ptt,
                      const std::vector<bool> &frames)
{
    std::vector<Slot> out;

    for (size_t i = 0; i < ptt.size(); i++) {
        dmrTxStep step = dmrTxFsm_next(&fsm, ptt[i], frames[i]);
        out.push_back({ step, fsm.stage });
        if (step.finished)
            break;
    }

    return out;
}

/// True if the transmission ended, and on which slot.
bool ends(const std::vector<Slot> &slots)
{
    return (slots.empty() == false) && slots.back().step.finished;
}

size_t countKind(const std::vector<Slot> &slots, enum dmrTxBurst kind)
{
    size_t n = 0;
    for (const auto &s : slots)
        if (s.step.kind == kind)
            n++;
    return n;
}

} /* namespace */

TEST_CASE("A call announces, speaks and terminates", "[dmr][tx]")
{
    dmrTxFsm fsm;
    dmrTxFsm_init(&fsm);

    /* Three slots of keyed voice, then release and let it drain */
    std::vector<bool> ptt(8, true);
    ptt[3] = ptt[4] = ptt[5] = ptt[6] = ptt[7] = false;
    std::vector<bool> frames(8, true);

    auto slots = run(fsm, ptt, frames);

    REQUIRE(ends(slots));
    REQUIRE(countKind(slots, DMR_TXB_VOICE_LC) == 1);
    REQUIRE(countKind(slots, DMR_TXB_TERMINATOR) == 1);
    REQUIRE(countKind(slots, DMR_TXB_VOICE) >= 1);

    /* The LC header comes before any voice burst, the terminator after it */
    size_t lc = 0, term = 0, firstVoice = slots.size();
    for (size_t i = 0; i < slots.size(); i++) {
        if (slots[i].step.kind == DMR_TXB_VOICE_LC)
            lc = i;
        if (slots[i].step.kind == DMR_TXB_VOICE)
            firstVoice = std::min(firstVoice, i);
        if (slots[i].step.kind == DMR_TXB_TERMINATOR)
            term = i;
    }
    REQUIRE(lc < firstVoice);
    REQUIRE(firstVoice < term);
}

TEST_CASE("One voice burst every other slot", "[dmr][tx]")
{
    dmrTxFsm fsm;
    dmrTxFsm_init(&fsm);

    std::vector<bool> ptt(12, true);
    ptt[8] = ptt[9] = ptt[10] = ptt[11] = false;
    std::vector<bool> frames(12, true);

    auto slots = run(fsm, ptt, frames);
    REQUIRE(ends(slots));

    /* Inside the voice stage, bursts never follow each other directly */
    bool inVoice = false;
    bool lastWasVoice = false;
    for (const auto &s : slots) {
        if (s.step.kind == DMR_TXB_VOICE_LC) {
            inVoice = true;
            continue;
        }
        if (s.step.kind == DMR_TXB_TERMINATOR)
            break;
        if ((inVoice == true) && (s.step.kind == DMR_TXB_EMPTY))
            lastWasVoice = false;
        if ((inVoice == true) && (s.step.kind == DMR_TXB_VOICE)) {
            REQUIRE(lastWasVoice == false);
            lastWasVoice = true;
        }
    }
}

TEST_CASE("Releasing the PTT in the lead time ends the transmission", "[dmr][tx]")
{
    dmrTxFsm fsm;
    dmrTxFsm_init(&fsm);

    /* Nothing queued yet: the old machine stayed in LEAD for ever here */
    std::vector<bool> ptt = { true, false, false, false, false, false };
    std::vector<bool> frames = { false, false, false, false, false, false };

    auto slots = run(fsm, ptt, frames);

    REQUIRE(ends(slots));
    REQUIRE(slots.size() <= 2);
    REQUIRE(countKind(slots, DMR_TXB_VOICE_LC) == 0);
}

TEST_CASE("The lead time does not last for ever with the PTT held", "[dmr][tx]")
{
    dmrTxFsm fsm;
    dmrTxFsm_init(&fsm);

    /* Dead vocoder, operator still holding the key: no infinite carrier */
    std::vector<bool> ptt(SLOT_LIMIT, true);
    std::vector<bool> frames(SLOT_LIMIT, false);

    auto slots = run(fsm, ptt, frames);

    REQUIRE(ends(slots));
    REQUIRE(slots.size() <= DMR_TX_MAX_LEAD_SLOTS + 1u);
    REQUIRE(countKind(slots, DMR_TXB_VOICE_LC) == 0);
}

TEST_CASE("No stage can hold the transmitter", "[dmr][tx]")
{
    /*
     * The safety exit must not depend on the stage: walk every stage (and two
     * values outside the enum, as a corrupted stage would be) with the PTT
     * released and nothing queued.
     */
    const std::vector<int> stages = { DMR_TX_STAGE_LEAD,
                                      DMR_TX_STAGE_LC_HEADER,
                                      DMR_TX_STAGE_VOICE,
                                      DMR_TX_STAGE_TERMINATOR,
                                      DMR_TX_STAGE_TAIL,
                                      42, 255 };

    for (int stage : stages) {
        dmrTxFsm fsm;
        dmrTxFsm_init(&fsm);
        fsm.stage = static_cast<dmrTxStage>(stage);

        std::vector<bool> ptt(SLOT_LIMIT, false);
        std::vector<bool> frames(SLOT_LIMIT, false);

        auto slots = run(fsm, ptt, frames);

        INFO("stage " << stage);
        REQUIRE(ends(slots));
        REQUIRE(slots.size() <= DMR_TX_MAX_SLOTS_AFTER_PTT + 1u);
    }
}

TEST_CASE("Every call ends after the PTT is released", "[dmr][tx]")
{
    /* Exhaustive: every stage, every queue state, release on slot 0..7 */
    const std::vector<int> stages = { DMR_TX_STAGE_LEAD, DMR_TX_STAGE_LC_HEADER,
                                      DMR_TX_STAGE_VOICE,
                                      DMR_TX_STAGE_TERMINATOR,
                                      DMR_TX_STAGE_TAIL };

    for (int stage : stages) {
        for (int haveFrames = 0; haveFrames < 2; haveFrames++) {
            for (unsigned keyed = 0; keyed < 8; keyed++) {
                dmrTxFsm fsm;
                dmrTxFsm_init(&fsm);
                fsm.stage = static_cast<dmrTxStage>(stage);

                std::vector<bool> ptt(SLOT_LIMIT, false);
                std::vector<bool> frames(SLOT_LIMIT, haveFrames != 0);
                for (unsigned i = 0; i < keyed; i++)
                    ptt[i] = true;

                auto slots = run(fsm, ptt, frames);

                INFO("stage " << stage << " frames " << haveFrames
                              << " keyed " << keyed);
                REQUIRE(slots.empty() == false);
                REQUIRE(slots.back().step.finished);
                /* Terminator plus tail slot, plus the two of margin */
                size_t afterRelease = slots.size() - std::min(slots.size(),
                                                              (size_t)keyed);
                REQUIRE(afterRelease <= DMR_TX_MAX_SLOTS_AFTER_PTT + 1u);
            }
        }
    }
}

/*
 * Cadence: the vocoder produces one 20 ms frame at a time (50 per second) and
 * the slot clock renders a burst every 60 ms, three frames each. This walks the
 * two together for ten seconds of a held PTT with the queue rules of
 * OpMode_DMR::txSlot() -- at most two frames taken per slot, at most three
 * carried by a burst, zero-filled when starved -- and checks that the speech
 * rate on the air is the rate of the microphone: no burst is dropped, none is
 * sent twice, and the queue neither grows without bound nor runs dry once the
 * call is under way.
 */
TEST_CASE("The voice cadence is one burst per slot pair, at 1x speed",
          "[dmr][tx]")
{
    constexpr unsigned SLOT_MS = 30;
    constexpr unsigned FRAME_MS = 20;
    constexpr unsigned FRAMES_PER_BURST = 3;
    constexpr unsigned FRAMES_PER_SLOT = 2; /* VOCODER_FRAMES_PER_SLOT */
    constexpr unsigned QUEUE_FRAMES = 12;   /* DMR_TX_QUEUE_FRAMES */
    constexpr unsigned SECONDS = 10;

    dmrTxFsm fsm;
    dmrTxFsm_init(&fsm);

    /* Encoded frames waiting for their burst, in production order */
    std::vector<int> queue;
    /* 8 kHz PCM blocks the microphone has delivered but the vocoder has not
     * eaten yet: the vocoder is what turns one into one encoded frame */
    unsigned pcmBlocks = 0;
    int produced = 0;
    int consumed = 0;
    std::vector<int> onAir;
    unsigned burstSlots = 0;
    unsigned emptySlots = 0;
    unsigned starvedBursts = 0;
    size_t maxQueue = 0;
    unsigned firstVoiceSlot = 0;
    unsigned slots = SECONDS * 1000 / SLOT_MS;
    unsigned nextFrameMs = 0;

    for (unsigned slot = 0; slot < slots; slot++) {
        unsigned slotStart = slot * SLOT_MS;

        /* The microphone: one 20 ms block every 20 ms */
        while (nextFrameMs <= slotStart) {
            pcmBlocks++;
            nextFrameMs += FRAME_MS;
        }

        /*
         * txSlot(): up to two inputStream_getData() calls per slot, each one
         * block of 160 samples and one encoded frame. The queue is bounded, as
         * in the firmware: a full queue means the slot carried no new frame.
         */
        for (unsigned i = 0; (i < FRAMES_PER_SLOT) && (pcmBlocks > 0)
                             && (queue.size() < QUEUE_FRAMES); i++) {
            pcmBlocks--;
            queue.push_back(produced++);
        }

        dmrTxStep step = dmrTxFsm_next(&fsm, true, !queue.empty());

        if (step.kind == DMR_TXB_VOICE) {
            burstSlots++;
            if (firstVoiceSlot == 0)
                firstVoiceSlot = slot;

            unsigned taken = 0;
            for (; taken < FRAMES_PER_BURST && !queue.empty(); taken++) {
                onAir.push_back(queue.front());
                queue.erase(queue.begin());
                consumed++;
            }
            if (taken < FRAMES_PER_BURST)
                starvedBursts++;
        } else if (step.kind == DMR_TXB_EMPTY) {
            emptySlots++;
        }

        maxQueue = std::max(maxQueue, queue.size());
    }

    /* One burst every other slot, and nothing else */
    REQUIRE(burstSlots == slots / 2);
    REQUIRE(emptySlots == slots / 2);

    /* Three frames per burst, 60 ms of speech: the rate of the microphone */
    REQUIRE(onAir.size() == (size_t)burstSlots * FRAMES_PER_BURST);
    REQUIRE(consumed == (int)(slots / 2) * (int)FRAMES_PER_BURST);

    /* No frame is sent twice, and none is skipped */
    for (size_t i = 0; i < onAir.size(); i++)
        REQUIRE(onAir[i] == (int)i);

    /* The queue stays bounded and, once the call is under way, does not starve */
    REQUIRE(maxQueue <= QUEUE_FRAMES);
    REQUIRE(starvedBursts <= 1);

    /*
     * Slot 0 is the lead (the first frame is already there), slot 1 carries the
     * voice LC header that announces the call and slot 2 the first voice burst:
     * speech is on the air 60 ms after the PTT, 90 ms at the latest.
     */
    REQUIRE(firstVoiceSlot == 2);

    /*
     * 10 s are 333 slots: 166 bursts of three frames, 498 frames on the air.
     * The microphone delivers 499 blocks in the same window -- the 20 ms and
     * 30 ms clocks only line up every 60 ms -- so the queue holds one frame at
     * the end and the PCM backlog the encoder has not eaten is bounded. That is
     * "1x speed": the air carries what the microphone produced, not more.
     */
    REQUIRE(produced == 499);
    REQUIRE(consumed == 498);
    REQUIRE(pcmBlocks <= 2);
}

TEST_CASE("A finished transmission asks for no more slots", "[dmr][tx]")
{
    dmrTxFsm fsm;
    dmrTxFsm_init(&fsm);

    std::vector<bool> ptt(6, true);
    ptt[2] = ptt[3] = ptt[4] = ptt[5] = false;
    std::vector<bool> frames(6, true);

    auto slots = run(fsm, ptt, frames);
    REQUIRE(ends(slots));

    /* The caller unkeys on `finished`; the machine is re-armed by init only */
    dmrTxFsm_init(&fsm);
    REQUIRE(fsm.stage == DMR_TX_STAGE_LEAD);
    REQUIRE(fsm.burstSlot == true);
    REQUIRE(fsm.slotsAfterPtt == 0);
}
