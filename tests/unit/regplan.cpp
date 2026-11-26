/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The BK4819 configuration of every C62 mode, checked off the radio.
 *
 * What these tests are for
 * ------------------------
 * A mode's chip configuration used to be the side effect of calling setters in
 * the driver's own order, spread over radio_setOpmode(), radio_enableRx(),
 * radio_updateConfiguration() and the spectrum analyser.  Nothing about it could
 * be checked without the radio in the hand, and one defect hid in there for
 * weeks: DMR ran the 25 kHz channel mode -- where the chip doubles the IF filter
 * to 9 kHz -- while the stock firmware runs its 4FSK at 12.5 kHz with 4.5 kHz
 * filters.  On the air that is a dancing colour code and corrupt audio; in a
 * test it never appeared.
 *
 * platform/drivers/baseband/radioPlan.c now holds that configuration as data.
 * These tests build every mode's plan and compare it, write for write, with the
 * sequence the driver issued before the plan existed.
 *
 * How the reference sequence is kept honest
 * -----------------------------------------
 * The driver's setters (bk4819_setAudioMode(), setBandwidth(), ...) are compiled
 * here, unmodified, against a recording fake bus: the BK4819 driver talks
 * through a const struct spiDevice * (spi_acquire/send/receive/release), so the
 * fake is one transfer function over a 128-register file.  A read-modify-write
 * shows up as a read followed by a write, at 16 bits each, exactly as on the
 * real bus.
 *
 * baseline*() below is the pre-refactor code of radio_C62.cpp at 4b8af88d,
 * copied verbatim: the constants, the order of the calls and the branches are
 * the golden table.  If a setter's mask or value changes, only the baseline
 * moves and the test fails, which is the point -- the plan is not allowed to
 * drift away from what the chip is actually told.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "drivers/baseband/BK4819.h"
#include "drivers/baseband/BK4819_priv.h"
#include "drivers/baseband/radioPlan.h"
}

/* ------------------------------------------------------------------ */
/* A BK4819 that records what it is asked to do                        */
/* ------------------------------------------------------------------ */

struct BusAccess {
    bool write;
    uint8_t reg;
    uint16_t value;

    bool operator==(const BusAccess &o) const
    {
        return (write == o.write) && (reg == o.reg) && (value == o.value);
    }
};

struct FakeBus {
    uint16_t regs[128];         /* Register file, 16 bits each */
    std::vector<BusAccess> log; /* Every access, in order */
    uint8_t cmd = 0;            /* Last command byte: address | R/W */
};

static FakeBus g_bus;

/*
 * The chip's serial protocol, as BK4819.c speaks it: one command byte, then two
 * bytes of data in either direction, separated by chip select (which the driver
 * toggles through the gpio pin, so a transaction is one or two transfers here).
 */
static int fakeTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    FakeBus *bus = static_cast<FakeBus *>(const_cast<void *>(dev->priv));
    const uint8_t reg = bus->cmd & 0x7f;

    if ((txBuf != nullptr) && (size == 1)) {
        bus->cmd = *static_cast<const uint8_t *>(txBuf);
        return 0;
    }

    if ((txBuf != nullptr) && (size == 2)) {
        const uint8_t *p = static_cast<const uint8_t *>(txBuf);
        const uint16_t value = (uint16_t)((p[0] << 8) | p[1]);

        bus->regs[reg] = value;
        bus->log.push_back({ true, reg, value });
        return 0;
    }

    if ((rxBuf != nullptr) && (size == 2)) {
        const uint16_t value = bus->regs[reg];
        uint8_t *p = static_cast<uint8_t *>(rxBuf);

        p[0] = (uint8_t)(value >> 8);
        p[1] = (uint8_t)(value & 0xff);
        bus->log.push_back({ false, reg, value });
        return 0;
    }

    return -1;
}

/* Chip select and the other pins the driver touches: nothing to drive here */
static int fakePinMode(const struct gpioDev *, const uint8_t, const uint16_t)
{
    return 0;
}
static void fakePinSet(const struct gpioDev *, const uint8_t)
{
}
static void fakePinClear(const struct gpioDev *, const uint8_t)
{
}
static bool fakePinRead(const struct gpioDev *, const uint8_t)
{
    return false;
}

static const struct gpioApi g_pinApi = {
    fakePinMode,
    fakePinSet,
    fakePinClear,
    fakePinRead,
};
static const struct gpioDev g_pinPort = { &g_pinApi, nullptr };

/* No spiFast: bk4819_tuneCached() is not exercised by a plan */
static const struct spiDevice g_spi = { fakeTransfer, &g_bus, nullptr };
static const struct BK4819 g_chip = { &g_spi, nullptr, { &g_pinPort, 0 } };

/* The register file right after bk4819_init(), which is what a mode starts from */
struct RegFile {
    uint16_t regs[128];
};

static RegFile initRegFile()
{
    RegFile f;

    std::memset(g_bus.regs, 0, sizeof(g_bus.regs));
    g_bus.log.clear();
    bk4819_init(&g_chip); /* 250 ms of microphone bias settling, once */

    std::memcpy(f.regs, g_bus.regs, sizeof(f.regs));
    return f;
}

static RegFile g_init;

static void resetTo(const RegFile &f)
{
    std::memcpy(g_bus.regs, f.regs, sizeof(g_bus.regs));
    g_bus.log.clear();
    g_bus.cmd = 0;
}

static uint16_t regOf(uint8_t reg)
{
    return g_bus.regs[reg & 0x7f];
}

/* ------------------------------------------------------------------ */
/* Formatting, so a failure shows the two sequences side by side       */
/* ------------------------------------------------------------------ */

static std::string logText(const std::vector<BusAccess> &log)
{
    std::string s;

    for (size_t i = 0; i < log.size(); i++) {
        char line[64];

        std::snprintf(line, sizeof(line), "  [%2u] %s 0x%02x %s 0x%04x\n",
                      (unsigned int)i, log[i].write ? "write" : "read ",
                      log[i].reg, log[i].write ? "=" : "->", log[i].value);
        s += line;
    }

    return s;
}

static std::string planText(const struct radioPlan &plan)
{
    char buf[1024];

    return std::string(radioPlan_format(&plan, buf, sizeof(buf)));
}

