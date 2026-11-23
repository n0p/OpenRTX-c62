/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrPhy.c
 * @brief DMR receive PHY, see DmrPhy.h.
 *
 * Streaming version of the chain validated by the host prototype in
 * work/dmr-rx (reports/dmr_rx_proto.md): the prototype filters and decodes a
 * whole file, this one runs burst after burst on the radio's audio stream.
 *
 * Two differences from the prototype, both measured against it:
 *  - the matched filter is evaluated at 2 samples/symbol (24 kHz) instead of
 *    48 kHz, which halves its cost; the fractional symbol timing interpolates
 *    that stream with the same Catmull-Rom kernel;
 *  - the coarse sampling phase is chosen on the first burst decoded, from the
 *    one that opens the 4-level eye widest, instead of scoring the ten phases
 *    over the first seconds of the stream.
 */

#include <math.h>
#include <string.h>

#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrFec.h"
#include "protocols/DMR/DmrPhy.h"

/** Matched filter: RRC alpha = 0.2, 8 symbols of span, unit energy. */
#define DMR_MF_TAPS 81
#define DMR_MF_CENTER 40
/** Input history for the matched filter, at 48 kHz. Power of two. */
#define DMR_HIST_LEN 128
/** Matched filter output history, at 24 kHz: 205 symbols. Power of two. */
#define DMR_RING_LEN 1024
/** Symbols between the sync start and the end of the burst: 24 + 54. */
#define DMR_HEAD_SYMBOLS 78
/** Symbols before the sync that the burst needs: CACH 12 + payload 54. */
#define DMR_TAIL_SYMBOLS 66
/** Sync acceptance: sign errors and normalised correlation. */
#define DMR_SYNC_MAX_ERRORS 2
#define DMR_SYNC_MIN_SCORE 0.45f
/** A burst this recently decoded means the receiver is synchronised. */
#define DMR_SYNC_TIMEOUT_MS 300
/**
 * Once a burst is decoded the receiver knows the 30 ms slot grid: the next
 * sync can only be a multiple of 144 symbols away, give or take the sample
 * clock.  Searching only there costs 20 times less and, more important, keeps
 * a spurious correlation in the middle of a burst from shadowing the real
 * sync of the next slot.  Grid slots without a burst before falling back to a
 * free search.
 */
#define DMR_GRID_TOLERANCE 3
#define DMR_GRID_MISSES 3
/**
 * A burst whose 4-level eye is closed this far carries no usable bits (the
 * measured AMBE+2 Golay protection runs out around 5 % BER, EVM 0.4): drop it,
 * it is either noise or a spurious correlation of the sync.
 */
#define DMR_MAX_EVM 0.50f
/**
 * The coarse sampling phase is re-acquired unless the eye is at least this
 * open: with the right phase a clean signal sits around 0.02-0.05.
 */
#define DMR_EYE_ACQUIRED 0.15f
#define DMR_EYE_LOST 0.22f
#define DMR_WEAK_BURSTS 5

/*
 * Root-raised-cosine impulse response, alpha 0.2, 10 samples per symbol,
 * 8 symbols of span, normalised to unit energy.  The host prototype computes
 * it at start-up from its closed form (work/dmr-rx/src/dsp.cpp, design_rrc);
 * here it is a table so that the firmware needs no transcendental at boot.
 * Regenerate with scripts/gen_dmr_rrc.py.
 */
