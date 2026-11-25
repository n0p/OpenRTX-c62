/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/ModeArena.hpp"

#include <errno.h>
#include <string.h>

constexpr uint8_t ModeArena::POISON;

/*
 * The one arena the firmware uses, bound by rtx_init(). Until then the two
 * functions below refuse instead of dereferencing a null pointer: the UI and
 * the scan ask about the radio before the RTX thread has started.
 */
static ModeArena *boundArena = nullptr;

void modeArena_bind(ModeArena *arena)
{
    boundArena = arena;
}

ModeArena *modeArena_bound(void)
{
    return boundArena;
}

int modeArena_acquire(uint8_t modeId)
{
    if (boundArena == nullptr)
        return -ENODEV;
    return boundArena->acquire(modeId);
}

void modeArena_release(void)
{
    if (boundArena != nullptr)
        boundArena->release();
}

ModeArena::ModeArena(uint8_t *storage, size_t capacity,
                     const modeArenaEntry *entries, size_t count)
    : storage(storage), capacity(capacity), entries(entries), count(count),
      base(nullptr), blockBytes(0), inUse(false), ownerId(0), ownerSize(0),
      stale(0)
{
    for (size_t i = 0; i < count; i++)
        if (entries[i].size > blockBytes)
            blockBytes = entries[i].size;

    /*
     * A registry that asks for more than the storage holds is a build error the
     * firmware cannot see; refuse everything rather than overlap whatever
     * follows the block.
     */
    if (blockBytes > capacity)
        blockBytes = 0;

    fill(POISON); /* start released, so a first acquire sees clean poison */
}

void ModeArena::fill(uint8_t value)
{
    if (storage != nullptr)
        memset(storage, value, capacity);
}

const modeArenaEntry *ModeArena::find(uint8_t id) const
{
    for (size_t i = 0; i < count; i++)
        if (entries[i].id == id)
            return &entries[i];
    return nullptr;
}

size_t ModeArena::sizeOf(uint8_t id) const
{
    const modeArenaEntry *e = find(id);
    return e != nullptr ? e->size : 0;
}

const char *ModeArena::nameOf(uint8_t id) const
{
    const modeArenaEntry *e = find(id);
    return e != nullptr ? e->name : "";
}

int ModeArena::acquire(uint8_t id)
{
    const modeArenaEntry *e = find(id);
    if (e == nullptr || e->size == 0)
        return -ENOENT;

    /* Overlap: the block is not ours to hand out twice */
    if (inUse)
        return -EBUSY;

    if (blockBytes == 0 || e->size > capacity)
        return -ENOSPC;

    /*
     * Blocks all start at the base of the storage, aligned up to what this
     * mode needs. The firmware aligns the storage itself to the largest
     * alignment in the registry, so this is a no-op there and a real check in
     * a caller that guessed the alignment.
     */
    uintptr_t start = (uintptr_t)storage;
    uintptr_t aligned = (start + (e->align - 1u)) & ~(uintptr_t)(e->align - 1u);
    if (aligned < start)
        return -EINVAL;
    if ((aligned - start) + e->size > capacity)
        return -ENOSPC;

    /*
     * Detection, not prevention: poison that is gone means something wrote to
     * the block after the last release. The block is zeroed either way, so the
     * mode gets the .bss starting point it has always had.
     */
    if (!poisonIntact())
        stale++;

    fill(0x00);

    base = (uint8_t *)aligned;
    ownerId = id;
    ownerSize = e->size;
    inUse = true;
    return 0;
}

void ModeArena::release()
{
    inUse = false;
    ownerId = 0;
    ownerSize = 0;
    base = nullptr;

    /* The whole storage, not just the block: a pointer from an older mode is
     * caught as well, and the block is the whole storage's worth anyway. */
    fill(POISON);
}

bool ModeArena::poisonIntact() const
{
    return poisonMissing() == 0;
}

size_t ModeArena::poisonMissing() const
{
    if (storage == nullptr)
        return 0;

    size_t missing = 0;
    for (size_t i = 0; i < capacity; i++)
        if (storage[i] != POISON)
            missing++;
    return missing;
}
