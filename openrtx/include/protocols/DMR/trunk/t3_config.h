/*
 * SPDX-License-Identifier: MIT
 *
 * Build switches and the one helper the DMR Tier III engine (t3_*.c, from
 * RadioFoundry, MIT: LICENSE.radiofoundry next to the sources) takes from
 * outside: RadioFoundry's config.h and dmr_lc.h, reduced to that.
 *
 * Phase 1 on the C62: the receive side only (listen). The access side
 * (registration, random access, calls) comes with CONFIG_DMR_TRUNK_TX.
 */
#ifndef C62_T3_CONFIG_H
#define C62_T3_CONFIG_H

#include <stdint.h>

#ifndef CONFIG_DMR_TRUNK_TX
#define CONFIG_DMR_TRUNK_TX 0
#endif
#ifndef CONFIG_DMR_TRUNK_SUPL
#define CONFIG_DMR_TRUNK_SUPL 0
#endif
#ifndef CONFIG_DMR_TRUNK_GW
#define CONFIG_DMR_TRUNK_GW 0
#endif

/* CRC-CCITT of len octets, MSB first, initial 0, inverted (ETSI TS 102
 * 361-1 B.3.7): RadioFoundry's dmr_crc_ccitt(), the same as the bit-wise
 * one in dmr_fec.c */
uint16_t t3_crc_ccitt(const uint8_t *buf, uint8_t len);

#endif
