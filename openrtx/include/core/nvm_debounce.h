/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NVM_DEBOUNCE_H
#define NVM_DEBOUNCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file nvm_debounce.h
 * @brief Debounced write-back of one NVM record, with a retry that survives
 *        a failure.
 *
 * A radio that cuts power with the volume knob cannot write its settings on
 * the way out (core/state.c, NVMEM_DEBOUNCED_WRITE), so every change is
 * written once it has been stable for a while.  This is the decision part of
 * that: it owns the CRC of what is known to be on flash, the CRC of what is
 * waiting to be written, and the clocks.
 *
 * The point of keeping the two CRCs apart is the failure: a write that does
 * not land must not be recorded as if it had.  The old code updated its CRC
 * before the write, so a failed write was indistinguishable from a successful
 * one on the next pass -- pending was already false and the comparison already
 * equal -- and the setting stayed in RAM until the radio was switched off,
 * silently.  Here the change stays pending until the caller reports the write
 * as done, and a failed one is retried on a backoff instead of being either
 * dropped or retried every pass (an erase every 100 ms wears the sector out).
 *
 * The unit is milliseconds of a monotonic clock chosen by the caller: nothing
 * here reads a platform clock, which is what makes it testable on the host.
 */
struct nvmDebounce
{
    long long lastChange;  /**< When the record last differed from the flash */
    long long lastAttempt; /**< When a write was last tried                  */
    uint16_t  written;     /**< CRC of what the flash is known to hold       */
    uint16_t  pending;     /**< CRC of what is to be written                  */
    bool      dirty;       /**< pending is not what the flash holds           */
    uint8_t   failures;    /**< Failed attempts since the last success        */
};

/** Delay before the first retry of a failed write, ms. */
#define NVM_DEBOUNCE_RETRY_MS 10000U

/** Longest delay between two retries, ms. */
#define NVM_DEBOUNCE_RETRY_MAX_MS 60000U

/**
 * Adopt the record just read from the flash as the one on flash.
 *
 * The policy of "repair what the firmware cannot accept, once per boot" is the
 * caller's: it seeds the debouncer with the record as loaded, repairs its copy
 * and lets the difference be written like any other change.
 */
void nvmDebounce_seed(struct nvmDebounce *db, const void *data, size_t len);

/**
 * Say whether this pass of the periodic task should write the record.
 *
 * The debounce window restarts on every change, so a knob being turned
 * postpones the write instead of triggering one per pass; a write that failed
 * is retried after nvmDebounce_retryMs(), and not before.
 *
 * @param db: the record's debouncer.
 * @param data: the record as it is in RAM.
 * @param len: its size in bytes.
 * @param now: monotonic time, ms.
 * @param debounceMs: how long the record has to be stable before a write.
 * @return true when the caller should write it now.
 */
bool nvmDebounce_due(struct nvmDebounce *db, const void *data, size_t len,
                     uint32_t now, uint32_t debounceMs);

/** Report the write as landed: what was pending is now what the flash has. */
void nvmDebounce_done(struct nvmDebounce *db);

/** Report a failed write: the change stays pending, the retry backs off. */
void nvmDebounce_failed(struct nvmDebounce *db, uint32_t now);

/** Delay before the next attempt after a failure, ms. */
uint32_t nvmDebounce_retryMs(const struct nvmDebounce *db);

#ifdef __cplusplus
}
#endif

#endif /* NVM_DEBOUNCE_H */
