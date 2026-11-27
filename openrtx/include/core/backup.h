/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BACKUP_H
#define BACKUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start a dump of the external flash memory content via xmodem transfer,
 * blocking function.
 */
void eflash_dump();
/*
 * There is no restore here on purpose: writing the whole external flash back
 * over xmodem would overwrite the bootloader, and core/backup_image.c is the
 * restore path the radio has (docs/BACKUP.md).
 */

#ifdef __cplusplus
}
#endif


#endif /* BACKUP_H */
