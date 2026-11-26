/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host check of the TETRA uplink path of the firmware, without a radio.
 *
 * Three things, all of them against the real code that runs on the C62:
 *
 *  1. The duplex conversion of protocols/TETRA/TetraUplink.c against libtetra's
 *     own table (tetra_uplink_hz), over every band, duplex code and both
 *     directions: two copies of TS 100 392-15 table 2, one of them for the host
 *     build, which has no libtetra, so they have to agree exactly.
 *  2. The cell event to the status: what a receiver locked to a downlink hands
 *     TetraStatus.c, and what the "Cell uplink" session makes of it (published
 *     uplink, or the duplex conversion of the downlink, or "no uplink").
 *  3. A real MS burst: a TMO control uplink burst (table 9.1, MAC-ACCESS with a
 *     U-SETUP, the extended training sequence x of clause 9.4.4) built with
 *     libtetra's own encoder, scrambled with the cell's colour code, pushed
 *     through the same receiver the mode uses, and checked in the firmware's
 *     very own status callback: uplink flag set and the sending SSI reported.
 *
 * Build (host; the firmware's own translation units, not copies):
 *
 *   g++ -std=c++14 -O2 -I openrtx/include -I <libtetra>/include \
 *       -I <libtetra>/build/generated tests/platform/c62_tetra_uplink_host.cpp \
 *       openrtx/src/protocols/TETRA/TetraStatus.c \
 *       openrtx/src/protocols/TETRA/TetraUplink.c \
 *       <libtetra>/build/libtetra.a -lpthread -lm -o c62_tetra_uplink_host
 *   ./c62_tetra_uplink_host            # the three checks above
 *   ./c62_tetra_uplink_host -u 228:8889:1 capture.wav|.s16
 *                                      # ... and a capture, when there is one
 *
 * The capture path is the harness waiting for a real uplink recording: it
 * demodulates 48 kHz discriminator audio with the firmware's demodulator, sets
 * the cell the operator would have learnt from the downlink, and prints what
 * the status ends up holding. No capture ships with the repository; §5 of
 * docs/TETRA_LIB.md says why and what to feed it.
 */

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <tetra/tetra.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>
#include "protocols/TETRA/TetraDemod.h"
#include "protocols/TETRA/TetraStatus.h"
#include "protocols/TETRA/TetraUplink.h"
}

/* ---- Stubs for what TetraStatus.c calls out of the mode ------------------ */

static long long ticks;
static unsigned nSds;
static unsigned nTraffic;

extern "C" long long getTick(void)
{
    return ticks;
}

extern "C" int tetraSds_onEvent(const struct tetra_sds *s)
{
    (void)s;
    nSds++;
    return 0;
}

extern "C" void tetraSpeech_traffic(const struct tetra_traffic *t)
{
    (void)t;
    nTraffic++;
}

/* ---- A tiny bit writer, as the library tests use ------------------------- */

namespace
{

struct Bits
{
    uint8_t b[512] = { 0 };
    unsigned n = 0;

    void put(uint32_t v, unsigned k)
    {
        for (unsigned i = 0; i < k; i++) {
            unsigned bit = (v >> (k - 1 - i)) & 1u;

            if (bit)
                b[n >> 3] |= (uint8_t)(0x80u >> (n & 7u));
            n++;
        }
    }

    void append(const Bits &src, unsigned k)
    {
        for (unsigned i = 0; i < k; i++)
            put((src.b[i >> 3] >> (7u - (i & 7u))) & 1u, 1);
    }

    void fillTo(unsigned end)
    {
        if (n >= end)
            return;
        put(1, 1); /* fill bits */
        while (n < end)
            put(0, 1);
    }

