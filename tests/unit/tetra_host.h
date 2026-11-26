/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The seams the TETRA host tests share: a PDU bit writer, the LLC hand-over
 * libtetra's own layer 3 tests use, a reader for a recorded downlink and an
 * uplink burst builder.
 *
 * Why these and not the air: the receive chain starts at the demodulator, and
 * on the host there is no demodulator fed by a radio. What can be handed to
 * the real library is what it would have decoded. Two seams are used, both the
 * ones libtetra's own tests use:
 *
 *  - the downlink, at the point where the MAC gives a complete TM-SDU to LLC
 *    (`tmo_llc()`, tmo_int.h). That is where a D-LOCATION-UPDATE-ACCEPT, a
 *    D-NWRK-BROADCAST or a D-SDS-DATA arrives, and the bits handed over are
 *    real PDU bits: the MM PDUs replayed from libtetra's capture and the
 *    header fields its own L3 tests write.
 *  - the uplink, one whole 510-dibit slot at a time (34 bits of ramping, the
 *    burst, noise). The burst is built from a MAC block with the library's
 *    encoder and its own burst builder, so a PDU produced by the firmware's
 *    encoder goes through the real SCH coding, scrambling and MAC decoding --
 *    the same path test_tmo_ul.cpp walks.
 *
 * Everything here is test scaffolding; no behaviour of the firmware depends on
 * it.
 */

#ifndef TETRA_HOST_H
#define TETRA_HOST_H

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>

extern "C" {
#include "tmo_int.h" /* tmo_llc(), struct tmo_macinfo: libtetra's L3 seam */
#include "util/bits.h"
}

namespace tetra_test
{

/** A packed PDU under construction: type-1 bits, MSB first, one per bit. */
struct writer {
    uint8_t b[512];
    unsigned n = 0;

    writer()
    {
        std::memset(b, 0, sizeof(b));
    }

    writer &put(uint32_t v, unsigned k)
    {
        bw_put(b, &n, v, k);
        return *this;
    }

    /** Append a literal bit string, e.g. a PDU captured from the air. */
    writer &bits(const char *s)
    {
        for (; *s != '\0'; s++)
            put(*s == '1', 1);
        return *this;
    }

    writer &append(const writer &src, unsigned from, unsigned k)
    {
        bits_copy(b, n, src.b, from, k);
        n += k;
        return *this;
    }

    /** MAC fill: a 1 then zeros, as the library expects at the end of an SDU. */
    writer &fillTo(unsigned end)
    {
        if (n >= end)
            return *this;

        put(1, 1);
        while (n < end)
            put(0, 1);
        return *this;
    }

    std::vector<uint8_t> oneBitEach(unsigned k) const
    {
        std::vector<uint8_t> v(k);

        for (unsigned i = 0; i < k; i++)
            v[i] = (uint8_t)bit_at(b, i);
        return v;
    }
};

/** A TM-SDU: LLC BL-UDATA, the MLE protocol discriminator, then the PDU. */
inline writer sdu(unsigned proto)
{
    writer w;

    return w.put(2, 4).put(proto, 3);
}

/**
 * Hand a TM-SDU to the receiver where the MAC would have, addressed by SSI.
 *
 * @param um: usage marker, 0 if the PDU does not assign one.
 */
inline void deliver(struct tetra_tmo_rx *rx, const writer &s, uint32_t ssi,
                    uint8_t um = 0)
{
    struct tmo_macinfo mi;

    std::memset(&mi, 0, sizeof(mi));
    mi.tn = 1;
    mi.addr_type = TMO_ADDR_SSI;
    mi.ssi = ssi;
    mi.usage_marker = um;
    tmo_llc(rx, &mi, s.b, 0, s.n);
}

/**
 * Read a recorded downlink (the .dibits files of libtetra's tests/vectors):
 * one byte per line packed two dibits per two bits, MSB first, as the module
 * stores them.
 *
 * @param path: the file.
 * @return one symbol per byte, or empty if the file cannot be read.
 */
inline std::vector<uint8_t> readDibits(const char *path)
{
    std::vector<uint8_t> out;
    FILE *f = std::fopen(path, "rb");

    if (f == nullptr)
        return out;

    int ch;
    while ((ch = std::fgetc(f)) != EOF) {
        for (int k = 6; k >= 0; k -= 2)
            out.push_back((uint8_t)((ch >> k) & 3));
    }
    fclose(f);
    return out;
}

/** One uplink slot stream: 34 bits of ramping, the burst, noise up to 510. */
struct uplink {
    std::vector<uint8_t> bits;
    uint32_t r = 0x9e3779b9u;
    uint32_t sc;

    explicit uplink(uint16_t mcc, uint16_t mnc, uint8_t cc)
        : sc(tetra_scramb_init(tetra_tmo_colour_code(mcc, mnc, cc)))
    {
    }

    void noise(unsigned n)
    {
        for (unsigned i = 0; i < n; i++) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            bits.push_back(r & 1);
        }
    }

    void slot(const uint8_t *burst, unsigned n)
    {
        noise(34);
        bits.insert(bits.end(), burst, burst + n);
        noise(510 - 34 - n);
    }

    /** A normal uplink burst (SCH/F) carrying one 268-bit MAC block. */
    void normal(const std::vector<uint8_t> &mac)
    {
        uint8_t t5[432], burst[TETRA_NUB_BITS];

        tetra_encode_sch(TETRA_SCH_F, mac.data(), sc, t5);
        tetra_tmo_build_nub(burst, t5, t5 + 216, false);
        slot(burst, TETRA_NUB_BITS);
    }

    /** A control uplink burst (SCH/HU) carrying one 92-bit MAC block. */
    void control(const std::vector<uint8_t> &mac)
    {
        uint8_t t5[168], burst[TETRA_CB_BITS];

        tetra_encode_sch(TETRA_SCH_HU, mac.data(), sc, t5);
        tetra_tmo_build_cb(burst, t5);
        slot(burst, TETRA_CB_BITS);
    }

    void push(struct tetra_tmo_rx *rx) const
    {
        tetra_tmo_push_bits(rx, bits.data(), bits.size());
    }
};

} /* namespace tetra_test */

#endif /* TETRA_HOST_H */
