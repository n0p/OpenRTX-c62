/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A flash partition in RAM, for the tests that have to watch a store survive a
 * power cut.
 *
 * The firmware's NVM and message stores go through Zephyr's flash_area API;
 * the host has no flash, so this header stands in for
 * <zephyr/storage/flash_map.h> and the implementation below (fake_flash.cpp)
 * is a byte array with two hooks: one that makes a write land half-way, which
 * is what a power cut in the middle of a save looks like, and one that makes a
 * read fail the way a busy driver does.
 */

#ifndef TESTS_UNIT_FAKE_FLASH_MAP_H
#define TESTS_UNIT_FAKE_FLASH_MAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct flash_area {
    uint32_t off;
    uint32_t size;
};

/** The partition id the firmware asks for; there is only one here. */
#define storage_partition 0
#define FIXED_PARTITION_ID(x) ((uint8_t)(x))

int flash_area_open(uint8_t id, const struct flash_area **out);
void flash_area_close(const struct flash_area *area);
int flash_area_read(const struct flash_area *area, uint32_t off, void *dst,
                    size_t len);
int flash_area_write(const struct flash_area *area, uint32_t off,
                     const void *src, size_t len);
int flash_area_erase(const struct flash_area *area, uint32_t off, size_t len);

/** Back to erased (0xFF) and no injected failures. */
void fake_flash_reset(void);

/** The bytes, to corrupt a sector by hand. */
uint8_t *fake_flash_bytes(size_t *size);

/** Let the next write of this many bytes land, then stop as a power cut does. */
void fake_flash_cut_write_after(size_t bytes);

/** Make the next `count` reads fail. */
void fake_flash_fail_reads(int count);

/** Let `pass` reads through first, then make the next `count` fail. */
void fake_flash_fail_reads_after(int pass, int count);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_FAKE_FLASH_MAP_H */
