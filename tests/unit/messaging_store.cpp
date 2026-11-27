/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The message store against a flash that can lose the power in the middle of a
 * write (tests/unit/fake_flash.cpp).
 *
 * The store keeps the record in two alternating sectors and a sequence number
 * decides which is newer.  The sequence number is in the *header*, and the
 * header used to go down before the payload: a power cut between the two left
 * the newest header pointing at half a record, the reader believed it, and the
 * CRC failure three layers up wiped the inbox and the outbox -- while the other
 * sector still held the previous set intact.  This radio is switched off by
 * cutting the power with the volume knob, so that is not a corner case.
 *
 * These cases are the promise: after a torn write the previous record is what
 * the radio comes back with.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" {
#include "core/messaging.h"
#include "core/messaging_store.h"
#include "fake_zephyr/zephyr/storage/flash_map.h"
}

namespace
{

/** A record holding one received message with the given text. */
int makeRecord(const char *text, uint32_t from, uint8_t *buf, size_t cap)
{
    messaging_clear();
    REQUIRE(messaging_received(MSG_TRANSPORT_APRS, from, 0u, text, true,
                               1000u) > 0);
    return messaging_serialise(buf, cap);
}

} // namespace

TEST_CASE("A record written to the store comes back byte for byte",
          "[messaging][store]")
{
    fake_flash_reset();

    uint8_t rec[MSG_STORE_MAX];
    uint8_t back[MSG_STORE_MAX];

    const int n = makeRecord("hola", 1234u, rec, sizeof(rec));
    REQUIRE(n > 0);
    REQUIRE(messaging_recordValid(rec, (size_t)n));

    REQUIRE(messaging_store_write(rec, (size_t)n));
    REQUIRE(messaging_store_size() == (size_t)n);

    const size_t got = messaging_store_read(back, sizeof(back));
    REQUIRE(got == (size_t)n);
    REQUIRE(memcmp(back, rec, got) == 0);
}

TEST_CASE("The newest of two sectors is the one read", "[messaging][store]")
{
    fake_flash_reset();

    uint8_t first[MSG_STORE_MAX];
    uint8_t second[MSG_STORE_MAX];
    uint8_t back[MSG_STORE_MAX];

    const int a = makeRecord("primero", 1111u, first, sizeof(first));
    REQUIRE(a > 0);
    REQUIRE(messaging_store_write(first, (size_t)a));

    const int b = makeRecord("segundo", 2222u, second, sizeof(second));
    REQUIRE(b > 0);
    REQUIRE(messaging_store_write(second, (size_t)b));

    const size_t got = messaging_store_read(back, sizeof(back));
    REQUIRE(got == (size_t)b);
    REQUIRE(memcmp(back, second, got) == 0);
}

/*
 * The defect: a header that made it to the flash over a payload that did not.
 * The reader must skip the newer sector and answer with the older one instead
 * of handing up a record that cannot be used.
 */
TEST_CASE("A sector whose payload was cut short does not overwrite the good one",
          "[messaging][store]")
{
    fake_flash_reset();

    uint8_t first[MSG_STORE_MAX];
    uint8_t second[MSG_STORE_MAX];
    uint8_t back[MSG_STORE_MAX];

    const int a = makeRecord("el bueno", 1111u, first, sizeof(first));
    REQUIRE(a > 0);
    REQUIRE(messaging_store_write(first, (size_t)a));

    const int b = makeRecord("el cortado", 2222u, second, sizeof(second));
    REQUIRE(b > 0);

    /*
     * Write the payload but stop half-way, as the power does.  The header is
     * written after the payload now, so this sector has no header at all; the
     * case below corrupts a payload that did get its header, which is what an
     * older build produced and what a flash that loses one block looks like.
     */
    fake_flash_cut_write_after(sizeof(second) / 2);
    REQUIRE_FALSE(messaging_store_write(second, (size_t)b));

    const size_t got = messaging_store_read(back, sizeof(back));
    REQUIRE(got == (size_t)a);
    REQUIRE(memcmp(back, first, got) == 0);
    REQUIRE(messaging_recordValid(back, got));
}

TEST_CASE("A header over a corrupt payload falls back to the other sector",
          "[messaging][store]")
{
    fake_flash_reset();

    uint8_t first[MSG_STORE_MAX];
    uint8_t second[MSG_STORE_MAX];
    uint8_t back[MSG_STORE_MAX];

    const int a = makeRecord("el bueno", 1111u, first, sizeof(first));
    REQUIRE(a > 0);
    REQUIRE(messaging_store_write(first, (size_t)a));

    const int b = makeRecord("el otro", 2222u, second, sizeof(second));
    REQUIRE(b > 0);
    REQUIRE(messaging_store_write(second, (size_t)b));

    /*
     * Corrupt one byte of the payload of whichever sector is the newest,
     * keeping its header: exactly the shape the old writer produced when the
     * power went off between the header and the payload.  The header carries
     * the sequence at +8, so the newest is decided the way the store does.
     */
    size_t size = 0;
    uint8_t *flash = fake_flash_bytes(&size);

    REQUIRE(size > 0x5000u + 64u);

    const uint32_t seqA = *(const uint32_t *)(flash + 0x4000u + 8u);
    const uint32_t seqB = *(const uint32_t *)(flash + 0x5000u + 8u);
    const uint32_t newest = ((int32_t)(seqA - seqB) > 0) ? 0x4000u : 0x5000u;
    const size_t older = (newest == 0x4000u) ? 0x5000u : 0x4000u;

    flash[newest + 32u] ^= 0x5Au;

    INFO("newest sector 0x" << std::hex << newest << std::dec);

    const size_t got = messaging_store_read(back, sizeof(back));
    REQUIRE(got == (size_t)a);
    REQUIRE(memcmp(back, first, got) == 0);
}

TEST_CASE("Two sectors that verify as nothing leave the store empty",
          "[messaging][store]")
{
    fake_flash_reset();

    uint8_t rec[MSG_STORE_MAX];
    uint8_t back[MSG_STORE_MAX];

    const int n = makeRecord("da igual", 1234u, rec, sizeof(rec));
    REQUIRE(n > 0);
    REQUIRE(messaging_store_write(rec, (size_t)n));

    /* Both sectors spoil, as a flash that has lost its charge would */
    size_t size = 0;
    uint8_t *flash = fake_flash_bytes(&size);

    flash[0x4000u + 32u] ^= 0xFFu;
    flash[0x5000u + 32u] ^= 0xFFu;

    REQUIRE(messaging_store_read(back, sizeof(back)) == 0u);
}

TEST_CASE("A read error on the newest sector falls back to the other",
          "[messaging][store]")
{
    fake_flash_reset();

    uint8_t first[MSG_STORE_MAX];
    uint8_t second[MSG_STORE_MAX];
    uint8_t back[MSG_STORE_MAX];

    const int a = makeRecord("viejo", 1111u, first, sizeof(first));
    REQUIRE(a > 0);
    REQUIRE(messaging_store_write(first, (size_t)a));

    const int b = makeRecord("nuevo", 2222u, second, sizeof(second));
    REQUIRE(b > 0);
    REQUIRE(messaging_store_write(second, (size_t)b));

    /*
     * The two headers are read first and pass; the payload of the newest
     * sector is the one that fails, and the other sector has to answer.
     */
    fake_flash_fail_reads_after(2, 1);
    const size_t got = messaging_store_read(back, sizeof(back));

    INFO("got " << got << " expected " << a);
    REQUIRE(got == (size_t)a);
    REQUIRE(memcmp(back, first, got) == 0);
}
