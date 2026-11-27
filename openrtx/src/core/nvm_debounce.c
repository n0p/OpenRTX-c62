/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/nvm_debounce.h"
#include "core/crc.h"
#include <string.h>

void nvmDebounce_seed(struct nvmDebounce *db, const void *data, size_t len)
{
    if (db == NULL)
        return;

    memset(db, 0, sizeof(*db));
    db->written = crc_ccitt(data, len);
    db->pending = db->written;
}

bool nvmDebounce_due(struct nvmDebounce *db, const void *data, size_t len,
                     uint32_t now, uint32_t debounceMs)
{
    if (db == NULL)
        return false;

    uint16_t crc = crc_ccitt(data, len);

    if (crc == db->written) {
        /*
         * The flash holds this record.  Anything pending is either written or
         * no longer wanted; either way there is nothing to do and no failure
         * to remember.
         */
        db->dirty = false;
        db->failures = 0;
        return false;
    }

    if (!db->dirty || (crc != db->pending)) {
        /* A change, and the debounce window restarts with it */
        db->pending = crc;
        db->dirty = true;
        db->lastChange = now;
    }

    if ((now - db->lastChange) < (long long)debounceMs)
        return false;

    if (db->failures > 0) {
        if ((now - db->lastAttempt) < (long long)nvmDebounce_retryMs(db))
            return false;
    }

    db->lastAttempt = now;
    return true;
}

void nvmDebounce_done(struct nvmDebounce *db)
{
    if (db == NULL)
        return;

    db->written = db->pending;
    db->dirty = false;
    db->failures = 0;
}

void nvmDebounce_failed(struct nvmDebounce *db, uint32_t now)
{
    if (db == NULL)
        return;

    /*
     * The change stays pending: the next attempt writes it again.  The delay
     * grows with the number of failures and stops growing at the cap, so a
     * medium that is busy is retried soon and one that is broken does not get
     * an erase every pass of the task.
     */
    db->lastAttempt = (long long)now;
    db->dirty = true;
    if (db->failures < 8)
        db->failures++;
}

uint32_t nvmDebounce_retryMs(const struct nvmDebounce *db)
{
    uint32_t delay = NVM_DEBOUNCE_RETRY_MS;
    unsigned shift = (db->failures > 0) ? (unsigned)(db->failures - 1) : 0;

    if (shift > 3)
        shift = 3;
    delay <<= shift;

    if (delay > NVM_DEBOUNCE_RETRY_MAX_MS)
        delay = NVM_DEBOUNCE_RETRY_MAX_MS;

    return delay;
}