static const float dmr_rrc[DMR_MF_TAPS] = {
    8.656555859e-03f,  6.031442977e-03f,  2.235020933e-03f,  -2.384808633e-03f,
    -7.330730141e-03f, -1.200305434e-02f, -1.576462128e-02f, -1.801719139e-02f,
    -1.828097858e-02f, -1.626786627e-02f, -1.193878807e-02f, -5.536830589e-03f,
    2.410222124e-03f,  1.112111611e-02f,  1.962566710e-02f,  2.685826042e-02f,
    3.177228436e-02f,  3.346444934e-02f,  3.129596224e-02f,  2.499686974e-02f,
    1.474070833e-02f,  1.178894526e-03f,  -1.457211399e-02f, -3.099328423e-02f,
    -4.627240185e-02f, -5.845259980e-02f, -6.560768581e-02f, -6.602845603e-02f,
    -5.840218720e-02f, -4.196726040e-02f, -1.662647831e-02f, 1.699400444e-02f,
    5.754840415e-02f,  1.030401309e-01f,  1.509519069e-01f,  1.984273216e-01f,
    2.424887347e-01f,  2.802725793e-01f,  3.092610513e-01f,  3.274891331e-01f,
    3.337079221e-01f,  3.274891331e-01f,  3.092610513e-01f,  2.802725793e-01f,
    2.424887347e-01f,  1.984273216e-01f,  1.509519069e-01f,  1.030401309e-01f,
    5.754840415e-02f,  1.699400444e-02f,  -1.662647831e-02f, -4.196726040e-02f,
    -5.840218720e-02f, -6.602845603e-02f, -6.560768581e-02f, -5.845259980e-02f,
    -4.627240185e-02f, -3.099328423e-02f, -1.457211399e-02f, 1.178894526e-03f,
    1.474070833e-02f,  2.499686974e-02f,  3.129596224e-02f,  3.346444934e-02f,
    3.177228436e-02f,  2.685826042e-02f,  1.962566710e-02f,  1.112111611e-02f,
    2.410222124e-03f,  -5.536830589e-03f, -1.193878807e-02f, -1.626786627e-02f,
    -1.828097858e-02f, -1.801719139e-02f, -1.576462128e-02f, -1.200305434e-02f,
    -7.330730141e-03f, -2.384808633e-03f, 2.235020933e-03f,  6.031442977e-03f,
    8.656555859e-03f,
};

/** Timing search grid: +-1 sample of 48 kHz in steps of a quarter. */
#define DMR_TIMING_STEPS 4
/**
 * The symbol timing drifts with the sample clock of the capture (5 ppm is
 * 0.24 samples of 48 kHz per second), so the fractional offset is carried from
 * burst to burst.  Past this many samples the coarse phase steps instead, to
 * keep the interpolation near the centre of a symbol where it is accurate.
 */
#define DMR_TIMING_LIMIT 4.0f

struct dmrPhy {
    float hist[DMR_HIST_LEN]; ///< Input at 48 kHz, circular
    float ring[DMR_RING_LEN]; ///< Matched filter at 24 kHz, circular
    long inIndex;             ///< Absolute index of the newest 48 kHz sample
    long ringIndex;           ///< Absolute index of the newest 24 kHz sample
    long scan;                ///< Next symbol index to test for a sync
    long lastBurstSample;     ///< 48 kHz sample index of the last burst
    int phase;                ///< Coarse phase, in 48 kHz samples
    float timing;             ///< Fractional offset, 48 kHz samples
    bool timed;               ///< The coarse phase has been acquired
    unsigned weakBursts;      ///< Consecutive bursts with a closed eye
    long grid;                ///< Symbol of the last burst, -1 without grid
    unsigned gridMisses;      ///< Slot positions searched without a burst
};

static struct dmrPhy phy;

static const enum dmrSync syncCandidates[4] = {
    DMR_SYNC_BS_VOICE, DMR_SYNC_BS_DATA, DMR_SYNC_MS_VOICE, DMR_SYNC_MS_DATA
};

void dmrPhy_init(void)
{
    memset(&phy, 0, sizeof(phy));
    phy.inIndex = -1;
    phy.ringIndex = -1;
    phy.lastBurstSample = -1000000;
    phy.grid = -1;
}

void dmrPhy_reset(void)
{
    memset(phy.hist, 0, sizeof(phy.hist));
    memset(phy.ring, 0, sizeof(phy.ring));
    phy.scan = 0;
    phy.phase = 0;
    phy.timing = 0.0f;
    phy.timed = false;
    phy.weakBursts = 0;
    phy.grid = -1;
    phy.gridMisses = 0;
}

int dmrPhy_phase(void)
{
    return phy.phase;
}

bool dmrPhy_synced(void)
{
    long elapsed = phy.inIndex - phy.lastBurstSample;

    return elapsed < ((long)DMR_SYNC_TIMEOUT_MS * 48);
}