    std::vector<uint8_t> bits(unsigned k) const
    {
        std::vector<uint8_t> v(k);

        for (unsigned i = 0; i < k; i++)
            v[i] = (uint8_t)((b[i >> 3] >> (7u - (i & 7u))) & 1u);
        return v;
    }
};

const uint16_t MCC = 228;
const uint16_t MNC = 8889;
const uint8_t CC = 1;
const uint32_t UL_SSI = 1234567;
const uint32_t UL_GSSI = 9001;

/* BL-UDATA, MLE: CMCE, U-SETUP to a group (the MS asks to transmit). */
Bits u_setup(uint32_t gssi)
{
    Bits p;

    p.put(2, 4);
    p.put(2, 3);
    p.put(7, 5);
    p.put(0, 4 + 1 + 1);
    p.put(1 << 2, 8); /* speech, clear, point to multipoint */
    p.put(1, 1);      /* request to transmit */
    p.put(5, 4);      /* priority */
    p.put(0, 2);
    p.put(1, 2);      /* called party SSI */
    p.put(gssi, 24);
    p.put(0, 1);
    return p;
}

/* MAC-ACCESS with SSI, no optional field: the SDU, then fill to 92 bits. */
std::vector<uint8_t> mac_access(uint32_t ssi, const Bits &sdu)
{
    Bits m;

    m.put(0, 1);
    m.put(1, 1); /* fill bits */
    m.put(0, 1);
    m.put(0, 2); /* SSI */
    m.put(ssi, 24);
    m.put(0, 1);
    m.append(sdu, sdu.n);
    m.fillTo(92);
    return m.bits(92);
}

uint32_t scramb(void)
{
    return tetra_scramb_init(tetra_tmo_colour_code(MCC, MNC, CC));
}

void noise(std::vector<uint8_t> &out, unsigned n, uint32_t &rng)
{
    for (unsigned i = 0; i < n; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        out.push_back((uint8_t)(rng & 1u));
    }
}

/* One uplink slot: 34 bits of ramping, the control burst, noise to 510. */
void slot(std::vector<uint8_t> &out, const uint8_t *burst, unsigned n,
          uint32_t &rng)
{
    noise(out, 34, rng);
    out.insert(out.end(), burst, burst + n);
    noise(out, 510 - 34 - n, rng);
}

struct StatusSink
{
    unsigned events = 0;
    unsigned uplink = 0;
};

void onEvent(void *user, const struct tetra_event *ev)
{
    StatusSink *s = static_cast<StatusSink *>(user);

    s->events++;
    if (ev->uplink)
        s->uplink++;
    tetraStatus_onEvent(nullptr, ev); /* the firmware's own callback */
}

std::vector<uint8_t> uplinkStream(void)
{
    std::vector<uint8_t> out;
    uint32_t rng = 0x9e3779b9u;
    uint8_t t5[168];
    uint8_t burst[TETRA_CB_BITS];
    std::vector<uint8_t> t1 = mac_access(UL_SSI, u_setup(UL_GSSI));

    tetra_encode_sch(TETRA_SCH_HU, t1.data(), scramb(), t5);
    tetra_tmo_build_cb(burst, t5);

    noise(out, 3000, rng);
    for (unsigned i = 0; i < 3; i++)
        slot(out, burst, TETRA_CB_BITS, rng);
    noise(out, 3000, rng);
    return out;
}

/* Three slots of the burst alone, 510 bits each: the audio needs the slots
 * aligned to their symbol grid, not the noisy stream the bit-level check
 * uses. */
std::vector<uint8_t> uplinkSlots(unsigned slots)
{
    std::vector<uint8_t> out;
    uint32_t rng = 0x1234567u;
    uint8_t t5[168];
    uint8_t burst[TETRA_CB_BITS];
    std::vector<uint8_t> t1 = mac_access(UL_SSI, u_setup(UL_GSSI));

    tetra_encode_sch(TETRA_SCH_HU, t1.data(), scramb(), t5);
    tetra_tmo_build_cb(burst, t5);

    for (unsigned i = 0; i < slots; i++)
        slot(out, burst, TETRA_CB_BITS, rng);
    return out;
}

/*
 * The same burst as FM discriminator audio at 48 kHz, which is what the radio
 * hands to TetraDemod.c: pi/4-DQPSK, RRC 0.35, the phase integrated per
 * symbol and the per-sample phase step written as instantaneous frequency.
 * The mapping is libtetra's (tools/tetra_gen.c: dibit 0..3 -> +1, +3, -1, -3
 * quarter turns) and the amplitude window is the reference one (silence over
 * the 34-bit guard and between slots), so what is generated here is the
 * waveform of the burst checkBurst() decodes at bit level.
 */
double rrc(double t)
{
    const double a = 0.35;
    double x;

    if (std::fabs(t) < 1e-9)
        return 1.0 - a + 4.0 * a / M_PI;
    if (std::fabs(std::fabs(t) - 1.0 / (4.0 * a)) < 1e-9) {
        x = (1.0 + 2.0 / M_PI) * std::sin(M_PI / (4.0 * a)) +
            (1.0 - 2.0 / M_PI) * std::cos(M_PI / (4.0 * a));
        return a / std::sqrt(2.0) * x;
    }
    x = std::sin(M_PI * t * (1.0 - a)) +
        4.0 * a * t * std::cos(M_PI * t * (1.0 + a));
    return x / (M_PI * t * (1.0 - 16.0 * a * a * t * t));
}

std::vector<int16_t> uplinkAudio(void)
{
    /*
     * Modulated at 144 kHz (8 samples per symbol, the rate the demodulator
     * interpolates to) and then averaged down to 48 kHz: the discriminator of
     * a baseband generated at 2.67 samples per symbol is aliased, and the
     * demodulator integrates over exactly one symbol.
     */
    const double fsHi = 144000.0;
    const double fs = TETRA_DEMOD_IN_RATE;
    const double symrate = 18000.0;
    const double sps = fsHi / symrate;
    const unsigned span = 4;

    /* Symbols: the three slots, the burst in symbols 16..251 of each 255 */
    std::vector<uint8_t> bits = uplinkSlots(24);
    std::vector<double> amp;
    std::vector<std::complex<double>> sym;
    std::complex<double> phase(1.0, 0.0);
    static const int dphi[4] = { 1, 3, -1, -3 };

    for (unsigned i = 0; i + 1 < bits.size(); i += 2) {
        unsigned k = (i / 2) % 255;
        double a = (k >= 16 && k <= 251) ? 1.0 : 0.0;

        phase *= std::exp(std::complex<double>(0.0,
                            M_PI / 4.0 * dphi[(bits[i] << 1) | bits[i + 1]]));
        sym.push_back(phase);
        amp.push_back(a);
    }

    /* RRC at 144 kHz, then the per-sample phase step as a frequency */
    std::vector<std::complex<double>> base;
    for (size_t n = 0; ; n++) {
        double t = n / sps;
        long k0 = (long)std::floor(t) - span;
        long k1 = (long)std::ceil(t) + span;

        if ((size_t)(k1) >= sym.size())
            break;
        std::complex<double> acc(0.0, 0.0);
        for (long k = k0; k <= k1; k++) {
            if (k < 0 || (size_t)k >= sym.size())
                continue;
            acc += sym[k] * amp[k] * rrc(t - (double)k);
        }
        base.push_back(acc);
    }

    /*
     * Matched filter: a real receiver's channel filter is the same RRC the
     * transmitter used, and only the two together are Nyquist (a single RRC
     * leaves ISI at the symbol instants, which is what the demodulator's
     * equalizer then has to undo on a real radio).
     */
    std::vector<std::complex<double>> mf(base.size(), std::complex<double>(0, 0));
    for (size_t n = 0; n < base.size(); n++) {
        std::complex<double> acc(0.0, 0.0);

        for (int j = -4 * (int)sps; j <= 4 * (int)sps; j++) {
            long idx = (long)n + j;
            if (idx < 0 || (size_t)idx >= base.size())
                continue;
            acc += base[idx] * rrc((double)j / sps);
        }
        mf[n] = acc / sps;
    }

    std::vector<double> hz;
    for (size_t n = 0; n + 1 < mf.size(); n++) {
        std::complex<double> z = mf[n + 1] * std::conj(mf[n]);

        hz.push_back(std::arg(z) * fsHi / (2.0 * M_PI));
    }

    /*
     * Down to 48 kHz through a low-pass, not a box average: the
     * instantaneous frequency of an RRC-shaped pi/4-DQPSK signal has energy
     * well above 24 kHz (the phase steps are sharp), and decimating it
     * without a filter folds that back over the symbol.
     */
    const unsigned taps = 33;
    const double fc = 16000.0;
    std::vector<double> lpf(taps), hzF(hz.size(), 0.0);
    double sum = 0.0;
    for (unsigned i = 0; i < taps; i++) {
        double t = (double)i - (taps - 1) / 2.0;
        double s = (t == 0) ? 2 * fc / fsHi
                            : sin(2 * M_PI * fc / fsHi * t) / (M_PI * t);
        double w = 0.54 - 0.46 * cos(2 * M_PI * i / (taps - 1));
        lpf[i] = s * w;
        sum += lpf[i];
    }
    for (auto &c : lpf)
        c /= sum;
    for (size_t n = 0; n < hz.size(); n++) {
        double acc = 0.0;
        for (unsigned i = 0; i < taps; i++) {
            long idx = (long)n + (long)i - (long)((taps - 1) / 2);
            if (idx < 0 || (size_t)idx >= hz.size())
                continue;
            acc += hz[idx] * lpf[i];
        }
        hzF[n] = acc;
    }

    std::vector<int16_t> audio;
    /* 6.75 kHz (a 3pi/4 symbol step) maps to half scale, then the whole
     * stream is normalised to 90% so the RRC overshoot never clips (a clipped
     * discriminator destroys the phase differences the demodulator needs) */
    const double scale = 16384.0 / 6750.0;
    double peak = 0.0;

    for (size_t n = 0; n < hzF.size(); n += 3) {
        double v = hzF[n] * scale;

        audio.push_back((int16_t)std::lrint(v));
        if (std::fabs(v) > peak)
            peak = std::fabs(v);
    }
    /* A real channel is never digitally silent: a small floor keeps the
     * equalizer from training on exact zeros between bursts */
    if (peak > 29000.0) {
        double g = 29000.0 / peak;
        for (auto &v : audio)
            v = (int16_t)std::lrint(v * g);
        peak = 29000.0;
    }
    uint32_t rng = 0x5bd1e995u;
    for (auto &v : audio) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        v = (int16_t)(v + (int)(rng % 61) - 30);
    }
    return audio;
}

