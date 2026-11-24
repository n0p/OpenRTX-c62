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

TEST_CASE("The call keeps one timeslot from the header to the terminator",
          "[dmr][tx]")
{
    /*
     * A DMR frame is 60 ms and holds two 30 ms slots.  Every burst of a call
     * -- voice LC header, each voice burst, the terminator -- must fall in the
     * same slot, which here is "the same parity of the rendered slot".  The
     * old machine announced the header one slot before the voice phase and
     * closed the call in whichever slot the PTT release landed on.
     */
    for (uint8_t ts = 1; ts <= 2; ts++) {
        dmrTxFsm fsm;
        dmrTxFsm_initSlot(&fsm, ts);

        std::vector<bool> ptt(SLOT_LIMIT, true);
        std::vector<bool> frames(SLOT_LIMIT, true);
        for (size_t i = 10; i < SLOT_LIMIT; i++)
            ptt[i] = false;

        auto slots = run(fsm, ptt, frames);
        REQUIRE(ends(slots));

        int phase = -1;
        size_t bursts = 0;

        for (size_t i = 0; i < slots.size(); i++) {
            const auto kind = slots[i].step.kind;
            if ((kind != DMR_TXB_VOICE_LC) && (kind != DMR_TXB_VOICE) &&
                (kind != DMR_TXB_TERMINATOR))
                continue;

            INFO("timeslot " << (int)ts << " slot " << i);
            if (phase < 0)
                phase = (int)(i & 1u);
            REQUIRE((int)(i & 1u) == phase);
            bursts++;
        }

        /* Header, several voice bursts and the terminator went out */
        REQUIRE(countKind(slots, DMR_TXB_VOICE_LC) == 1);
        REQUIRE(countKind(slots, DMR_TXB_VOICE) >= 2);
        REQUIRE(countKind(slots, DMR_TXB_TERMINATOR) == 1);
        REQUIRE(bursts >= 4);

        /* Slot 1 opens on the first slot, slot 2 on the second one */
        REQUIRE(phase == (ts == 1 ? 0 : 1));
    }
}

TEST_CASE("Timeslot 2 starts the call one slot later than timeslot 1",
          "[dmr][tx]")
{
    dmrTxFsm fsm1;
    dmrTxFsm fsm2;
    dmrTxFsm_initSlot(&fsm1, 1);
    dmrTxFsm_initSlot(&fsm2, 2);

    std::vector<bool> ptt(SLOT_LIMIT, true);
    std::vector<bool> frames(SLOT_LIMIT, true);
    for (size_t i = 10; i < SLOT_LIMIT; i++)
        ptt[i] = false;

    auto s1 = run(fsm1, ptt, frames);
    auto s2 = run(fsm2, ptt, frames);

    REQUIRE(ends(s1));
    REQUIRE(ends(s2));

    /* The header goes out in the first slot on TS1 and in the second on TS2 */
    REQUIRE(s1[0].step.kind == DMR_TXB_VOICE_LC);
    REQUIRE(s2[0].step.kind == DMR_TXB_EMPTY);
    REQUIRE(s2[1].step.kind == DMR_TXB_VOICE_LC);

    /* The same call, one slot (30 ms) later, never more */
    REQUIRE(s2.size() <= s1.size() + 1);
    REQUIRE(s1.size() <= s2.size() + 1);

    /* An out-of-range timeslot is not slot 2 */
    dmrTxFsm fsm3;
    dmrTxFsm_initSlot(&fsm3, 7);
    REQUIRE(fsm3.burstSlot == true);
}

TEST_CASE("A data call keeps its timeslot too", "[dmr][tx]")
{
    for (uint8_t ts = 1; ts <= 2; ts++) {
        dmrTxData data;
        dmrTxData_initSlot(&data, 2, ts);

        int phase = -1;

        for (unsigned i = 0; i < DMR_TX_DATA_MAX_SLOTS; i++) {
            dmrTxStep step = dmrTxData_next(&data);

            if (step.kind == DMR_TXB_DATA) {
                INFO("timeslot " << (int)ts << " slot " << i);
                if (phase < 0)
                    phase = (int)(i & 1u);
                REQUIRE((int)(i & 1u) == phase);
            }

            if (step.finished)
                break;
        }

        REQUIRE(phase == (ts == 1 ? 0 : 1));
    }
}
