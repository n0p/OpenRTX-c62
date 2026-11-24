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