/* ---- 1. The two duplex tables ------------------------------------------- */

int checkConverter(void)
{
    unsigned bad = 0, checked = 0;

    for (uint8_t band = 0; band < 16; band++) {
        for (uint8_t duplex = 0; duplex < 8; duplex++) {
            for (int rev = 0; rev < 2; rev++) {
                const uint32_t dl[3] = { 392512500u, 851000000u, 430500000u };

                for (uint32_t hz : dl) {
                    uint32_t a = tetraUl_hzFromDownlink(hz, band, duplex,
                                                        rev != 0);
                    uint32_t b = tetra_uplink_hz(hz, band, duplex, rev != 0);

                    checked++;
                    if (a != b) {
                        printf("MISMATCH band %u duplex %u rev %d %u Hz: "
                               "firmware %u, libtetra %u\n",
                               band, duplex, rev, hz, a, b);
                        bad++;
                    }
                }
            }
        }
    }
    printf("duplex table: %u combinations, %u mismatches\n", checked, bad);
    return bad == 0 ? 0 : 1;
}

/* ---- 2. The cell event to the status and the session -------------------- */

int checkCell(void)
{
    int fails = 0;
    struct tetra_cell cell;
    struct tetra_event ev;
    struct tetraStatus ts;

    memset(&cell, 0, sizeof(cell));
    cell.mcc = MCC;
    cell.mnc = MNC;
    cell.colour_code = CC;
    cell.have_sysinfo = true;
    cell.location_area = 7;
    cell.main_carrier = 1210;
    cell.band = 3;
    cell.duplex_spacing = 2; /* 400 MHz, code 2: no split */
    cell.dl_freq_hz = 430500000u;
    cell.ul_freq_hz = 0; /* nothing published: the conversion has to fill it */

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_CELL;
    ev.u.cell = &cell;

    tetraStatus_reset();
    tetraStatus_onEvent(nullptr, &ev);
    tetra_getStatus(&ts);

    if (!ts.cell || (ts.mcc != MCC) || (ts.mnc != MNC) ||
        (ts.colourCode != CC)) {
        printf("FAIL the cell event did not reach the status\n");
        fails++;
    }
    if (ts.band != 3 || ts.duplexSpacing != 2 || ts.reverseOp) {
        printf("FAIL the raw duplex fields were not copied\n");
        fails++;
    }
    if (ts.dlFreq != 430500000u) {
        printf("FAIL the downlink is %u Hz\n", ts.dlFreq);
        fails++;
    }

    tetraUplink_reset();
    /* Code 2 at band 3 is a zero split: the uplink *is* the downlink, there is
     * nowhere to tune to, so the session says no uplink */
    if (tetraUplink_start(&ts, ts.dlFreq) != -1) {
        printf("FAIL a zero duplex split produced an uplink\n");
        fails++;
    }

    /* A reserved spacing (band 0, code 0) does not convert either */
    cell.band = 0;
    cell.duplex_spacing = 0;
    cell.dl_freq_hz = 410000000u;
    tetraStatus_onEvent(nullptr, &ev);
    tetra_getStatus(&ts);
    if (tetraUplink_start(&ts, ts.dlFreq) != -1) {
        printf("FAIL a reserved duplex spacing produced an uplink\n");
        fails++;
    }

    /* Code 0 at band 3: 10 MHz below the downlink */
    cell.band = 3;
    cell.dl_freq_hz = 430500000u;
    tetraStatus_onEvent(nullptr, &ev);
    tetra_getStatus(&ts);
    if (tetraUplink_start(&ts, ts.dlFreq) != 0) {
        printf("FAIL the duplex conversion did not produce an uplink\n");
        fails++;
    } else if (tetraUplink_freq() != 420500000u) {
        printf("FAIL the converted uplink is %u Hz\n", tetraUplink_freq());
        fails++;
    }

    /* What the cell publishes wins over the conversion */
    tetraUplink_reset();
    cell.duplex_spacing = 0;
    cell.ul_freq_hz = 415000000u;
    tetraStatus_onEvent(nullptr, &ev);
    tetra_getStatus(&ts);
    if (ts.ulFreq != 415000000u) {
        printf("FAIL the published uplink is %u Hz\n", ts.ulFreq);
        fails++;
    }
    if (tetraUplink_start(&ts, ts.dlFreq) != 0 ||
        tetraUplink_freq() != 415000000u) {
        printf("FAIL the session did not take the published uplink\n");
        fails++;
    }
    tetraUplink_reset();

    printf("cell event: %s\n", fails == 0 ? "ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}

/* ---- 3. A real MS burst through the receiver and the status -------------- */

/*
 * Feed discriminator audio to the firmware's demodulator and the receiver,
 * with the cell seeded, the way OpMode_TETRA does when the operator has the
 * "Cell uplink" row on. Returns the number of uplink events seen; the status
 * is left where tetra_getStatus() reads it.
 */
unsigned runAudio(const int16_t *audio, size_t n)
{
    static struct tetraDemod demod;
    static struct tetra_tmo_rx rx;
    static int8_t soft[1440 * 3 / 4 + 8];
    StatusSink sink;
    struct tetra_rx_config cfg;
    uint32_t jumps = 0;
    size_t off = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = onEvent;
    cfg.user = &sink;
    cfg.promiscuous = true;

    tetraStatus_reset();
    tetraDemod_init(&demod);
    tetra_tmo_init(&rx, &cfg);
    tetra_tmo_set_uplink_cell(&rx, MCC, MNC, CC);

    while (off < n) {
        size_t k = (n - off < 1440) ? (n - off) : 1440;
        size_t sym = tetraDemod_process(&demod, audio + off, k, soft);
        uint32_t j = tetraDemod_reacquisitions(&demod);

        if (j != jumps) {
            jumps = j;
            tetra_tmo_init(&rx, &cfg);
            tetra_tmo_set_uplink_cell(&rx, MCC, MNC, CC);
        }
        tetra_tmo_push_soft(&rx, soft, sym);
        ticks += 30;
        off += k;
    }
    return sink.uplink;
}

int checkAudio(const char *path)
{
    std::vector<int16_t> audio = uplinkAudio();
    struct tetraStatus ts;

    if (path != nullptr) {
        FILE *f = fopen(path, "wb");
        if (f == nullptr) {
            perror(path);
            return 1;
        }
        fwrite(audio.data(), sizeof(int16_t), audio.size(), f);
        fclose(f);
        printf("audio: %s, %zu samples at %d Hz\n", path, audio.size(),
               TETRA_DEMOD_IN_RATE);
    }

    unsigned uplink = runAudio(audio.data(), audio.size());
    tetra_getStatus(&ts);
    printf("audio: %u uplink events through TetraDemod, status uplink %d "
           "ssi %lu\n", uplink, (int)ts.uplink, (unsigned long)ts.uplinkSs);

    if (uplink == 0 || !ts.uplink || ts.uplinkSs != UL_SSI) {
        printf("FAIL the synthesised MS burst did not decode through the "
               "demodulator\n");
        return 1;
    }
    printf("audio: the firmware's status shows UL %lu through the demodulator\n",
           (unsigned long)ts.uplinkSs);
    return 0;
}

int checkBurst(void)
{
    static struct tetra_tmo_rx rx;
    StatusSink sink;
    struct tetra_rx_config cfg;
    struct tetraStatus ts;
    std::vector<uint8_t> stream = uplinkStream();

    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = onEvent;
    cfg.user = &sink;
    cfg.promiscuous = true;

    tetraStatus_reset();
    tetra_tmo_init(&rx, &cfg);
    tetra_tmo_set_uplink_cell(&rx, MCC, MNC, CC);
    tetra_tmo_push_bits(&rx, stream.data(), stream.size());

    tetra_getStatus(&ts);
    printf("burst: %u events, %u from the uplink, status uplink %d ssi %lu\n",
           sink.events, sink.uplink, (int)ts.uplink,
           (unsigned long)ts.uplinkSs);

    if (sink.uplink == 0) {
        printf("FAIL the control uplink burst was not recognised\n");
        return 1;
    }
    if (!ts.uplink || ts.uplinkSs != UL_SSI) {
        printf("FAIL the status did not record the sending MS (want %u)\n",
               UL_SSI);
        return 1;
    }
    printf("burst: the firmware's status shows UL %lu\n",
           (unsigned long)ts.uplinkSs);
    return 0;
}

/* ---- A capture, when there is one --------------------------------------- */

int checkCapture(const char *path, const char *cellarg)
{
    unsigned mcc = 0, mnc = 0, cc = 0;

    if (sscanf(cellarg, "%u:%u:%u", &mcc, &mnc, &cc) != 3) {
        fprintf(stderr, "cell must be mcc:mnc:cc, got %s\n", cellarg);
        return 2;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return 1;
    }

    char hdr[12];
    if (fread(hdr, 1, 12, f) == 12 && memcmp(hdr, "RIFF", 4) == 0) {
        char id[4];
        uint32_t len;

        while (fread(id, 1, 4, f) == 4 && fread(&len, 4, 1, f) == 1) {
            if (memcmp(id, "data", 4) == 0)
                break;
            fseek(f, len, SEEK_CUR);
        }
    } else {
        fseek(f, 0, SEEK_SET);
    }

    static struct tetraDemod demod;
    static struct tetra_tmo_rx rx;
    StatusSink sink;
    struct tetra_rx_config cfg;
    static int16_t buf[1440];
    static int8_t soft[1440 * 3 / 4 + 8];
    uint32_t jumps = 0;
    size_t n;
    unsigned blocks = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = onEvent;
    cfg.user = &sink;
    cfg.promiscuous = true;

    tetraStatus_reset();
    tetraDemod_init(&demod);
    tetra_tmo_init(&rx, &cfg);
    tetra_tmo_set_uplink_cell(&rx, (uint16_t)mcc, (uint16_t)mnc, (uint8_t)cc);

    while ((n = fread(buf, 2, 1440, f)) > 0) {
        size_t k = tetraDemod_process(&demod, buf, n, soft);
        uint32_t j = tetraDemod_reacquisitions(&demod);

        if (j != jumps) {
            jumps = j;
            tetra_tmo_init(&rx, &cfg);
            tetra_tmo_set_uplink_cell(&rx, (uint16_t)mcc, (uint16_t)mnc,
                                      (uint8_t)cc);
        }
        tetra_tmo_push_soft(&rx, soft, k);
        ticks += 30;
        blocks++;
    }
    fclose(f);

    struct tetraStatus ts;
    tetra_getStatus(&ts);
    printf("capture: %u blocks, %u events (%u uplink)\n", blocks, sink.events,
           sink.uplink);
    printf("sync %d cell %d %u-%u CC %u uplink %d ssi %lu\n", (int)ts.sync,
           (int)ts.cell, ts.mcc, ts.mnc, ts.colourCode, (int)ts.uplink,
           (unsigned long)ts.uplinkSs);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "-u") == 0)
        return checkCapture(argv[3], argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--bits") == 0) {
        std::vector<uint8_t> b = uplinkStream();
        FILE *f = fopen(argv[2], "wb");
        if (f == NULL) { perror(argv[2]); return 1; }
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
        printf("bits: %s, %zu\n", argv[2], b.size());
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--audio") == 0) {
        /* Write the synthesised discriminator audio and decode it */
        int rc = checkAudio(argv[2]);
        return rc;
    }

    int fails = 0;

#if TETRA_UPLINK_DETECT
    printf("libtetra built with TETRA_UPLINK_DETECT on\n");
#else
    printf("libtetra built with TETRA_UPLINK_DETECT off\n");
#endif
#if TETRA_UPLINK_DECODE
    printf("libtetra built with TETRA_UPLINK_DECODE on\n");
#else
    printf("libtetra built with TETRA_UPLINK_DECODE off\n");
#endif
    fails += checkConverter();
    fails += checkCell();

#if TETRA_UPLINK_DECODE
    fails += checkBurst();
#else
    printf("burst: not built, TETRA_UPLINK_DECODE is off\n");
#endif

    printf("%s\n", fails == 0 ? "all checks passed" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
