/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_TETRA_SDS_ENC_H
#define OPENRTX_TETRA_SDS_ENC_H

/**
 * @file TetraSdsEnc.h
 * @brief TETRA uplink short data: the PDU encoder, the transmit counterpart
 * of SdsMsg.c.
 *
 * The layouts follow EN 300 392-2 clauses 14.7.2.7/8 (U-STATUS, U-SDS-DATA),
 * 14.8.38 (short data type identifier), 29.4 (SDS-TL) and 21.4.2
 * (MAC-ACCESS, MAC-DATA), and were checked against libtetra's uplink
 * receiver, which decodes what this produces back to the same text
 * (work/tetra-sds, `make test`).
 *
 * What this produces is the layer 2/3 content of an uplink burst.  What turns
 * it into radio is not here and is not enabled in this build: MAC-ACCESS
 * scheduling from the AACH, slot timing, LLC acknowledgement, pi/4-DQPSK
 * modulation and power ramping.  The C62 can receive TETRA but its transmit
 * chain has no TETRA mode, so a queued SDS is encoded and held, never keyed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tetra/tetra_sds.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Uplink CMCE PDU types. */
#define SDS_ENC_U_SDS_DATA 15
#define SDS_ENC_U_STATUS 8

/** Area selection used when the cell does not say otherwise. */
#define SDS_ENC_AREA_DEFAULT 0

/** A packed uplink PDU under construction. */
struct sds_tx {
    uint8_t buf[400];
    unsigned nbits;
    /**
     * Set when a writer was asked for more bits than buf holds.  Every
     * function that reports success or failure returns -1 once it is set, so
     * a too long text is refused instead of written past the end of the
     * writer -- which, in tetraSds_encodeText(), is a stack object.
     */
    bool overflow;
};

/**
 * Build an SDS-TL (or simple) text payload: protocol id, header and text,
 * coded GSM 7-bit, ISO 8859-1 or UCS-2.
 *
 * @param t: writer, its bits are appended, not reset.
 * @param text: NUL terminated ASCII or ISO 8859-1 text.
 * @param coding: TETRA_TCS_GSM7, TETRA_TCS_LATIN1 or TETRA_TCS_UCS2.
 * @param sdstl: true for an SDS-TRANSFER header (PIDs 0x82/0x89).
 * @param immediate: use the "display immediately" protocol id.
 * @param ref: SDS-TL message reference, 0..255 (ignored without SDS-TL).
 * @return 0 on success, -1 if the text does not fit or a character cannot be
 *         coded in GSM 7-bit.
 */
int sds_tx_text_payload(struct sds_tx *t, const char *text, uint8_t coding,
                        bool sdstl, bool immediate, uint8_t ref);

/**
 * Build a U-SDS-DATA PDU around an already coded payload.
 *
 * @param t: writer, reset by this call.
 * @param dst_ssi: called party SSI (individual or group).
 * @param area: area selection, 0..15.
 * @param payload: packed UDD4 bits, protocol id first.
 * @param payload_bits: number of payload bits.
 * @return 0 on success, -1 if it does not fit.
 */
int sds_tx_u_sds_data(struct sds_tx *t, uint32_t dst_ssi, uint8_t area,
                      const uint8_t *payload, unsigned payload_bits);

/** Build a U-STATUS PDU with a 16-bit pre-coded status. */
int sds_tx_u_status(struct sds_tx *t, uint32_t dst_ssi, uint8_t area,
                    uint16_t status);

/**
 * Wrap a CMCE PDU in MLE (CMCE) and LLC BL-UDATA, as MAC-DATA needs.
 *
 * BL-UDATA is the unacknowledged LLC service; an acknowledged transfer uses
 * BL-ADATA and needs the N(R)/N(S) state machine, which is not here.
 */
int sds_tx_wrap_llc(struct sds_tx *t, const struct sds_tx *cmce);

/**
 * MAC-ACCESS block (92 type-1 bits) carrying a TM-SDU, for SCH/HU.
 *
 * Only useful to test the encoder against a receiver: a real transmitter has
 * to win the random access slot first (AACH ACCESS-ASSIGN).
 *
 * @param out: 92 type-1 bits, one per byte.
 * @param ssi: sending MS identity.
 * @param sdu: TM-SDU bits.
 * @return 0 on success, -1 if it does not fit.
 */
int sds_tx_mac_access(uint8_t out[92], uint32_t ssi, const struct sds_tx *sdu);

/**
 * MAC-DATA block (268 type-1 bits) carrying a TM-SDU, for SCH/F.
 *
 * @param out: 268 type-1 bits, one per byte.
 * @param ssi: sending MS identity.
 * @param sdu: TM-SDU bits.
 * @return 0 on success, -1 if it does not fit.
 */
int sds_tx_mac_data(uint8_t out[268], uint32_t ssi, const struct sds_tx *sdu);

/**
 * Encode a text message end to end: payload (GSM 7-bit, or UCS-2 when a
 * character is not in the alphabet) and the U-SDS-DATA PDU around it.
 *
 * @param out: destination PDU writer.
 * @param dst_ssi: called party SSI.
 * @param text: UTF-8 or ISO 8859-1 text, NUL terminated.
 * @param sdstl: wrap the text in an SDS-TL transfer (protocol id 0x82).
 * @return 0 on success, -1 if the text does not fit.
 */
int tetraSds_encodeText(struct sds_tx *out, uint32_t dst_ssi, const char *text,
                        bool sdstl);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_TETRA_SDS_ENC_H */
