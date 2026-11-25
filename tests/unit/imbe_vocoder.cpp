/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The P25 IMBE decoder, on the host, against a committed rendering of it.
 *
 * lib/imbe_vocoder (docs/IMBE.md) is a C port of mbelib-neo's IMBE path that the
 * P25 receive mode links when CONFIG_C62_P25_VOCODER is on, which it is by
 * default. What this test protects is that the shipped decoder keeps producing
 * the same samples: the first 64 frames of the off-air corpus in
 * lib/imbe_vocoder/tests/data/p25_vc_frames.imbe, decoded here, must be
 * byte-identical to tests/unit/assets/imbe_p25_vc_64.pcm.
 *
 * That asset is the output of the decoder as it was verified against mbelib-neo
 * (61.78 dB SNR on this corpus, 50.73 dB on the 1000-frame encoder corpus, see
 * docs/IMBE.md §4 and lib/imbe_vocoder/tests/run_host.sh, which re-runs the
 * comparison against the reference build). It is a regression baseline, not the
 * fidelity check: the fidelity check needs mbelib-neo, which is not in this
 * repository.
 *
 * The corpus format is what the P25 LDU carries: 11 bytes per frame, 88
 * parameter bits, b0 first -- the same 11 bytes p25Tx_setVoiceSource() asks an
 * encoder for.
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
constexpr size_t PCM_BYTES = IMBE_VOCODER_PCM_SAMPLES * sizeof(int16_t);

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

/// One of the two frame corpora that ship with the decoder.
std::vector<uint8_t> readCorpus(const char *name)
{
    return readFile(std::string(IMBE_CORPUS_DIR) + "/" + name);
}

/// Decode a corpus with the shipped decoder, in order, from a reset state.
std::vector<int16_t> decodeCorpus(const std::vector<uint8_t> &frames,
                                  size_t limit)
{
    std::vector<int16_t> pcm;
    int16_t block[IMBE_VOCODER_PCM_SAMPLES];

    imbe_vocoder_reset();

    const size_t count = frames.size() / FRAME_BYTES;
    const size_t n = (limit < count) ? limit : count;

    for (size_t i = 0; i < n; i++) {
        imbe_vocoder_decode(&frames[i * FRAME_BYTES], block);
        pcm.insert(pcm.end(), block, block + IMBE_VOCODER_PCM_SAMPLES);
    }

    return pcm;
}

} /* namespace */

TEST_CASE("the decoder renders the committed frames bit for bit", "[imbe][p25]")
{
    /* The corpus of off-air frames and the baseline rendering of its first 64 */
    const std::vector<uint8_t> frames = readCorpus("p25_vc_frames.imbe");
    const std::vector<uint8_t> baseline =
        readFile(std::string(IMBE_ASSET_DIR) + "/imbe_p25_vc_64.pcm");

    REQUIRE(frames.size() == 261 * FRAME_BYTES);
    REQUIRE(baseline.size() == 64 * PCM_BYTES);

    const std::vector<int16_t> pcm = decodeCorpus(frames, 64);

    REQUIRE(pcm.size() * sizeof(int16_t) == baseline.size());

    size_t mismatched = 0;
    int worst = 0;

    for (size_t i = 0; i < pcm.size(); i++) {
        int16_t want;
        memcpy(&want, &baseline[i * sizeof(int16_t)], sizeof(want));

        if (pcm[i] != want) {
            mismatched++;
            const int diff = (int)pcm[i] - (int)want;
            if (diff > worst || -diff > worst)
                worst = (diff < 0) ? -diff : diff;
        }
    }

    INFO("samples that differ: " << mismatched << ", worst " << worst
                                 << " LSB");
    REQUIRE(mismatched == 0);
}

TEST_CASE("a new transmission starts from silence, not from the last one",
          "[imbe][p25]")
{
    const std::vector<uint8_t> frames = readCorpus("p25_vc_frames.imbe");
    REQUIRE(frames.size() == 261 * FRAME_BYTES);

    const std::vector<int16_t> first = decodeCorpus(frames, 32);

    /* Decode something else in between, then the same 32 frames again */
    decodeCorpus(readCorpus("speech_frames.imbe"), 64);
    const std::vector<int16_t> second = decodeCorpus(frames, 32);

    REQUIRE(first == second);

    /* And the state is the same size whatever the code is doing */
    REQUIRE(imbe_vocoder_state_bytes() == 11632);
}

TEST_CASE("the corpora decode as voice, in order, from a clean state",
          "[imbe][p25]")
{
    struct Corpus {
        const char *file;
        size_t frames;
        int peakMin; ///< The loudest sample has to reach at least this
    };

    const Corpus corpora[] = {
        { "p25_vc_frames.imbe", 261, 2000 },
        { "speech_frames.imbe", 1000, 2000 },
    };

    for (const Corpus &c : corpora) {
        const std::vector<uint8_t> frames = readCorpus(c.file);
        REQUIRE(frames.size() == c.frames * FRAME_BYTES);

        imbe_vocoder_reset();

        const size_t count = frames.size() / FRAME_BYTES;
        int peak = 0;
        unsigned voice = 0;
        unsigned rejected = 0;

        for (size_t i = 0; i < count; i++) {
            int16_t pcm[IMBE_VOCODER_PCM_SAMPLES];
            const int rc = imbe_vocoder_decode(&frames[i * FRAME_BYTES], pcm);

            /* 0 is a voice frame, 1 a frame the decoder repeated or muted */
            REQUIRE((rc == 0 || rc == 1));

            if (rc == 0)
                voice++;
            else
                rejected++;

            for (int16_t s : pcm) {
                const int a = (s < 0) ? -(int)s : (int)s;
                if (a > peak)
                    peak = a;
            }

            /* The corrected-bit count is the one the frame came with: 0 */
            REQUIRE(imbe_vocoder_last_errors() == 0);
        }

        INFO(c.file << ": " << voice << " voice, " << rejected << " repeated");
        REQUIRE(rejected == 0);
        REQUIRE(voice == count);
        REQUIRE(peak >= c.peakMin);
        /* 16-bit PCM with headroom: a full-scale frame would be a bug */
        REQUIRE(peak <= 32767);
    }

    /* Arguments are checked: a null block is refused, not crashed on */
    uint8_t params[IMBE_VOCODER_PARAM_BYTES] = { 0 };
    int16_t pcm[IMBE_VOCODER_PCM_SAMPLES];

    REQUIRE(imbe_vocoder_decode(nullptr, pcm) == -1);
    REQUIRE(imbe_vocoder_decode(params, nullptr) == -1);
}
