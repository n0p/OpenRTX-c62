/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include "rtx/ModeArena.hpp"
#include "rtx/OpMode.hpp"

/*
 * The mode arena, on the host.
 *
 * One block, the size of the largest mode, handed from one mode to the next.
 * What this test pins down:
 *
 *   - the block is the size of the largest mode, not the sum of them, and that
 *     size is the one the firmware's map shows (docs/MEMORY.md, "inventory");
 *   - every one of the 121 ordered pairs of the eleven modes leaves and enters
 *     cleanly: the block is the same memory, it starts zeroed, the object
 *     constructed in it reports the mode it was built for, and nothing of the
 *     previous mode survives in the bytes the new mode was given;
 *   - the poison notices a write to a block that was already given back, which
 *     is the bug that would otherwise be silent until a mode showed another
 *     mode's call state;
 *   - acquiring while a block is held is refused (the overlap check), and so
 *     are modes the registry does not have.
 *
 * The sizes in the table are the firmware's, measured on the all-modes build
 * with DMR transmit (nm of the C62 image, docs/MEMORY.md). The objects here are
 * stubs: what the arena owns is bytes, and a stub is smaller than its entry,
 * which is the direction that has to be safe anyway. On the target, the
 * registry is built from alignof/sizeof of the real classes, so the same
 * properties hold with the real objects without a number to keep in sync.
 */

namespace
{

/// The eleven modes of the C62 build and what their objects cost, in bytes.
constexpr modeArenaEntry firmwareSizes[] = {
    { OPMODE_FM, "FM", 16, 8 },
    { OPMODE_AIRBAND, "AM airband", 28, 8 },
    { OPMODE_WFM_BC, "WFM broadcast", 16, 8 },
    { OPMODE_M17, "M17", 1864, 8 },
    { OPMODE_TETRA, "TETRA", 28, 8 },
    { OPMODE_DMR, "DMR", 13112, 8 },
    { OPMODE_NXDN, "NXDN", 5716, 8 },
    { OPMODE_DPMR, "dPMR", 5664, 8 },
    { OPMODE_P25, "P25", 32, 8 },
    { OPMODE_DSTAR, "D-STAR", 5788, 8 },
    { OPMODE_YSF, "YSF", 40, 8 },
};

constexpr size_t firmwareEntries =
    sizeof(firmwareSizes) / sizeof(firmwareSizes[0]);

/// The largest of them: what the block has to be.
constexpr size_t firmwareLargest = 13112;

/// Ids, in the order the table has them, for the 11x11 sweep.
constexpr uint8_t modeIds[] = {
    OPMODE_FM,     OPMODE_AIRBAND, OPMODE_WFM_BC, OPMODE_M17,   OPMODE_TETRA,
    OPMODE_DMR,    OPMODE_NXDN,    OPMODE_DPMR,   OPMODE_P25,   OPMODE_DSTAR,
    OPMODE_YSF,
};

constexpr size_t modeCount = sizeof(modeIds) / sizeof(modeIds[0]);

/// A mode's object, as far as the arena can tell one from another.
template <uint8_t ID>
class StubMode : public OpMode
{
public:
    opmode getID() override { return static_cast<opmode>(ID); }
};

/**
 * Construct the stub of a mode in a block, as rtx.cpp's constructMode() does
 * with the real classes.
 */
OpMode *construct(uint8_t id, void *block)
{
    switch (id) {
        case OPMODE_FM:
            return new (block) StubMode<OPMODE_FM>();
        case OPMODE_AIRBAND:
            return new (block) StubMode<OPMODE_AIRBAND>();
        case OPMODE_WFM_BC:
            return new (block) StubMode<OPMODE_WFM_BC>();
        case OPMODE_M17:
            return new (block) StubMode<OPMODE_M17>();
        case OPMODE_TETRA:
            return new (block) StubMode<OPMODE_TETRA>();
        case OPMODE_DMR:
            return new (block) StubMode<OPMODE_DMR>();
        case OPMODE_NXDN:
            return new (block) StubMode<OPMODE_NXDN>();
        case OPMODE_DPMR:
            return new (block) StubMode<OPMODE_DPMR>();
        case OPMODE_P25:
            return new (block) StubMode<OPMODE_P25>();
        case OPMODE_DSTAR:
            return new (block) StubMode<OPMODE_DSTAR>();
        case OPMODE_YSF:
            return new (block) StubMode<OPMODE_YSF>();
        default:
            return nullptr;
    }
}

/*
 * The storage the firmware declares with alignas(strictestModeAlign()), which
 * the table above says is 8.
 */
alignas(8) uint8_t storage[firmwareLargest];

} /* namespace */