/** Matched filter output at a fractional 24 kHz index, Catmull-Rom. */
static float mf_at(float pos)
{
    long i = (long)pos;
    float f = pos - (float)i;
    long oldest = phy.ringIndex - DMR_RING_LEN + 4;

    if ((i < oldest + 1) || (i > phy.ringIndex - 2))
        return 0.0f;

    float x0 = phy.ring[(unsigned)i & (DMR_RING_LEN - 1)];
    if (f == 0.0f)
        return x0;

    float xm1 = phy.ring[(unsigned)(i - 1) & (DMR_RING_LEN - 1)];
    float x1 = phy.ring[(unsigned)(i + 1) & (DMR_RING_LEN - 1)];
    float x2 = phy.ring[(unsigned)(i + 2) & (DMR_RING_LEN - 1)];
    float a = -0.5f * xm1 + 1.5f * x0 - 1.5f * x1 + 0.5f * x2;
    float b = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c = -0.5f * xm1 + 0.5f * x1;

    return ((a * f + b) * f + c) * f + x0;
}

/**
 * Symbol k at a sampling phase, displaced by d 48 kHz samples: the ring holds
 * two samples per symbol, so each 48 kHz sample is half a ring sample.
 */
static float symbol_at(long k, int phase, float d)
{
    return mf_at(5.0f * (float)k + 0.5f * (float)phase + 0.5f * d);
}

/** Dibit of a symbol: +3 -> 3, +1 -> 2, -1 -> 0, -3 -> 1 (ETSI mapping). */
static int level_to_dibit(float y, float thresholdMid, float thresholdOuter)
{
    if (y > thresholdOuter)
        return 3;
    if (y > thresholdMid)
        return 2;
    if (y > -thresholdOuter)
        return 0;
    return 1;
}

/** Nominal level of a dibit, for the EVM: 0 and 2 are the inner levels. */
static float nominal_level(int dibit, float inner, float outer)
{
    float magnitude = ((dibit == 0) || (dibit == 2)) ? inner : outer;

    return (dibit < 2) ? -magnitude : magnitude;
}

/**
 * Centre and outer level of a burst, from its 24 sync symbols: all of them
 * carry the outer levels, so their mean is the carrier offset and their mean
 * absolute deviation the deviation.
 */
static float sync_levels(long k, int phase, float d, float *outer)
{
    float sum = 0.0f;
    float deviation = 0.0f;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
        sum += symbol_at(k + i, phase, d);
    float centre = sum / (float)DMR_SYNC_SYMBOLS;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
        deviation += fabsf(symbol_at(k + i, phase, d) - centre);
    *outer = deviation / (float)DMR_SYNC_SYMBOLS;

    return centre;
}

/**
 * Eye opening of one burst at a given sampling phase and fractional offset:
 * RMS distance of its 132 symbols to the four nominal levels, relative to the
 * outer level.
 */
static float burst_evm(long k, int phase, float d, float *centreOut,
                       float *outerOut)
{
    float outer;
    float centre = sync_levels(k, phase, d, &outer);

    *centreOut = centre;
    *outerOut = outer;

    if (outer < 1e-6f)
        return 1e9f;

    float inner = outer / 3.0f;
    float thresholdMid = 0.5f * inner;
    float thresholdOuter = 0.5f * (inner + outer);
    float acc = 0.0f;

    for (int i = 0; i < DMR_BURST_SYMBOLS; i++) {
        float v = symbol_at(k - 54 + i, phase, d) - centre;
        int dibit = level_to_dibit(v, thresholdMid, thresholdOuter);
        float error = v - nominal_level(dibit, inner, outer);
        acc += error * error;
    }

    return sqrtf(acc / (float)DMR_BURST_SYMBOLS) / outer;
}

/**
 * Fractional timing for a burst.  The 2-level sync correlates well even half
 * a symbol off, so it is not a usable timing criterion: the 4-level payload
 * is.  With the phase not acquired yet, all five even sampling phases of the
 * 48 kHz stream are tried (the quarter-sample grid covers the odd ones), and
 * the one that opens the eye widest is kept.
 *
 * @return the fractional offset, in 48 kHz samples.
 */