static struct radioPlan planOf(enum radioPlanStage stage, enum opmode mode,
                               enum bandwidth bw, enum radioPlanDir dir,
                               const struct radioPlanOptions *opt)
{
    struct radioPlan plan;

    radioPlan_build(&plan, stage, mode, bw, dir, opt);
    return plan;
}

/* ------------------------------------------------------------------ */
/* The golden table: what the driver issued before the plan existed    */
/* ------------------------------------------------------------------ */

/*
 * Per-mode numbers, frozen.  They are the constants of radio_C62.cpp at
 * 4b8af88d, kept here so that a change to one of them has to be made twice,
 * deliberately.
 */
#define M17_TX_DEVIATION 0x582
#define DMR_TX_DEVIATION 0x476
#define AM_RX_AUDIO_GAIN1 1
#define AM_RX_AUDIO_GAIN2 0x3f

/* BK4819 filters as the driver names them, in enum bk4819_bandwidth order */
static void baselineMode(enum opmode mode, enum bandwidth bw,
                         const struct radioPlanOptions &opt)
{
    /* radio_setOpmode(): every mode but airband restores the FM profile first */
    if (mode != OPMODE_AIRBAND)
        bk4819_setModulation(&g_chip, true);

    if (mode == OPMODE_M17) {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_BYPASS);
        bk4819_setTxDeviation(&g_chip, true, M17_TX_DEVIATION);
        if (opt.wideRx) {
            bk4819_setBandwidth(&g_chip, BK4819_BW_25K);
            bk4819_setRfFilter(&g_chip, 7, 7);
        } else {
            bk4819_setBandwidth(&g_chip, BK4819_BW_12K5);
            bk4819_setRfFilter(&g_chip, 7, 4);
        }
        bk4819_setTxAudioFilter(&g_chip, 0);
        bk4819_setRxAudioGain(&g_chip, 1, 0x3f);
        bk4819_setMicGain(&g_chip, 0);
    } else if (mode == OPMODE_DMR) {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_BYPASS);
        bk4819_setTxDeviation(&g_chip, true, DMR_TX_DEVIATION);
        if (opt.dmr12k5)
            bk4819_setBandwidth(&g_chip, BK4819_BW_12K5);
        else
            bk4819_setBandwidth(&g_chip, BK4819_BW_25K);
        bk4819_setRfFilter(&g_chip, 7, 7);
        bk4819_setTxAudioFilter(&g_chip, 0);
        bk4819_setRxAudioGain(&g_chip, 1, 0x3f);
        bk4819_setMicGain(&g_chip, 0);
    } else if (mode == OPMODE_NXDN) {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_BYPASS);
        /* Added after the refactor on purpose (docs/RADIO_CONFIG.md): the 4FSK
         * modes used to inherit REG_40 from whichever mode ran before. */
        bk4819_setTxDeviation(&g_chip, true, M17_TX_DEVIATION);
        bk4819_setBandwidth(&g_chip, BK4819_BW_12K5);
        if (bw == BW_6_25)
            bk4819_setRfFilter(&g_chip, 3, 3);
        else
            bk4819_setRfFilter(&g_chip, 7, 7);
        bk4819_setTxAudioFilter(&g_chip, 0);
        bk4819_setRxAudioGain(&g_chip, 1, 0x3f);
        bk4819_setMicGain(&g_chip, 0);
    } else if (mode == OPMODE_DPMR) {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_BYPASS);
        bk4819_setTxDeviation(&g_chip, true, M17_TX_DEVIATION); /* intended */ 
        bk4819_setBandwidth(&g_chip, BK4819_BW_12K5);
        bk4819_setRfFilter(&g_chip, 7, 7);
        bk4819_setTxAudioFilter(&g_chip, 0);
        bk4819_setRxAudioGain(&g_chip, 1, 0x3f);
        bk4819_setMicGain(&g_chip, 0);
    } else if ((mode == OPMODE_P25) || (mode == OPMODE_YSF)
               || (mode == OPMODE_DSTAR)) {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_BYPASS);
        bk4819_setTxDeviation(&g_chip, true, M17_TX_DEVIATION); /* intended */
        bk4819_setBandwidth(&g_chip, BK4819_BW_12K5);
        bk4819_setRfFilter(&g_chip, 7, 7);
        bk4819_setTxAudioFilter(&g_chip, 0);
        bk4819_setRxAudioGain(&g_chip, 1, 0x3f);
        bk4819_setMicGain(&g_chip, 0);
    } else if (mode == OPMODE_TETRA) {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_BYPASS);
        bk4819_setBandwidth(&g_chip, BK4819_BW_25K);
        bk4819_setRfFilter(&g_chip, 7, 7);
        bk4819_setTxAudioFilter(&g_chip, 0);
        bk4819_setRxAudioGain(&g_chip, 1, 0x3f);
        bk4819_setMicGain(&g_chip, 0);
    } else if (mode == OPMODE_AIRBAND) {
        bk4819_setAmReceive(&g_chip,
                            (bw == BW_25) ? BK4819_AM_WIDE : BK4819_AM_NARROW);
        bk4819_setRxAudioGain(&g_chip, AM_RX_AUDIO_GAIN1, AM_RX_AUDIO_GAIN2);
    } else if (mode == OPMODE_WFM_BC) {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_VOICE);
        bk4819_setAfOutput(&g_chip, BK4819_AF_MUTE);
    } else {
        bk4819_setAudioMode(&g_chip, BK4819_AUDIO_VOICE);
    }
}

/* radio_enableRx(): the mode-dependent part of starting the receiver */
static void baselineRx(enum opmode mode, const struct radioPlanOptions &opt)
{
    const bool flat = (mode == OPMODE_M17) || (mode == OPMODE_DMR)
                   || (mode == OPMODE_TETRA) || (mode == OPMODE_NXDN)
                   || (mode == OPMODE_DPMR) || (mode == OPMODE_P25)
                   || (mode == OPMODE_YSF) || (mode == OPMODE_DSTAR);

    if (!flat)
        return;

    bk4819_setAfOutput(&g_chip, BK4819_AF_BYPASS);

    if ((mode == OPMODE_TETRA) && opt.tetraIfDcFilter)
        bk4819_maskSetRegister(&g_chip, 0x7E, 0x0007, 0x0006);
}

