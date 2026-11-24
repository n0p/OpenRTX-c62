/* SPDX-FileCopyrightText: 2010 mbelib Author
 * SPDX-FileCopyrightText: 2025 arancormonk
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * AMBE+2 (3600x2450) quantiser tables, from mbelib through OpenDMR
 * (decoder/ambe3600x2450_const.h).  Verbatim values; only the names
 * carry a dmr_enc_ prefix so that the firmware has one namespace.
 */
#ifndef DMR_ENC_AMBE_TABLES_H
#define DMR_ENC_AMBE_TABLES_H

#include <stdint.h>
extern const float dmr_enc_AmbeW0table[120];
extern const float dmr_enc_AmbeLtable[120];
extern const int dmr_enc_AmbeVuv[32][8];
extern const int dmr_enc_AmbeLmprbl[57][4];
extern const float dmr_enc_AmbeDg[32];
extern const float dmr_enc_AmbePRBA24[512][3];
extern const float dmr_enc_AmbePRBA58[128][4];
extern const float dmr_enc_AmbeHOCb5[32][4];
extern const float dmr_enc_AmbeHOCb6[16][4];
extern const float dmr_enc_AmbeHOCb7[16][4];
extern const float dmr_enc_AmbeHOCb8[8][4];
#define DMR_ENC_B0_LOOKUP_LEN 898
extern const uint8_t dmr_enc_b0_lookup[DMR_ENC_B0_LOOKUP_LEN];

#endif /* DMR_ENC_AMBE_TABLES_H */