static float timing_search(long k, bool wide)
{
    float bestOffset = phy.timing;
    float bestEvm = 1e9f;
    int bestPhase = phy.phase;

    const int first = wide ? 0 : phy.phase;
    const int last = wide ? 8 : phy.phase;
    const int phaseStep = wide ? 2 : 1;
    const float centre = wide ? 0.0f : phy.timing;

    for (int phase = first; phase <= last; phase += phaseStep) {
        for (int step = -DMR_TIMING_STEPS; step <= DMR_TIMING_STEPS; step++) {
            float d = centre + 0.25f * (float)step;
            float levelCentre;
            float outer;
            float evm = burst_evm(k, phase, d, &levelCentre, &outer);

            if (evm < bestEvm) {
                bestEvm = evm;
                bestOffset = d;
                bestPhase = phase;
            }
        }
    }

    if (wide) {
        phy.phase = bestPhase;
        phy.timing = bestOffset;
        if (bestEvm < DMR_EYE_ACQUIRED) {
            phy.timed = true;
            phy.weakBursts = 0;
        }
        return bestOffset;
    }

    phy.timing = bestOffset;

    /* Follow the drift by stepping the phase, keeping the offset small */
    if (phy.timing > DMR_TIMING_LIMIT) {
        phy.phase = (phy.phase + 2) % 10;
        phy.timing -= 2.0f;
    } else if (phy.timing < -DMR_TIMING_LIMIT) {
        phy.phase = (phy.phase + 10 - 2) % 10;
        phy.timing += 2.0f;
    }

    return phy.timing;
}

/** Sign errors and normalised correlation of a 24-symbol window. */
static float match_sync(long k, const int8_t *pattern, float centre,
                        int *errors)
{
    float numerator = 0.0f;
    float denominator = 0.0f;
    int signErrors = 0;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
        float y = symbol_at(k + i, phy.phase, phy.timing) - centre;
        float w = (float)pattern[i];

        numerator += y * w;
        denominator += fabsf(y) * fabsf(w);
        if ((y > 0.0f) != (pattern[i] > 0))
            signErrors++;
    }

    *errors = signErrors;
    return (denominator > 1e-12f) ? (numerator / denominator) : -1.0f;
}

/**
 * Test a sync at symbol k.
 *
 * @return the best candidate, or DMR_SYNC_NONE.
 */
static enum dmrSync find_sync(long k, int *signErrors, float *score)
{
    int8_t pattern[DMR_SYNC_SYMBOLS];
    float centre = 0.0f;
    enum dmrSync best = DMR_SYNC_NONE;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
        centre += symbol_at(k + i, phy.phase, phy.timing);
    centre /= (float)DMR_SYNC_SYMBOLS;

    int bestErrors = DMR_SYNC_MAX_ERRORS + 1;
    float bestScore = -2.0f;

    for (size_t c = 0; c < 4; c++) {
        int errors;
        dmr_sync_symbols(syncCandidates[c], pattern);
        float s = match_sync(k, pattern, centre, &errors);

        if ((errors < bestErrors)
            || ((errors == bestErrors) && (s > bestScore))) {
            bestErrors = errors;
            bestScore = s;
            best = syncCandidates[c];
        }
    }

    *signErrors = bestErrors;
    *score = bestScore;

    if ((bestErrors > DMR_SYNC_MAX_ERRORS) || (bestScore < DMR_SYNC_MIN_SCORE))
        return DMR_SYNC_NONE;

    return best;
}

/** Pack 264 burst bits out of the symbol window. */
static void burst_to_bits(long k, float d, float centre, float inner,
                          float outer, uint8_t bits[DMR_BURST_BITS])
{
    float thresholdMid = 0.5f * inner;
    float thresholdOuter = 0.5f * (inner + outer);

    for (int i = 0; i < DMR_BURST_SYMBOLS; i++) {
        int dibit = level_to_dibit(symbol_at(k - 54 + i, phy.phase, d) - centre,
                                   thresholdMid, thresholdOuter);
        bits[2 * i] = (uint8_t)((dibit >> 1) & 1);
        bits[2 * i + 1] = (uint8_t)(dibit & 1);
    }
}

/**
 * Decode the burst whose sync starts at symbol k.  The caller has checked
 * that the samples for the whole window are in.
 */
static void decode_burst(long k, enum dmrSync sync, int signErrors,
                         struct dmrBurst *out)
{
    memset(out, 0, sizeof(*out));
    out->symbol = k;
    out->sync = sync;
    out->voice = dmr_sync_is_voice(sync);
    out->syncErrors = signErrors;

    float d = timing_search(k, !phy.timed);
    float outer;
    float centre = sync_levels(k, phy.phase, d, &outer);

    if (outer < 1e-6f)
        return;