/* radio_updateConfiguration(): channel spacing and deviation, airband filter */
static void baselineUpdate(enum opmode mode, enum bandwidth bw,
                           const struct radioPlanOptions &opt)
{
    if (mode == OPMODE_FM) {
        switch (bw) {
            case BW_6_25:
                bk4819_setBandwidth(&g_chip, BK4819_BW_6K25);
                bk4819_setTxDeviation(&g_chip, true, opt.fmTxDevNarrow);
                break;

            case BW_12_5:
                bk4819_setBandwidth(&g_chip, BK4819_BW_12K5);
                bk4819_setTxDeviation(&g_chip, true, opt.fmTxDevNarrow);
                break;

            case BW_25:
                bk4819_setBandwidth(&g_chip, BK4819_BW_25K);
                bk4819_setTxDeviation(&g_chip, true, opt.fmTxDevWide);
                break;

            default:
                break;
        }
    }

    if (mode == OPMODE_AIRBAND)
        bk4819_setAmReceive(&g_chip,
                            (bw == BW_25) ? BK4819_AM_WIDE : BK4819_AM_NARROW);
}

/* radio_enableAfOutput() */
static void baselineAf(enum opmode mode)
{
    if (mode == OPMODE_WFM_BC)
        return;

    const bool flat = (mode == OPMODE_M17) || (mode == OPMODE_DMR)
                   || (mode == OPMODE_TETRA) || (mode == OPMODE_NXDN)
                   || (mode == OPMODE_DPMR) || (mode == OPMODE_P25)
                   || (mode == OPMODE_YSF) || (mode == OPMODE_DSTAR);

    if (mode == OPMODE_AIRBAND)
        bk4819_setAfOutput(&g_chip, BK4819_AF_AM);
    else if (flat)
        bk4819_setAfOutput(&g_chip, BK4819_AF_BYPASS);
    else
        bk4819_setAfOutput(&g_chip, BK4819_AF_FM);
}

/* radio_spectrumSweep(true) */
static void baselineSpectrum(const struct radioPlanOptions &opt)
{
    bk4819_setBandwidth(&g_chip, BK4819_BW_25K);
    bk4819_setRfFilter(&g_chip, 7, 7);
    bk4819_setAfOutput(&g_chip, BK4819_AF_MUTE);
    bk4819_maskSetRegister(&g_chip, 0x73, 0x0010, 0x0010);

    if (opt.spectrumAgcFix)
        bk4819_maskSetRegister(&g_chip, 0x7E, 0x8000, 0x8000);
}

/* ------------------------------------------------------------------ */

static const enum opmode allModes[] = {
    OPMODE_FM,    OPMODE_DMR,     OPMODE_M17,    OPMODE_TETRA,
    OPMODE_NXDN,  OPMODE_DPMR,    OPMODE_P25,    OPMODE_YSF,
    OPMODE_DSTAR, OPMODE_AIRBAND, OPMODE_WFM_BC,
};

static const enum bandwidth allBandwidths[] = { BW_6_25, BW_12_5, BW_25 };

/* Both build variants of the two settings that change a mode's filters */
static std::vector<struct radioPlanOptions> buildVariants()
{
    std::vector<struct radioPlanOptions> v;
    struct radioPlanOptions opt;

    for (int wideRx = 0; wideRx < 2; wideRx++) {
        for (int dmr12k5 = 0; dmr12k5 < 2; dmr12k5++) {
            for (int dcFilter = 0; dcFilter < 2; dcFilter++) {
                for (int agcFix = 0; agcFix < 2; agcFix++) {
                    radioPlan_defaultOptions(&opt);
                    opt.wideRx = (wideRx != 0);
                    opt.dmr12k5 = (dmr12k5 != 0);
                    opt.tetraIfDcFilter = (dcFilter != 0);
                    opt.spectrumAgcFix = (agcFix != 0);
                    opt.airTx = true;
                    opt.fmTxDevNarrow = 0x4d2;
                    opt.fmTxDevWide = 0x9a4;
                    v.push_back(opt);
                }
            }
        }
    }

    return v;
}

/* ------------------------------------------------------------------ */
/* 1. The plan is the sequence the driver used to issue                */
/* ------------------------------------------------------------------ */

TEST_CASE("Every mode's plan is the sequence the driver issued", "[regplan]")
{
    g_init = initRegFile();

    for (const auto &opt : buildVariants()) {
        for (enum opmode mode : allModes) {
            for (enum bandwidth bw : allBandwidths) {
                const std::string name = std::string(radioPlan_modeName(mode))
                                       + " bw" + std::to_string((int)bw);

                /* The mode profile: radio_setOpmode(), both sides at once */
                resetTo(g_init);
                baselineMode(mode, bw, opt);
                const std::vector<BusAccess> wantMode = g_bus.log;
                const RegFile wantModeRegs = [&] {
                    RegFile f;
                    std::memcpy(f.regs, g_bus.regs, sizeof(f.regs));
                    return f;
                }();

                resetTo(g_init);
                struct radioPlan plan = planOf(RADIO_PLAN_STAGE_MODE, mode, bw,
                                               RADIO_PLAN_BOTH, &opt);
                radioPlan_apply(&plan, &g_chip);

                INFO(planText(plan));
                INFO(name << ": the driver issued\n" << logText(wantMode));
                INFO(name << ": the plan issues\n" << logText(g_bus.log));
                REQUIRE(g_bus.log == wantMode);
                REQUIRE(std::memcmp(g_bus.regs, wantModeRegs.regs,
                                    sizeof(g_bus.regs))
                        == 0);
                REQUIRE(plan.dir == RADIO_PLAN_BOTH);
                REQUIRE(plan.mode == mode);

                /* The receive path: radio_enableRx() */
                resetTo(g_init);
                baselineRx(mode, opt);
                const std::vector<BusAccess> wantRx = g_bus.log;

                resetTo(g_init);
                plan = planOf(RADIO_PLAN_STAGE_RX, mode, bw, RADIO_PLAN_RX,
                              &opt);
                radioPlan_apply(&plan, &g_chip);

                INFO(planText(plan));
                INFO(name << ": the driver issued\n" << logText(wantRx));
                INFO(name << ": the plan issues\n" << logText(g_bus.log));
                REQUIRE(g_bus.log == wantRx);

                /* Reconfiguration: radio_updateConfiguration() */
                resetTo(g_init);
                baselineUpdate(mode, bw, opt);
                const std::vector<BusAccess> wantUpdate = g_bus.log;

                resetTo(g_init);
                plan = planOf(RADIO_PLAN_STAGE_UPDATE, mode, bw,
                              RADIO_PLAN_BOTH, &opt);
                radioPlan_apply(&plan, &g_chip);

                INFO(planText(plan));
                INFO(name << ": the driver issued\n" << logText(wantUpdate));
                INFO(name << ": the plan issues\n" << logText(g_bus.log));
                REQUIRE(g_bus.log == wantUpdate);

                /* Opening the audio path: radio_enableAfOutput() */
                resetTo(g_init);
                baselineAf(mode);
                const std::vector<BusAccess> wantAf = g_bus.log;

                resetTo(g_init);
                plan = planOf(RADIO_PLAN_STAGE_AF, mode, bw, RADIO_PLAN_RX,
                              &opt);
                radioPlan_apply(&plan, &g_chip);

                INFO(planText(plan));
                INFO(name << ": the driver issued\n" << logText(wantAf));
                INFO(name << ": the plan issues\n" << logText(g_bus.log));
                REQUIRE(g_bus.log == wantAf);
            }
        }

        /* The spectrum profile is not per mode, so it is checked once */
        resetTo(g_init);
        baselineSpectrum(opt);
        const std::vector<BusAccess> wantSpec = g_bus.log;

        resetTo(g_init);
        struct radioPlan plan = planOf(RADIO_PLAN_STAGE_SPECTRUM, OPMODE_FM,
                                       BW_25, RADIO_PLAN_RX, &opt);
        radioPlan_apply(&plan, &g_chip);

        INFO(planText(plan));
        INFO("the driver issued\n" << logText(wantSpec));
        INFO("the plan issues\n" << logText(g_bus.log));
        REQUIRE(g_bus.log == wantSpec);
    }
}

