/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MODE_ARENA_H
#define MODE_ARENA_H

#include <stddef.h>
#include <stdint.h>

/**
 * One entry of a mode arena's registry: what a mode's object needs and what
 * the block is handed out for.
 */
struct modeArenaEntry
{
    uint8_t     id;     ///< OPMODE_* value the block belongs to
    const char *name;   ///< Short name, for the log and for the tests
    size_t      size;   ///< Bytes the mode's object needs
    size_t      align;  ///< Alignment the object needs
};

/**
 * One block of memory shared by all the operating modes.
 *
 * Only one mode runs at a time, so only one mode's object has to exist at a
 * time: the block is the size of the largest of them and each mode takes the
 * whole thing on entry and gives it back on exit. The objects are constructed
 * in place with the placement form of new and destroyed when the mode leaves
 * (docs/MEMORY.md has the inventory this size comes from).
 *
 * The class knows nothing about the modes: the caller passes the storage, its
 * capacity and the registry, so a test can drive it with its own table. The
 * firmware's registry and its single instance are in rtx.cpp, behind
 * modeArena_acquire()/modeArena_release().
 *
 * Ownership is single: acquiring while a block is held fails with -EBUSY
 * instead of returning the same memory twice, which is the overlap check.
 *
 * Released memory is filled with POISON, and acquiring looks at it first. A
 * block that is not all POISON at that moment was written after it was given
 * back -- a use after release -- and is counted in staleDetections(). It is
 * zeroed before the new mode gets it, exactly as the objects had in .bss, so a
 * mode still finds its "all zeros" starting point.
 */
class ModeArena
{
public:
    /// Byte written to a released block; not 0, so a stale write shows.
    static constexpr uint8_t POISON = 0xA5;

    /**
     * Constructor.
     *
     * @param storage: the block. Must be aligned at least to the largest
     *                 alignment in the registry; the firmware declares it
     *                 with that alignas.
     * @param capacity: bytes available at storage.
     * @param entries: the registry, kept by pointer: it must outlive the arena.
     * @param count: number of entries.
     */
    ModeArena(uint8_t *storage, size_t capacity, const modeArenaEntry *entries,
              size_t count);

    /** Bytes of the block: the size of the largest entry in the registry. */
    size_t size() const { return blockBytes; }

    /** Size an entry asks for, 0 if the mode is not in the registry. */
    size_t sizeOf(uint8_t id) const;

    /** Name of an entry, "" if the mode is not in the registry. */
    const char *nameOf(uint8_t id) const;

    /** True while a mode owns the block. */
    bool held() const { return inUse; }

    /** Id of the mode holding the block, OPMODE_NONE (0) when free. */
    uint8_t owner() const { return inUse ? ownerId : 0; }

    /** Bytes the current owner asked for, 0 when free. */
    size_t used() const { return inUse ? ownerSize : 0; }

    /** Start of the block, nullptr while free. */
    void *block() const { return inUse ? base : nullptr; }

    /**
     * Take the block for a mode.
     *
     * @param id: OPMODE_* of the mode that is about to be constructed.
     * @return 0 on success, -ENOENT if the mode is not in the registry,
     *         -EBUSY if a mode already holds the block (overlap), -ENOSPC if
     *         the registry asks for more than the storage has, -EINVAL if the
     *         storage is not aligned for that entry.
     */
    int acquire(uint8_t id);

    /**
     * Give the block back, filling it with POISON.
     *
     * The caller destroys the object first: this only owns bytes.
     */
    void release();

    /**
     * True if the released block still holds nothing but POISON, i.e. nobody
     * wrote to it after release(). Meaningless while a block is held.
     */
    bool poisonIntact() const;

    /** How many acquires found the released block written to. */
    uint32_t staleDetections() const { return stale; }

    /** Bytes of poison found in the released block, 0 if it is intact. */
    size_t poisonMissing() const;

private:
    const modeArenaEntry *find(uint8_t id) const;
    void fill(uint8_t value);

    uint8_t *storage;
    size_t capacity;
    const modeArenaEntry *entries;
    size_t count;

    uint8_t *base;     ///< Aligned start inside storage, set by acquire
    size_t blockBytes; ///< Size of the largest entry
    bool inUse;
    uint8_t ownerId;
    size_t ownerSize;
    uint32_t stale;
};

/**
 * The firmware's single arena, bound by rtx_init() and used by the mode switch.
 *
 * They exist so that the mode switch reads as one acquire and one release, and
 * so that a test can bind its own arena and drive exactly the same functions.
 */
void modeArena_bind(ModeArena *arena);

/**
 * Take the block for an operating mode.
 *
 * @return 0 on success, a negative errno from ModeArena::acquire() otherwise,
 *         or -ENODEV before rtx_init() has bound the arena.
 */
int modeArena_acquire(uint8_t modeId);

/** Give the block back. Safe to call with nothing bound or acquired. */
void modeArena_release(void);

/** The bound arena, nullptr before modeArena_bind(). */
ModeArena *modeArena_bound(void);

#endif /* MODE_ARENA_H */