TEST_CASE("the block is the size of the largest mode, not the sum", "[arena]")
{
    ModeArena arena(storage, sizeof(storage), firmwareSizes, firmwareEntries);

    size_t sum = 0;
    for (size_t i = 0; i < firmwareEntries; i++)
        sum += firmwareSizes[i].size;

    REQUIRE(arena.size() == firmwareLargest);
    REQUIRE(arena.size() < sum);
    REQUIRE(arena.nameOf(OPMODE_DMR) == std::string("DMR"));
}

TEST_CASE("every mode gets the block, aligned and zeroed", "[arena]")
{
    ModeArena arena(storage, sizeof(storage), firmwareSizes, firmwareEntries);

    for (size_t i = 0; i < firmwareEntries; i++) {
        const uint8_t id = firmwareSizes[i].id;

        REQUIRE(arena.acquire(id) == 0);
        REQUIRE(arena.held());
        REQUIRE(arena.owner() == id);
        REQUIRE(arena.used() == firmwareSizes[i].size);
        REQUIRE(arena.block() == static_cast<void *>(storage));
        REQUIRE((reinterpret_cast<uintptr_t>(arena.block()) %
                 firmwareSizes[i].align) == 0);

        /* The block starts at the .bss value the modes have always seen */
        const uint8_t *b = static_cast<const uint8_t *>(arena.block());
        for (size_t k = 0; k < arena.size(); k++)
            REQUIRE(b[k] == 0);

        arena.release();
        REQUIRE(!arena.held());
        REQUIRE(arena.block() == nullptr);
        REQUIRE(arena.poisonIntact());
    }
}

TEST_CASE("all 121 mode transitions leave nothing behind", "[arena]")
{
    ModeArena arena(storage, sizeof(storage), firmwareSizes, firmwareEntries);

    unsigned transitions = 0;

    for (size_t from = 0; from < modeCount; from++) {
        for (size_t to = 0; to < modeCount; to++) {
            /*
             * Enter "from": construct its object in the block and fill the
             * bytes past the object with a pattern, as a mode's own sample
             * buffers would.
             */
            REQUIRE(arena.acquire(modeIds[from]) == 0);

            OpMode *stub = construct(modeIds[from], arena.block());
            REQUIRE(stub != nullptr);
            REQUIRE(stub->getID() == static_cast<opmode>(modeIds[from]));

            uint8_t *bytes = static_cast<uint8_t *>(arena.block());
            for (size_t k = sizeof(OpMode); k < arena.size(); k++)
                bytes[k] = static_cast<uint8_t>(0x30u + from);

            /* Leave: destroy, give the block back, take it for the next mode */
            stub->~OpMode();
            arena.release();
            REQUIRE(arena.poisonIntact());

            REQUIRE(arena.acquire(modeIds[to]) == 0);
            REQUIRE(arena.block() == static_cast<void *>(storage));
            REQUIRE(arena.owner() == modeIds[to]);

            /*
             * Nothing of the previous mode survives, in the bytes this mode was
             * given nor in the ones past its end: a mode cannot read the
             * previous call's state by accident.
             */
            const uint8_t *fresh = static_cast<const uint8_t *>(arena.block());
            for (size_t k = 0; k < arena.size(); k++)
                REQUIRE(fresh[k] == 0);

            OpMode *next = construct(modeIds[to], arena.block());
            REQUIRE(next != nullptr);
            REQUIRE(next->getID() == static_cast<opmode>(modeIds[to]));
            next->~OpMode();

            arena.release();
            transitions++;
        }
    }

    REQUIRE(transitions == modeCount * modeCount);
    REQUIRE(transitions == 121);
    REQUIRE(arena.staleDetections() == 0);
}