TEST_CASE("Keying writes no part of a mode's configuration", "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    g_init = initRegFile();

    /* The way in which the DMR deviation could be "fixed" at the wrong time */
    for (enum opmode mode : allModes) {
        for (enum bandwidth bw : allBandwidths) {
            for (int stage = RADIO_PLAN_STAGE_MODE;
                 stage <= RADIO_PLAN_STAGE_SPECTRUM; stage++) {
                struct radioPlan plan = planOf((enum radioPlanStage)stage, mode,
                                               bw, RADIO_PLAN_TX, &opt);

                INFO(planText(plan));
                REQUIRE(plan.count == 0);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 2. The values themselves                                            */
/* ------------------------------------------------------------------ */

/* Apply a mode's profile to the post-init register file and look at the chip */
static struct radioPlan applyMode(enum opmode mode, enum bandwidth bw,
                                  const struct radioPlanOptions *opt)
{
    struct radioPlan plan = planOf(RADIO_PLAN_STAGE_MODE, mode, bw,
                                   RADIO_PLAN_BOTH, opt);

    resetTo(g_init);
    radioPlan_apply(&plan, &g_chip);

    return plan;
}

/*
 * Fields of the registers these tests read.  They are the driver's own macros,
 * used here as the reader of the chip's state rather than as the writer.
 */
static uint16_t rg43Channel(uint16_t v)
{
    return v & 0x0030;
}
static uint16_t rg43RfBw(uint16_t v)
{
    return v & 0x7000;
}
static uint16_t rg43RfBwWeak(uint16_t v)
{
    return v & 0x0e00;
}
static uint16_t rg43TxLpf2(uint16_t v)
{
    return v & 0x01c0;
}
static uint16_t rg7eRxDc(uint16_t v)
{
    return v & 0x0007;
}
static uint16_t rg7eTxDc(uint16_t v)
{
    return v & 0x0038;
}
static uint16_t rg47Af(uint16_t v)
{
    return v & 0x0f00;
}
static uint16_t rg48Gain1(uint16_t v)
{
    return v & 0x0c00;
}
static uint16_t rg48Gain2(uint16_t v)
{
    return v & 0x03f0;
}
static uint16_t rg40Dev(uint16_t v)
{
    return v & 0x0fff;
}

TEST_CASE("REG_43: channel mode and IF filters, per mode", "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    g_init = initRegFile();

    /*
     * FM: a mode change always leaves the chip in the 12.5 kHz channel mode,
     * because it goes through bk4819_setAudioMode() and that is the spacing the
     * chip's voice profile asks for; the operator's spacing is written by the
     * reconfiguration that follows.
     */
    applyMode(OPMODE_FM, BW_25, &opt);
    REQUIRE(rg43Channel(regOf(0x43)) == BK4819_REG43_BW_MODE(0));
    REQUIRE(rg43RfBw(regOf(0x43)) == BK4819_REG43_RF_BW(4));

    struct radioPlan upd = planOf(RADIO_PLAN_STAGE_UPDATE, OPMODE_FM, BW_25,
                                  RADIO_PLAN_BOTH, &opt);
    resetTo(g_init);
    applyMode(OPMODE_FM, BW_25, &opt);
    radioPlan_apply(&upd, &g_chip);
    INFO(planText(upd));
    REQUIRE(rg43Channel(regOf(0x43)) == BK4819_REG43_BW_MODE(2));
    REQUIRE(rg43RfBw(regOf(0x43)) == BK4819_REG43_RF_BW(3));
    REQUIRE(rg43RfBwWeak(regOf(0x43)) == 0);

    upd = planOf(RADIO_PLAN_STAGE_UPDATE, OPMODE_FM, BW_6_25, RADIO_PLAN_BOTH,
                 &opt);
    resetTo(g_init);
    applyMode(OPMODE_FM, BW_6_25, &opt);
    radioPlan_apply(&upd, &g_chip);
    INFO(planText(upd));
    REQUIRE(rg43Channel(regOf(0x43)) == BK4819_REG43_BW_MODE(1));
    REQUIRE(rg43RfBw(regOf(0x43)) == BK4819_REG43_RF_BW(3));
    REQUIRE(rg43RfBwWeak(regOf(0x43)) == BK4819_REG43_RF_BW_WEAK(3));
    REQUIRE(rg43TxLpf2(regOf(0x43)) == BK4819_REG43_AF_TX_LPF2_BW(1));

    /*
     * DMR: this is the defect the plan was built to make visible.  The stock
     * firmware runs its 4FSK in the 12.5 kHz channel mode with the 4.5 kHz
     * filters.  The build's default is the 25 kHz mode, where the chip doubles
     * the 4.5 kHz IF filter to 9 kHz -- wider than the channel.
     */
    struct radioPlan dmr = applyMode(OPMODE_DMR, BW_12_5, &opt);
    INFO(planText(dmr));
    REQUIRE(regOf(0x43) == 0x7e28); /* C62_DMR_12K5 off: 25 kHz, 9 kHz IF */
    REQUIRE(rg43Channel(regOf(0x43)) == BK4819_REG43_BW_MODE(2));

    opt.dmr12k5 = true;
    dmr = applyMode(OPMODE_DMR, BW_12_5, &opt);
    INFO(planText(dmr));
    REQUIRE(rg43Channel(regOf(0x43)) == BK4819_REG43_BW_MODE(0));
    REQUIRE(rg43RfBw(regOf(0x43)) == BK4819_REG43_RF_BW(7)); /* 4.5 kHz */
    REQUIRE(rg43RfBwWeak(regOf(0x43)) == BK4819_REG43_RF_BW_WEAK(7));
    REQUIRE(rg43TxLpf2(regOf(0x43)) == 0); /* 3 kHz, the flat TX path */
    /*
     * 0x7e08, not 0x7e48: the driver sets the TX audio LPF2 to its 3 kHz value
     * after the IF filters, and REG_43<8:6> is one of the fields that word
     * covers.  See the mirror test below -- the chip and the word the dump
     * reports differ in exactly this field.
     */
    REQUIRE(regOf(0x43) == 0x7e08);
    opt.dmr12k5 = false;

    /* M17: 12.5 kHz, 4.5 kHz IF, its own filters; C62_WIDE_RX widens it */
    opt.wideRx = false;
    applyMode(OPMODE_M17, BW_12_5, &opt);
    REQUIRE(regOf(0x43) == 0x7808);
    REQUIRE(rg43Channel(regOf(0x43)) == BK4819_REG43_BW_MODE(0));
    REQUIRE(rg43RfBw(regOf(0x43)) == BK4819_REG43_RF_BW(7));
    REQUIRE(rg43RfBwWeak(regOf(0x43)) == BK4819_REG43_RF_BW_WEAK(4));

    opt.wideRx = true;
    applyMode(OPMODE_M17, BW_12_5, &opt);
    REQUIRE(regOf(0x43) == 0x7e28);
    REQUIRE(rg43Channel(regOf(0x43)) == BK4819_REG43_BW_MODE(2));

    /* TETRA: 25 kHz channels, widest filters, the flat path its EQ was set on */
    applyMode(OPMODE_TETRA, BW_25, &opt);
    REQUIRE(regOf(0x43) == 0x7e28);

    /* NXDN: the IF filter follows the channel spacing, the mode does not */
    applyMode(OPMODE_NXDN, BW_6_25, &opt);
    REQUIRE(regOf(0x43) == 0x3608); /* 12.5 kHz mode, 3 kHz IF (RF 3,3) */
    applyMode(OPMODE_NXDN, BW_12_5, &opt);
    REQUIRE(regOf(0x43) == 0x7e08);

    /* dPMR, P25, YSF and D-STAR: 12.5 kHz mode, 4.5 kHz IF */
    for (enum opmode m :
         { OPMODE_DPMR, OPMODE_P25, OPMODE_YSF, OPMODE_DSTAR }) {
        INFO(radioPlan_modeName(m));
        applyMode(m, BW_12_5, &opt);
        REQUIRE(regOf(0x43) == 0x7e08);
    }

    /* Airband: the AM detector's IF follows the channel spacing */
    applyMode(OPMODE_AIRBAND, BW_12_5, &opt);
    REQUIRE(regOf(0x43) == 0x0048); /* 12.5 kHz mode, 1.7 kHz IF */
    applyMode(OPMODE_AIRBAND, BW_25, &opt);
    REQUIRE(regOf(0x43) == 0x3628); /* 25 kHz mode, 3 kHz IF -> 6 kHz */
}

TEST_CASE("REG_7E: the baseband high-pass is off for the digital modes",
          "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    g_init = initRegFile();

    /*
     * REG_7E<2:0> is the RX IF DC filter -- a high-pass whose corner sits just
     * below DC.  A 4FSK slicer reads absolute levels, so it has to stay off for
     * the digital modes; TETRA decodes phase differences and turns it on, which
     * is the one difference between its flat path and the others'.
     */
    for (enum opmode m : { OPMODE_M17, OPMODE_DMR, OPMODE_NXDN, OPMODE_DPMR,
                           OPMODE_P25, OPMODE_YSF, OPMODE_DSTAR }) {
        INFO(radioPlan_modeName(m));
        applyMode(m, BW_12_5, &opt);
        REQUIRE(rg7eRxDc(regOf(0x7E)) == 0);
        REQUIRE(rg7eTxDc(regOf(0x7E)) == 0); /* no TX DC filter either */
    }

    applyMode(OPMODE_TETRA, BW_25, &opt);
    /* The mode profile clears it; the receive path puts 0b110 back */
    REQUIRE(rg7eRxDc(regOf(0x7E)) == 0);
    struct radioPlan rx = planOf(RADIO_PLAN_STAGE_RX, OPMODE_TETRA, BW_25,
                                 RADIO_PLAN_RX, &opt);
    radioPlan_apply(&rx, &g_chip);
    INFO(planText(rx));
    REQUIRE(rg7eRxDc(regOf(0x7E)) == 6);

    /* Without the option the IF DC filter stays off, as the bypass leaves it */
    opt.tetraIfDcFilter = false;
    rx = planOf(RADIO_PLAN_STAGE_RX, OPMODE_TETRA, BW_25, RADIO_PLAN_RX, &opt);
    resetTo(g_init);
    applyMode(OPMODE_TETRA, BW_25, &opt);
    radioPlan_apply(&rx, &g_chip);
    REQUIRE(rx.count == 1);
    REQUIRE(rg7eRxDc(regOf(0x7E)) == 0);

    /*
     * Voice puts back the chip's own DC filters (TX 5, RX 6): an analog
     * receiver wants its DC blocked, and FM has no constellation to protect.
     */
    opt.tetraIfDcFilter = true;
    applyMode(OPMODE_FM, BW_12_5, &opt);
    REQUIRE(rg7eRxDc(regOf(0x7E)) == BK4819_DEFAULT_RX_DCF_BW);
    REQUIRE(rg7eTxDc(regOf(0x7E)) == BK4819_REG7E_TX_DCF_BW(5));

    /* AGC not fixed: REG_7E<15> belongs to the spectrum, not to a mode */
    REQUIRE((regOf(0x7E) & 0x8000) == 0);
}

TEST_CASE("The audio path: bypass for the digital modes, voice for FM",
          "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    g_init = initRegFile();

    /*
     * REG_2B<8> is the RX de-emphasis, REG_31<3> the compander, REG_4B<5> the
     * ALC and REG_47<1:0> the AF TX filter bypass: the four things a flat,
     * DC-coupled 4FSK baseband cannot have.
     */
    for (enum opmode m :
         { OPMODE_M17, OPMODE_DMR, OPMODE_NXDN, OPMODE_DPMR, OPMODE_P25,
           OPMODE_YSF, OPMODE_DSTAR, OPMODE_TETRA }) {
        INFO(radioPlan_modeName(m));
        applyMode(m, BW_12_5, &opt);

        REQUIRE((regOf(0x2B) & BK4819_REG2B_AF_FILTERS_DISABLE)
                == BK4819_REG2B_AF_FILTERS_DISABLE);
        REQUIRE((regOf(0x2B) & BK4819_REG2B_RX_DEEMPHASIS_DISABLE) != 0);
        REQUIRE((regOf(0x4B) & BK4819_REG4B_ALC_DISABLE) != 0);
        REQUIRE((regOf(0x47) & BK4819_REG47_BYPASS) == BK4819_REG47_BYPASS);
        REQUIRE((regOf(0x70) & 0x8080) == 0);
    }

    /* FM: de-emphasis on, the AF filters on, the ALC on, no audio bypass */
    applyMode(OPMODE_FM, BW_12_5, &opt);
    REQUIRE((regOf(0x2B) & BK4819_REG2B_AF_FILTERS_DISABLE) == 0);
    REQUIRE((regOf(0x2B) & BK4819_REG2B_RX_DEEMPHASIS_DISABLE) == 0);
    REQUIRE((regOf(0x4B) & BK4819_REG4B_ALC_DISABLE) == 0);
    REQUIRE((regOf(0x47) & BK4819_REG47_BYPASS) == 0);
    /* and the 1:2 expander of the voice profile */
    REQUIRE((regOf(0x28) & BK4819_REG28_EXPANDER_MASK)
            == BK4819_REG28_EXPANDER_1_2);

    /*
     * REG_31<3>, the compander, is off in every mode of this driver: the voice
     * profile of bk4819_setAudioMode() clears it and nothing in the radio sets
     * it back, so what an FM operator hears is the 1:2 expander and not a
     * compander.  Frozen here because the plan must not change it by accident.
     */
    for (enum opmode m : allModes) {
        INFO(radioPlan_modeName(m));
        applyMode(m, BW_12_5, &opt);
        REQUIRE((regOf(0x31) & BK4819_REG31_COMPANDER) == 0);
    }

    /* Airband AM: no de-emphasis, no expander, the AM detector at the output */
    applyMode(OPMODE_AIRBAND, BW_12_5, &opt);
    /* Bit 8 belongs to both masks: it is the de-emphasis, and AM turns it off */
    REQUIRE((regOf(0x2B) & BK4819_REG2B_RX_DEEMPHASIS_DISABLE) != 0);
    REQUIRE((regOf(0x28) & BK4819_REG28_EXPANDER_MASK) == 0);
    REQUIRE(rg47Af(regOf(0x47)) == BK4819_REG47_AF_OUTPUT(BK4819_AF_AM));
    REQUIRE((regOf(0x13)) == BK4819_REG13_AGC_TOP_AM);
    REQUIRE((regOf(0x4A)) == BK4819_REG4A_RX_AM);

    /* The FM profile every non-airband mode restores first */
    applyMode(OPMODE_FM, BW_12_5, &opt);
    REQUIRE((regOf(0x13)) == BK4819_REG13_AGC_TOP_FM);
    REQUIRE((regOf(0x4A)) == BK4819_REG4A_RX_FM);
    REQUIRE((regOf(0x28) & BK4819_REG28_EXPANDER_MASK)
            == BK4819_REG28_EXPANDER_1_2);

    /* And the AF output each mode opens its audio path with */
    struct {
        enum opmode mode;
        enum bk4819_afOutput af;
        uint8_t count;
    } afTable[] = {
        { OPMODE_FM, BK4819_AF_FM, 1 },
        { OPMODE_DMR, BK4819_AF_BYPASS, 1 },
        { OPMODE_M17, BK4819_AF_BYPASS, 1 },
        { OPMODE_TETRA, BK4819_AF_BYPASS, 1 },
        { OPMODE_DPMR, BK4819_AF_BYPASS, 1 },
        { OPMODE_AIRBAND, BK4819_AF_AM, 1 },
        { OPMODE_WFM_BC, BK4819_AF_MUTE, 0 }, /* the tuner has the amplifier */
    };

    for (const auto &row : afTable) {
        INFO(radioPlan_modeName(row.mode));
        struct radioPlan plan = planOf(RADIO_PLAN_STAGE_AF, row.mode, BW_12_5,
                                       RADIO_PLAN_RX, &opt);
        INFO(planText(plan));

        REQUIRE(plan.count == row.count);
        REQUIRE((int)radioPlan_afOutput(row.mode) == (int)row.af);

        if (row.count > 0) {
            resetTo(g_init);
            radioPlan_apply(&plan, &g_chip);
            REQUIRE(rg47Af(regOf(0x47)) == BK4819_REG47_AF_OUTPUT(row.af));
        }
    }
}

TEST_CASE("The gains and the deviation, per mode", "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    g_init = initRegFile();

    /* The digital modes: -6 dB RX, gain2 0x3f, microphone at 0 */
    for (enum opmode m :
         { OPMODE_M17, OPMODE_DMR, OPMODE_NXDN, OPMODE_DPMR, OPMODE_P25,
           OPMODE_YSF, OPMODE_DSTAR, OPMODE_TETRA }) {
        INFO(radioPlan_modeName(m));
        applyMode(m, BW_12_5, &opt);
        REQUIRE(rg48Gain1(regOf(0x48)) == BK4819_REG48_AF_RX_GAIN1(1));
        REQUIRE(rg48Gain2(regOf(0x48)) == BK4819_REG48_AF_RX_GAIN2(0x3f));
        REQUIRE((regOf(0x7D) & BK4819_REG7D_MIC_GAIN_MASK) == 0);
    }

    /* DMR's deviation: ETSI's 1.944 kHz outer symbols, 0x582 * 1944 / 2400 */
    applyMode(OPMODE_DMR, BW_12_5, &opt);
    REQUIRE(rg40Dev(regOf(0x40)) == 0x476);
    REQUIRE((regOf(0x40) & BK4819_REG40_TX_DEVIATION_ENABLE) != 0);

    /* M17's: the A36Plus word, for +-2.4 kHz */
    applyMode(OPMODE_M17, BW_12_5, &opt);
    REQUIRE(rg40Dev(regOf(0x40)) == 0x582);

    /* FM keeps the chip's own word at mode change, the calibration sets it */
    applyMode(OPMODE_FM, BW_12_5, &opt);
    REQUIRE(rg40Dev(regOf(0x40)) == BK4819_DEFAULT_TX_DEVIATION);
    REQUIRE((regOf(0x7D) & BK4819_REG7D_MIC_GAIN_MASK)
            == BK4819_DEFAULT_MIC_GAIN);

    /* and radio_updateConfiguration() writes the calibrated one */
    opt.fmTxDevNarrow = 0x4d2;
    opt.fmTxDevWide = 0x9a4;
    struct radioPlan plan = planOf(RADIO_PLAN_STAGE_UPDATE, OPMODE_FM, BW_12_5,
                                   RADIO_PLAN_BOTH, &opt);
    resetTo(g_init);
    radioPlan_apply(&plan, &g_chip);
    INFO(planText(plan));
    REQUIRE(rg40Dev(regOf(0x40)) == 0x4d2);

    plan = planOf(RADIO_PLAN_STAGE_UPDATE, OPMODE_FM, BW_25, RADIO_PLAN_BOTH,
                  &opt);
    resetTo(g_init);
    radioPlan_apply(&plan, &g_chip);
    INFO(planText(plan));
    REQUIRE(rg40Dev(regOf(0x40)) == 0x9a4);

    /* An airband reconfiguration is the AM filter again, and nothing else */
    plan = planOf(RADIO_PLAN_STAGE_UPDATE, OPMODE_AIRBAND, BW_25,
                  RADIO_PLAN_BOTH, &opt);
    INFO(planText(plan));
    REQUIRE(plan.count == 6);
}

TEST_CASE("The airband carrier and its duty window", "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    opt.airTx = true;
    g_init = initRegFile();

    /*
     * AM transmit: the BK4819 emits a bare carrier and the PA bias is driven
     * with the voice envelope, so the numbers that describe the mode are the
     * carrier power and the linear window of that bias (docs/TX_AIRBAND.md).
     */
    struct radioPlan plan = applyMode(OPMODE_AIRBAND, BW_12_5, &opt);
    INFO(planText(plan));

    REQUIRE(plan.knobs.amTx);
    REQUIRE(plan.knobs.amAudio);
    REQUIRE(plan.knobs.rxAfGain1 == 1);
    REQUIRE(RADIO_PLAN_AM_CARRIER_MW == 1000);
    REQUIRE(RADIO_PLAN_AM_DUTY_MIN_PCT == 10);
    REQUIRE(RADIO_PLAN_AM_DUTY_MAX_PCT == 190);

    /* No modulator compiled in: the mode is receive only and says so */
    opt.airTx = false;
    plan = applyMode(OPMODE_AIRBAND, BW_12_5, &opt);
    REQUIRE(!plan.knobs.amTx);
    REQUIRE(plan.knobs.amAudio);

    /* The other modes have neither */
    for (enum opmode m :
         { OPMODE_FM, OPMODE_DMR, OPMODE_M17, OPMODE_TETRA, OPMODE_NXDN,
           OPMODE_DPMR, OPMODE_P25, OPMODE_YSF, OPMODE_DSTAR, OPMODE_WFM_BC }) {
        INFO(radioPlan_modeName(m));
        plan = applyMode(m, BW_12_5, &opt);
        REQUIRE(!plan.knobs.amTx);
        REQUIRE(!plan.knobs.amAudio);
    }
}

TEST_CASE("Every mode's REG_43 mirror is what the chip holds", "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    g_init = initRegFile();

    /*
     * c62_rx_reg43 is the word the baseband dump reports and the one the REG_43
     * sweep writes, so it should be what the chip is holding.  In the 25 kHz
     * modes it is; in the 12.5 kHz digital ones it is not, and the difference is
     * REG_43<8:6>, the AF TX LPF2 field: the mode asks the chip for 3 kHz after
     * setting the IF filters, so the chip ends holding 0x7e08 where the mirror
     * says 0x7e48.  That is frozen here, not fixed: this change was not allowed
     * to move a single write, and a capture's header is not a register.
     *
     * dPMR is a second, unrelated slip under the same mirror: it reports M17's
     * narrow word 0x7808 while the chip holds the same 0x7e08 as NXDN.
     */
    struct {
        enum opmode mode;
        enum bandwidth bw;
        uint16_t mirror; /* c62_rx_reg43, what the dump reports */
        uint16_t reg43;  /* what the chip holds */
        bool wideRx;
    } table[] = {
        { OPMODE_M17, BW_12_5, 0x7808, 0x7808, false },
        { OPMODE_M17, BW_12_5, 0x7e28, 0x7e28, true },
        { OPMODE_DMR, BW_12_5, 0x7e28, 0x7e28, true },
        { OPMODE_NXDN, BW_12_5, 0x7e48, 0x7e08, true },
        { OPMODE_P25, BW_12_5, 0x7e48, 0x7e08, true },
        { OPMODE_YSF, BW_12_5, 0x7e48, 0x7e08, true },
        { OPMODE_DSTAR, BW_12_5, 0x7e48, 0x7e08, true },
        { OPMODE_TETRA, BW_25, 0x7e28, 0x7e28, true },
        { OPMODE_DPMR, BW_12_5, 0x7808, 0x7e08, true },
    };

    for (const auto &row : table) {
        INFO(radioPlan_modeName(row.mode));

        struct radioPlanOptions o = opt;

        o.wideRx = row.wideRx;

        struct radioPlan plan = applyMode(row.mode, row.bw, &o);
        INFO(planText(plan));
        REQUIRE(plan.knobs.rxReg43 == row.mirror);
        REQUIRE(regOf(0x43) == row.reg43);
    }

    /* DMR with the 12.5 kHz setting: same 0x40 of difference */
    struct radioPlanOptions o = opt;

    o.dmr12k5 = true;
    struct radioPlan plan = applyMode(OPMODE_DMR, BW_12_5, &o);
    INFO(planText(plan));
    REQUIRE(plan.knobs.rxReg43 == 0x7e48);
    REQUIRE(regOf(0x43) == 0x7e08);
}

TEST_CASE("The spectrum profile is the widest receive path", "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    g_init = initRegFile();

    /*
     * The sweep needs one bin per channel and no AFC: a carrier near the bin
     * would pull the synthesizer onto itself and move what is being measured.
     */
    struct radioPlan plan = planOf(RADIO_PLAN_STAGE_SPECTRUM, OPMODE_FM, BW_25,
                                   RADIO_PLAN_RX, &opt);
    INFO(planText(plan));

    resetTo(g_init);
    radioPlan_apply(&plan, &g_chip);

    REQUIRE(regOf(0x43) == 0x7e28);       /* 25 kHz mode, 4.5 kHz IF -> 9 kHz */
    REQUIRE((regOf(0x73) & 0x0010) != 0); /* AFC off */
    REQUIRE((regOf(0x47) & 0x0f00) == 0); /* muted */
    REQUIRE(plan.knobs.rxReg43 == 0x7e28);
    REQUIRE((regOf(0x7E) & 0x8000) == 0); /* AGC left automatic */

    opt.spectrumAgcFix = true;
    plan = planOf(RADIO_PLAN_STAGE_SPECTRUM, OPMODE_FM, BW_25, RADIO_PLAN_RX,
                  &opt);
    resetTo(g_init);
    radioPlan_apply(&plan, &g_chip);
    INFO(planText(plan));
    REQUIRE((regOf(0x7E) & 0x8000) != 0);
}

TEST_CASE("A plan prints its writes in the order it walks them", "[regplan]")
{
    struct radioPlanOptions opt;

    radioPlan_defaultOptions(&opt);
    opt.dmr12k5 = true;

    struct radioPlan plan = planOf(RADIO_PLAN_STAGE_MODE, OPMODE_DMR, BW_12_5,
                                   RADIO_PLAN_BOTH, &opt);
    const std::string text = planText(plan);

    INFO(text);
    REQUIRE(text.find("DMR BOTH mode: 17 writes") == 0);
    /* The mode profile starts by restoring the FM demodulator profile */
    REQUIRE(text.find("[ 0] 0x47 mask 0x0f00 value 0x0100")
            != std::string::npos);
    /* ...and the last thing it writes is the microphone gain */
    REQUIRE(text.find("[16] 0x7d mask 0x001f value 0x0000")
            != std::string::npos);
    /* The 12.5 kHz mode with the 4.5 kHz filters, the stock firmware's word */
    REQUIRE(text.find("0x43 mask 0x7ff8 value 0x4048") != std::string::npos);
    REQUIRE(text.find("0x43 mask 0x7e00 value 0x7e00") != std::string::npos);
}

/* The plan's AF selector and the driver's enumeration are the same numbers */
TEST_CASE("Plan enumerations match the driver's", "[regplan]")
{
    REQUIRE((int)RADIO_PLAN_AF_MUTE == (int)BK4819_AF_MUTE);
    REQUIRE((int)RADIO_PLAN_AF_FM == (int)BK4819_AF_FM);
    REQUIRE((int)RADIO_PLAN_AF_AM == (int)BK4819_AF_AM);
    REQUIRE((int)RADIO_PLAN_AF_BYPASS == (int)BK4819_AF_BYPASS);
}

/*
 * Every transmitting mode writes the deviation word.  It used to be only M17
 * and DMR: NXDN, dPMR, P25, YSF and D-STAR keyed the PA with whatever the
 * previous mode had left in REG_40 -- M17's word, FM's band-calibrated one or
 * the chip's reset value -- which is a transmission whose deviation depends on
 * what the operator did before, and no test could see it.
 */
TEST_CASE("Every digital transmitter writes its deviation word", "[regplan]")
{
    struct radioPlanOptions opt;
    radioPlan_defaultOptions(&opt);

    const enum opmode modes[] = { OPMODE_M17, OPMODE_DMR, OPMODE_NXDN,
                                  OPMODE_DPMR, OPMODE_P25, OPMODE_YSF,
                                  OPMODE_DSTAR };
    for (enum opmode mode : modes) {
        struct radioPlan plan = planOf(RADIO_PLAN_STAGE_MODE, mode, BW_12_5,
                                       RADIO_PLAN_BOTH, &opt);
        const std::string text = planText(plan);
        INFO(text);

        bool found = false;
        for (uint8_t i = 0; i < plan.count; i++) {
            /*
             * 0x40 with the deviation enable set (bit 12) and a non-zero word:
             * the enabled form, not the one that leaves the chip's own value.
             */
            if ((plan.write[i].reg == 0x40)
                && ((plan.write[i].value & 0x1000u) != 0)
                && ((plan.write[i].value & 0x0fffu) != 0)) {
                found = true;
            }
        }
        INFO("mode " << radioPlan_modeName(mode));
        REQUIRE(found);
    }
}

/*
 * AM has no deviation to set: airband's plan must not touch the word, so an AM
 * reception cannot inherit a digital mode's deviation and start limiting.
 */
TEST_CASE("Airband does not write the deviation word", "[regplan]")
{
    struct radioPlanOptions opt;
    radioPlan_defaultOptions(&opt);

    struct radioPlan plan = planOf(RADIO_PLAN_STAGE_MODE, OPMODE_AIRBAND,
                                   BW_12_5, RADIO_PLAN_RX, &opt);
    for (uint8_t i = 0; i < plan.count; i++) {
        INFO(planText(plan));
        REQUIRE(plan.write[i].reg != 0x40);
    }
}