    float inner = outer / 3.0f;
    float thresholdMid = 0.5f * inner;
    float thresholdOuter = 0.5f * (inner + outer);

    /* EVM of the burst, after the timing and level estimates */
    float acc = 0.0f;
    for (int i = 0; i < DMR_BURST_SYMBOLS; i++) {
        float v = symbol_at(k - 54 + i, phy.phase, d) - centre;
        int dibit = level_to_dibit(v, thresholdMid, thresholdOuter);
        float error = v - nominal_level(dibit, inner, outer);
        acc += error * error;
    }
    out->evm = sqrtf(acc / (float)DMR_BURST_SYMBOLS) / outer;

    /* A closed eye for several bursts in a row: re-acquire the phase */
    if (phy.timed) {
        if (out->evm > DMR_EYE_LOST) {
            if (++phy.weakBursts >= DMR_WEAK_BURSTS) {
                phy.timed = false;
                phy.weakBursts = 0;
            }
        } else {
            phy.weakBursts = 0;
        }
    }

    uint8_t bits[DMR_BURST_BITS];
    burst_to_bits(k, d, centre, inner, outer, bits);

    /* CACH, 12 symbols before the payload */
    uint8_t cach[DMR_CACH_BITS];
    for (int i = 0; i < DMR_CACH_SYMBOLS; i++) {
        int dibit = level_to_dibit(
            symbol_at(k - DMR_TAIL_SYMBOLS + i, phy.phase, d) - centre,
            thresholdMid, thresholdOuter);
        cach[2 * i] = (uint8_t)((dibit >> 1) & 1);
        cach[2 * i + 1] = (uint8_t)(dibit & 1);
    }
    int fixes = dmr_fec_cach_decode(cach);
    out->cachValid = fixes >= 0;
    out->accessType = cach[dmr_cach_tact_pos[0]];
    out->timeslot2 = cach[dmr_cach_tact_pos[1]] != 0;
    out->lateEntry = cach[dmr_cach_tact_pos[2]] != 0;

    if (out->voice) {
        /* 216 payload bits: 3 AMBE+2 frames, no further FEC outside them. */
        uint8_t payload[DMR_PAYLOAD_BITS];
        memcpy(payload, bits, DMR_HALF_BITS);
        memcpy(payload + DMR_HALF_BITS, bits + 2 * (54 + DMR_SYNC_SYMBOLS),
               DMR_HALF_BITS);

        out->frameCount = DMR_FRAMES_PER_BURST;
        for (int f = 0; f < DMR_FRAMES_PER_BURST; f++) {
            dmr_fec_voice_deinterleave(payload + f * DMR_AMBE_BITS,
                                       out->frame[f]);
        }
        return;
    }

    /* Data burst: BPTC(196,96) over the payload, slot type next to the sync */
    uint8_t info[DMR_INFO_BITS];
    memcpy(info, bits, 98);
    memcpy(info + 98, bits + 166, 98);

    uint8_t data[96];
    (void)dmr_fec_bptc_decode(info, data);

    uint8_t slotBits[20];
    memcpy(slotBits, bits + 98, 10);
    memcpy(slotBits + 10, bits + 156, 10);

    uint32_t codeword = 0;
    for (int i = 0; i < 20; i++)
        codeword = (codeword << 1) | (uint32_t)(slotBits[i] & 1u);

    uint8_t field = 0;
    int distance = dmr_fec_golay_decode(codeword, &field);
    out->slotTypeValid = distance <= 2;
    out->colourCode = (uint8_t)((field >> 4) & 0x0F);
    out->dataType = (uint8_t)(field & 0x0F);