TEST_CASE("the poison sees a write after release", "[arena]")
{
    ModeArena arena(storage, sizeof(storage), firmwareSizes, firmwareEntries);

    REQUIRE(arena.acquire(OPMODE_DMR) == 0);
    uint8_t *bytes = static_cast<uint8_t *>(arena.block());
    bytes[100] = 0x11;
    arena.release();

    REQUIRE(arena.poisonIntact());
    REQUIRE(arena.poisonMissing() == 0);

    /* A pointer kept from the DMR mode writes where the block now is */
    bytes[100] = 0x11;
    REQUIRE(!arena.poisonIntact());
    REQUIRE(arena.poisonMissing() == 1);

    /* The next mode still gets clean memory, and the write is counted */
    REQUIRE(arena.acquire(OPMODE_NXDN) == 0);
    REQUIRE(arena.staleDetections() == 1);
    REQUIRE(bytes[100] == 0);
    for (size_t k = 0; k < arena.size(); k++)
        REQUIRE(bytes[k] == 0);
}

TEST_CASE("a second mode cannot be given the block", "[arena]")
{
    ModeArena arena(storage, sizeof(storage), firmwareSizes, firmwareEntries);

    REQUIRE(arena.acquire(OPMODE_FM) == 0);
    REQUIRE(arena.sizeOf(OPMODE_DMR) == 13112);

    /* Overlap: the same memory would be handed out twice */
    REQUIRE(arena.acquire(OPMODE_DMR) == -EBUSY);
    REQUIRE(arena.owner() == OPMODE_FM);
    REQUIRE(arena.used() == 16);
    REQUIRE(arena.block() == static_cast<void *>(storage));

    arena.release();
    REQUIRE(arena.acquire(OPMODE_DMR) == 0);
    REQUIRE(arena.owner() == OPMODE_DMR);
}

TEST_CASE("modes outside the registry and short blocks are refused", "[arena]")
{
    ModeArena arena(storage, sizeof(storage), firmwareSizes, firmwareEntries);

    REQUIRE(arena.acquire(OPMODE_NONE) == -ENOENT);
    REQUIRE(arena.acquire(200) == -ENOENT);
    REQUIRE(!arena.held());
    REQUIRE(arena.sizeOf(OPMODE_NONE) == 0);
    REQUIRE(std::strcmp(arena.nameOf(OPMODE_NONE), "") == 0);

    /*
     * A block shorter than the largest mode is a build mistake, not a mode
     * error: it hands out nothing at all rather than overlap what follows it.
     */
    alignas(8) uint8_t small[1024];
    ModeArena narrow(small, sizeof(small), firmwareSizes, firmwareEntries);
    REQUIRE(narrow.size() == 0);
    REQUIRE(narrow.acquire(OPMODE_FM) == -ENOSPC);
    REQUIRE(narrow.acquire(OPMODE_DMR) == -ENOSPC);
    REQUIRE(!narrow.held());
}

TEST_CASE("the firmware's two functions drive the bound arena", "[arena]")
{
    ModeArena arena(storage, sizeof(storage), firmwareSizes, firmwareEntries);

    /* Before rtx_init() there is no arena, and that is not a crash */
    modeArena_bind(nullptr);
    REQUIRE(modeArena_acquire(OPMODE_FM) == -ENODEV);
    modeArena_release(); /* nothing bound: no-op */

    modeArena_bind(&arena);
    REQUIRE(modeArena_bound() == &arena);

    REQUIRE(modeArena_acquire(OPMODE_DMR) == 0);
    REQUIRE(arena.used() == 13112);
    REQUIRE(modeArena_acquire(OPMODE_FM) == -EBUSY);

    modeArena_release();
    REQUIRE(!arena.held());
    REQUIRE(arena.poisonIntact());

    modeArena_bind(nullptr);
}
