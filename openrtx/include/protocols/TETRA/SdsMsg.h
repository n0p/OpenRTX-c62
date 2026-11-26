/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * TETRA SDS message model for the C62 firmware.
 *
 * libtetra hands the application a struct tetra_sds: the raw short data of a
 * CMCE D-SDS-DATA / D-STATUS PDU (or its uplink counterparts) plus a few
 * addressing flags.  Everything the firmware needs to *show* a message --
 * which kind it is, the text in a font-safe form, the status value, the
 * protocol header -- is derived here, once, so the UI and the message store
 * never parse bits themselves.
 *
 * Field layout follows EN 300 392-2 clause 29 (SDS-TL) and clause 14.7.1.10
 * (D-SDS-DATA); see sds_msg.c for the exact bit positions.
 */

#ifndef TETRA_SDS_MSG_H
#define TETRA_SDS_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <tetra/tetra_common.h>
#include <tetra/tetra_sds.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Display bytes kept per message (the C62 line holds ~21 characters). */
#define SDS_TEXT_MAX 64
/** Raw bytes kept for SDS type 1/2/3 (8 octets is the largest of the three). */
#define SDS_RAW_BYTES 8

/** What a short data event turned out to be. */
enum sds_kind {
    SDS_KIND_EMPTY = 0, /**< Nothing displayable (or not short data). */
    SDS_KIND_TEXT,      /**< Text messaging (simple or SDS-TL). */
    SDS_KIND_STATUS,    /**< Pre-coded status, 16 bits. */
    SDS_KIND_RAW,       /**< SDS type 1/2/3: 16, 32 or 64 user bits. */
    SDS_KIND_LOCATION,  /**< Simple location system / SDS-TL location, NMEA. */
    SDS_KIND_LIP,       /**< LIP location report (TS 100 392-18-1). */
    SDS_KIND_OTHER,     /**< UDD4 with a protocol id we do not render. */
};

/** Decoded short data message, ready for display and storage. */
struct sds_msg {
    enum sds_kind kind;

    /* Origin, as libtetra reported it. */
    uint32_t src_ssi;
    uint32_t dst_ssi;
    uint8_t sds_type;   /**< enum tetra_sds_type (0..4). */
    bool complete;      /**< MAC reassembly completed (not a lost fragment). */
    bool addressed;     /**< Addressed to us (else seen in promiscuous mode). */

    /* UDD4 header (EN 300 392-2 clause 29.4 / table 29.18). */
    uint8_t pid;        /**< Protocol identifier, table 29.21. */
    bool sdstl;         /**< Carried in an SDS-TL SDS-TRANSFER. */
    bool immediate;     /**< "Display immediately" variant of the PID. */
    bool has_timestamp; /**< SDS-TL time stamp present. */
    uint32_t timestamp; /**< Raw 24-bit SDS-TL time stamp. */
    uint8_t coding;     /**< Text coding scheme (GSM7, Latin-1, UCS-2, ...). */
    uint8_t msg_ref;    /**< SDS-TL message reference. */
    uint8_t report_req; /**< SDS-TL delivery report request. */
    uint16_t text_bits; /**< Bits of payload after the header. */
    bool truncated;     /**< Text was cut to SDS_TEXT_MAX (or partly lost). */

    /* Rendered content. */
    char text[SDS_TEXT_MAX + 1]; /**< Font-safe ASCII, NUL terminated. */
    uint8_t text_len;
    uint16_t status;             /**< Pre-coded status (SDS_KIND_STATUS). */
    uint8_t raw[SDS_RAW_BYTES];  /**< SDS type 1/2/3 payload, first bytes. */
};

/**
 * \brief Decode a libtetra short data event into the message model.
 *
 * Never fails on well-formed libtetra data: the worst case is SDS_KIND_OTHER
 * or SDS_KIND_RAW. Returns 0 when there is something worth storing, -1 for
 * an event that carries nothing (part of a lost fragment, empty payload).
 *
 * @param s: short data event, valid during the libtetra callback only.
 * @param m: destination; the payload is copied, no pointers are kept.
 */
int sds_msg_decode(const struct tetra_sds *s, struct sds_msg *m);

/**
 * \brief One-line rendering for the UI, e.g. "4242: HOLA" or "STATUS 7".
 *
 * @param m: decoded message.
 * @param out: destination buffer.
 * @param len: size of the buffer.
 * @return bytes written, excluding the terminating NUL.
 */
int sds_msg_format(const struct sds_msg *m, char *out, size_t len);

/** @return a stable short name for the kind, for logs and diagnostics. */
const char *sds_msg_kind_name(enum sds_kind kind);

/**
 * \brief Hex dump of the first bytes of the raw payload.
 *
 * @param m: decoded message (SDS_KIND_RAW or any UDD4 kind).
 * @param out: destination buffer.
 * @param len: size of the buffer.
 * @return bytes written, excluding the terminating NUL.
 */
int sds_msg_hex(const struct sds_msg *m, char *out, size_t len);

/**
 * \brief Fold UTF-8 into the 7-bit ASCII the C62 font draws.
 *
 * Accented Latin-1 letters lose their accent (a-acute -> a), the GSM 03.38
 * symbols map to their nearest ASCII form (pound -> E ...), and anything the
 * font cannot draw becomes '?'. Control characters become spaces.
 *
 * @param in: NUL terminated UTF-8.
 * @param out: destination buffer.
 * @param len: size of the buffer.
 * @param truncated: set to true if the output did not fit (may be NULL).
 * @return bytes written, excluding the terminating NUL.
 */
int sds_ascii_from_utf8(const char *in, char *out, size_t len, bool *truncated);

/** @return true if the payload carried text we could render. */
bool sds_msg_has_text(const struct sds_msg *m);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_SDS_MSG_H */
