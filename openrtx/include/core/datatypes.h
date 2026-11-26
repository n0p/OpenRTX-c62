/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DATATYPES_H
#define DATATYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief CTCSS and DCS type definition.
 *
 * Continuous Tone Controlled Squelch System (CTCSS)
 * sub-audible tone frequency are expressed in \em tenth of Hz.
 * For example, the subaudible tone of 88.5 Hz is represented within Hamlib by
 * 885.
 *
 * Digitally-Coded Squelch codes are simple direct integers.
 */
typedef unsigned int tone_t;

/**
 * \brief Frequency type.
 *
 * Frequency type unit in Hz, able to hold SHF frequencies.
 */
typedef uint32_t freq_t;

/**
 * \brief RSSI type.
 *
 * Data type for RSSI, in dBm.
 */
typedef int32_t rssi_t;

/**
 * \brief Destination of a DMR call.
 *
 * Shared by the settings (the operator's default), the RTX status (what the
 * transmitter uses) and the channel store (per-channel override), so that the
 * three never disagree on the numbering.  The numeric values are stored in
 * flash: only append.
 */
enum dmrDestType
{
    DMR_DEST_NONE  = 0, /**< No destination: a voice call cannot be placed */
    DMR_DEST_GROUP = 1, /**< Talkgroup call   (LC PF 0, group address)     */
    DMR_DEST_USER  = 2, /**< Unit-to-unit call (LC PF 0, individual)       */
};

/** Smallest DMR identifier the transmission chain accepts (0 is "none"). */
#define DMR_ID_MIN 1U

/** Largest DMR identifier: 24 bits, TS 102 361-1 clause 9.3.1. */
#define DMR_ID_MAX 0xFFFFFFU

/**
 * \brief Payload format of a DMR short message.
 *
 * Seven of these are in the air (see protocols/DMR/dmr_sms.h); the operator
 * picks which one the radio transmits.  AUTO is the format this firmware has
 * always sent, so an upgrade does not change what leaves the radio.  The
 * numeric values are stored in the settings record: only append.
 */
enum dmrSmsFmtSetting
{
    DMR_SMS_FMT_AUTO = 0,   /**< The radio's own default (IP_HC today)      */
    DMR_SMS_FMT_DD_UTF16BE, /**< Defined data (DD_HEAD 0xD), UTF-16BE       */
    DMR_SMS_FMT_DD_UTF8,    /**< Defined data (DD_HEAD 0xD), UTF-8          */
    DMR_SMS_FMT_IP_5016,    /**< ETSI IP, UDP port 5016, UTF-16BE           */
    DMR_SMS_FMT_IP_HC,      /**< The same with the compressed header        */
    DMR_SMS_FMT_ANYTONE,    /**< AnyTone: UDP 5016, 00 0d 00 0a + UTF-16LE  */
    DMR_SMS_FMT_TMS,        /**< Motorola TMS, UDP 4007, UCS-2              */
    DMR_SMS_FMT_HYTERA,     /**< Hytera H-SMS: DD=BCD + UTF-16LE            */
    DMR_SMS_FMT_COUNT       /**< Number of formats, not a valid setting     */
};

#ifdef __cplusplus
}
#endif

#endif /* DATATYPES_H */
