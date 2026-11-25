/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The IMBE decoder against parameters that never came out of a transmitter.
 *
 * A P25 receiver hands the vocoder whatever the channel decoder produced, and
 * on a noisy channel or off a wrong sync that is any 88-bit vector at all: the
 * frame's own pitch index can point outside the table, the gain can be at its
 * maximum with no voiced harmonics to carry it, L can be 9 or 56 in the same
 * call. The decoder has to survive all of it -- return a value, fill 160
 * samples, keep them inside 16 bits -- because a fault here is a crash inside
 * the playback path, where the watchdog would take the whole radio.
 *
 * This is the host counterpart of lib/imbe_vocoder/tests/fuzz.c (which does the
 * same sweep under ASan/UBSan, see docs/IMBE.md §5): the same guarantees, but
 * run as part of the normal test suite, on the code the firmware links.
 *
 * The parameters are a fixed, seeded sequence, so a failure is reproducible.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "imbe_vocoder.h"
}

namespace
{

constexpr size_t FRAME_BYTES = IMBE_VOCODER_PARAM_BYTES;

/// Read a whole file, empty vector when it cannot be opened.
std::vector<uint8_t> readFile(const std::string &path)
{
    std::vector<uint8_t> out;
    FILE *f = fopen(path.c_str(), "rb");

    if (f == nullptr)
        return out;

    uint8_t chunk[4096];
    size_t n;

    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        out.insert(out.end(), chunk, chunk + n);

    fclose(f);
    return out;
}

/// 32-bit linear congruential generator: the same numbers on every host.
struct Lcg {
    uint32_t state = 0x12345678U;

    uint32_t next()
    {
        state = state * 1664525U + 1013904223U;
        return state >> 8;
    }

    uint8_t byte() { return (uint8_t)(next() & 0xFFU); }
};

/// Decode one frame into a buffer pre-filled with a pattern no frame produces.
int decodeInto(const uint8_t *params, int16_t *pcm)
{
    memset(pcm, 0x7F, IMBE_VOCODER_PCM_SAMPLES * sizeof(int16_t));
    return imbe_vocoder_decode(params, pcm);
}

/// True when the call filled the buffer: no sample is left at the pattern.
bool rendered(const int16_t *pcm)
{
    for (int i = 0; i < IMBE_VOCODER_PCM_SAMPLES; i++)
        if (pcm[i] == 0x7F7F)
            return false;

    return true;
}

/// Decode one frame and check the contract of the call.
int decodeChecked(const uint8_t *params, int16_t *pcm)
{
    const int rc = decodeInto(params, pcm);

    REQUIRE((rc == 0 || rc == 1));

    /* The frame is always rendered: a rejected one repeats the last good
       parameters, it does not leave the buffer as it was */
    REQUIRE(rendered(pcm));

    return rc;
}

} /* namespace */

TEST_CASE("every single bit error is survivable", "[imbe][p25][robust]")
{
    const std::vector<uint8_t> frames =
        readFile(std::string(IMBE_CORPUS_DIR) + "/p25_vc_frames.imbe");
    REQUIRE(frames.size() >= 8 * FRAME_BYTES);

    imbe_vocoder_reset();

    int16_t pcm[IMBE_VOCODER_PCM_SAMPLES];
    unsigned rejected = 0;
    unsigned total = 0;

    /* The first eight frames, each with every one of its 88 bits flipped once */
    for (size_t f = 0; f < 8; f++) {
        for (size_t bit = 0; bit < IMBE_VOCODER_PARAM_BITS; bit++) {
            uint8_t params[FRAME_BYTES];

            memcpy(params, &frames[f * FRAME_BYTES], FRAME_BYTES);
            params[bit / 8] ^= (uint8_t)(0x80U >> (bit % 8));

            rejected += (decodeChecked(params, pcm) == 1) ? 1U : 0U;
            total++;
        }
    }

    /* docs/IMBE.md §5: a single bit error costs a frame only when it lands on
       the parameters the repeat decision watches. Most land elsewhere. */
    INFO(rejected << " of " << total << " single-bit-error frames rejected");
    REQUIRE(total == 8 * 88);
    REQUIRE(rejected < total / 4);
}

TEST_CASE("random parameters are rendered, not trusted", "[imbe][p25][robust]")
{
    imbe_vocoder_reset();

    Lcg rng;
    int16_t pcm[IMBE_VOCODER_PCM_SAMPLES];
    uint8_t params[FRAME_BYTES];

    int peak = 0;
    int loudest = 0;

    for (unsigned frame = 0; frame < 20000; frame++) {
        for (size_t i = 0; i < FRAME_BYTES; i++)
            params[i] = rng.byte();

        decodeChecked(params, pcm);
        REQUIRE(imbe_vocoder_last_errors() == 0);

        /* The synthesis clips at PCM_LIMIT, 0.95 of full scale (imbe_decoder.c),
           so no sample can reach a rail whatever the parameters say */
        for (int16_t s : pcm) {
            const int a = (s < 0) ? -(int)s : (int)s;
            if (a > loudest)
                loudest = a;
        }

        if (loudest > peak)
            peak = loudest;

        REQUIRE(loudest <= 31128);  /* (int16_t)(32767 * 0.95) */
    }

    /* A pathological frame still produces sound, it does not go silent */
    REQUIRE(peak > 0);
}

TEST_CASE("the same parameters always render the same samples",
          "[imbe][p25][robust]")
{
    Lcg rng;
    uint8_t params[FRAME_BYTES];

    for (size_t i = 0; i < FRAME_BYTES; i++)
        params[i] = rng.byte();

    int16_t first[IMBE_VOCODER_PCM_SAMPLES];
    int16_t second[IMBE_VOCODER_PCM_SAMPLES];

    imbe_vocoder_reset();
    imbe_vocoder_decode(params, first);

    imbe_vocoder_reset();
    imbe_vocoder_decode(params, second);

    REQUIRE(memcmp(first, second, sizeof(first)) == 0);

    /* And the error count the frame is given reaches the state machine: the
       same frame with 8 corrected bits is not the same call as with none */
    imbe_vocoder_reset();
    imbe_vocoder_decode_errs(params, 8, second);
    REQUIRE(imbe_vocoder_last_errors() == 8);

    imbe_vocoder_decode_errs(params, 0, second);
    REQUIRE(imbe_vocoder_last_errors() == 0);
}