    for (int i = 0; i < 96; i++) {
        if (data[i] != 0)
            out->lc[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    }

    /*
     * CRC-16 over the 96-bit block, masked per data type (TS 102 361-1
     * annex B): a valid block leaves a zero residue.  This is the link
     * quality measurement of the receiver.
     */
    uint16_t mask = 0;
    switch (out->dataType) {
        case DMR_DT_CSBK:
            mask = 0xA5A5;
            break;
        case DMR_DT_DATA_HEADER:
            mask = 0xCCCC;
            break;
        case DMR_DT_PI_HEADER:
            mask = 0x6969;
            break;
        case DMR_DT_MBC_HEADER:
            mask = 0xAAAA;
            break;
        default:
            break;
    }

    if ((mask != 0) && out->slotTypeValid) {
        uint8_t block[96];
        memcpy(block, data, sizeof(block));
        for (int i = 0; i < 16; i++)
            block[80 + i] ^= (uint8_t)((mask >> (15 - i)) & 1u);
        out->crcChecked = true;
        out->crcOk = dmr_fec_crc16(block, 96) == 0;
    }
}

/**
 * Next position of the slot grid at or after the scan pointer, with the
 * tolerance on either side.
 */
static long grid_target(long scan)
{
    long relative = scan - phy.grid;
    long slot = (relative + DMR_SLOT_SYMBOLS - 1 - DMR_GRID_TOLERANCE)
              / DMR_SLOT_SYMBOLS;

    if (slot < 1)
        slot = 1;
    return phy.grid + slot * DMR_SLOT_SYMBOLS;
}

/**
 * Run the sync search over the symbols that are fully in the matched filter
 * history, and decode what it finds.
 */
static void scan(dmrBurstHandler handler, void *user)
{
    /*
     * Newest complete symbol: 5k + phase/2 <= ringIndex, and two samples of
     * the interpolation kernel must exist past it.
     */
    long newest = (phy.ringIndex - phy.phase / 2) / 5;
    long oldest = newest - (DMR_RING_LEN / 5) + 8;

    if (newest < DMR_HEAD_SYMBOLS)
        return;

    if (phy.scan < DMR_TAIL_SYMBOLS)
        phy.scan = DMR_TAIL_SYMBOLS;
    if (phy.scan < oldest + DMR_TAIL_SYMBOLS)
        phy.scan = oldest + DMR_TAIL_SYMBOLS;

    while (phy.scan + DMR_HEAD_SYMBOLS <= newest) {
        long target = phy.scan;

        if (phy.grid >= 0) {
            target = grid_target(phy.scan);

            if (phy.scan < target - DMR_GRID_TOLERANCE) {
                phy.scan = target - DMR_GRID_TOLERANCE;
                continue;
            }
            if (phy.scan > target + DMR_GRID_TOLERANCE) {
                /* Slot searched, no burst: give up the grid after a few */
                if (++phy.gridMisses >= DMR_GRID_MISSES)
                    phy.grid = -1;
                continue;
            }
        }

        int signErrors;
        float score;
        enum dmrSync sync = find_sync(phy.scan, &signErrors, &score);
        (void)score;

        if (sync == DMR_SYNC_NONE) {
            phy.scan++;
            continue;
        }

        struct dmrBurst burst;
        decode_burst(phy.scan, sync, signErrors, &burst);
        if (burst.evm > DMR_MAX_EVM) {
            /* Closed eye: not a burst, keep looking */
            phy.scan++;
            continue;
        }

        if (handler != NULL) {
            handler(&burst, user);
            phy.lastBurstSample = phy.inIndex;
        }

        /*
         * Stay on the predicted slot, not on where the correlation peaked:
         * snapping to the prediction keeps the grid from creeping by the
         * couple of symbols a spurious peak can be off by.
         */
        phy.grid = target;
        phy.gridMisses = 0;
        phy.scan += DMR_BURST_SYMBOLS - 4;
    }
}

/** Matched filter at 24 kHz and symbol scan for one new input sample. */
static void push_sample(float sample, dmrBurstHandler handler, void *user)
{
    phy.inIndex++;
    phy.hist[(unsigned)phy.inIndex & (DMR_HIST_LEN - 1)] = sample;

    if (phy.inIndex < DMR_MF_CENTER)
        return;

    long mfIndex = phy.inIndex - DMR_MF_CENTER;
    if ((mfIndex & 1) != 0)
        return;

    float acc = 0.0f;
    for (int t = 0; t < DMR_MF_TAPS; t++) {
        acc += dmr_rrc[t]
             * phy.hist[(unsigned)(phy.inIndex - (long)t) & (DMR_HIST_LEN - 1)];
    }

    phy.ringIndex++;
    phy.ring[(unsigned)phy.ringIndex & (DMR_RING_LEN - 1)] = acc;

    scan(handler, user);
}

void dmrPhy_push(const int16_t *samples, size_t count, dmrBurstHandler handler,
                 void *user)
{
    for (size_t i = 0; i < count; i++)
        push_sample((float)samples[i], handler, user);
}
