/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <hwconfig.h>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#endif
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/DmrSpeech.h"
#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_voice.h"
#ifdef CONFIG_DMR_TRUNK
#include "protocols/DMR/trunk/dmr_trunk.h"
#include "ui/C62Extras.h"
void radio_c62_setRxFrequency(uint32_t hz);
void radio_c62_setTxFrequency(uint32_t hz);

/*
 * DMR Tier III (Ajustes > DMR > Troncal T3): the channel's RX frequency is
 * the system's control channel, the engine (dmr_trunk.h) says where to
 * listen. trunkCtrlHz: the control channel it was started on; trunkHz:
 * where the receiver is (radio_enableRx() puts it on the channel's);
 * trunkRetune: tune again even to the same frequency (a configuration
 * change may have put the receiver back on the channel's).
 */
static uint32_t trunkCtrlHz;
static uint32_t trunkHz;
static bool trunkRetune;
/*
 * Registered: what a transmission is. txT3 0 an ordinary one; 1 the
 * engine's bursts (a random access CSBK, an answer, P_MAINT, a short data
 * upload) in consecutive frames of one inbound slot; 2 voice on the call
 * granted. Its frequency, slot, colour code and target; the next burst
 * fetched ahead (t3Dt, t3B), so that the last one is known as such.
 */
static uint8_t txT3;
static uint32_t txT3Hz, txT3Dst;
static uint8_t txT3Slot, txT3Cc;
static bool txT3Group;
static uint8_t t3Dt, t3B[12];
static long long txT3WaitFrom;
/* The engine's bursts end with less carrier than a call: the DSP's queue
 * (some 180 ms) and a frame, so that the system's answer, which may come
 * in the next frames, finds the receiver back sooner */
#define T3_TAIL_SYMS (240 * 48 / 10)
#endif
#include "rtx/OpMode_DMR_RX.hpp"
#ifdef CONFIG_DMR_TX
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "protocols/DMR/dmr_tx.h"
#include "protocols/DMR/dmr_ep.h"
#include "protocols/DMR/dmr_sms.h"
#include "protocols/DMR/dmr_aes.h"
#include "protocols/DMR/dmr_hytera.h"
#include "ui/C62Extras.h"
#include "ui/C62TetraMemory.h"
#include "opendmr/dmr_tx_enc.h"
#endif

#define DMR_IN_RATE 48000
/* 30 ms per half buffer: one burst period */
#define HALF_SAMPLES 1440

/*
 * Received samples, or in transmission the baseband sent. The modes are
 * exclusive (disable() ends the transmission and the sampling before the
 * next mode starts): the System Fusion mode's buffer when that mode is
 * built, as dPMR, NXDN and the data modes, 5.6 KB less in SRAM for the heap
 * (M17 takes some 45 KB of it).
 */
#include "c62_scratch.h"
#define baseband c62_scratch.fsk4.baseband
static_assert(sizeof(baseband) == 2 * HALF_SAMPLES * sizeof(int16_t),
              "c62_scratch.h: the 4FSK baseband");
/* The demodulator's float input, converted a chunk at a time on the stack:
 * a whole half buffer (5.6 KB) was SRAM taken from the heap M17 needs */
#define FLOAT_CHUNK 160

/* A configuration change seen while transmitting, or on a call to update()
 * that returns before the receive path: acted on as soon as reception
 * resumes instead of being dropped. Outside the transmitter's guard: the
 * receive side needs it too. */
static bool cfgDirty;
/* The frequencies cfgDirty was last judged against: a reconfiguration that
 * leaves them alone is not a new channel */
static uint32_t cfgRxHz, cfgTxHz;

#ifdef CONFIG_DMR_TX
/*
 * Transmission as a mobile station: a burst every 60 ms (132 symbols, 27.5
 * ms).
 *
 * Simplex: bursts at our own rhythm, the carrier on throughout (the PA power
 * is a filtered PWM voltage, too slow to ramp in the 1.25 ms guard of DMR).
 *
 * Repeater: the bursts in the repeater's inbound timeslot, which lines up
 * with its outbound one (ETSI "aligned" channels; the MMDVM looks for the
 * sync within about +-2 ms). The timing is kept in capture samples of the
 * audio codec, which clocks both the radio input and the transmitter's DAC:
 *  - the repeater's TS1 bursts, as received (rptTiming());
 *  - where our audio leaves the DAC: a single +3 symbol sent first (PA off)
 *    and found in the DSP's echo reference of that DAC channel; the DSP's
 *    output latency, tens of ms and not known otherwise, drops out.
 * What remains (BK4819 and capture filter delays, the MMDVM's own) is the
 * correction of Settings > Radio > Ajuste TX DMR, searched automatically:
 * after each call the repeater did not answer (Parrot 9990 calling back)
 * the next one is tried; an answer fixes it. The carrier stays on between
 * the bursts for now (no gating of the PA yet): test on a quiet repeater.
 *
 * Level: M17 gives +-2.4 kHz for symbols +-3 on the C62 with its gain of
 * 23000 on a filter of DC gain 0.3165 per symbol; DMR wants +-1.944 kHz.
 * The C62 transmitter inverts, as for M17.
 */
#define DMR_TX_GAIN (23000.0f * 0.3165f * 1944.0f / 2400.0f)
#define DMR_TX_INVERT true
#define DMR_TX_HEADERS 2
#define HALF_SYMS (HALF_SAMPLES / DMR_TX_SPS) /* 144: 30 ms */
#define FRAME_SAMPLES 2880                    /* 60 ms */
#define PROBE_SYM (HALF_SYMS + HALF_SYMS / 2) /* In the first half written */
/*
 * Length of the probe, symbols. One +3 symbol through the 81 tap root
 * raised cosine only reaches a fraction of the amplitude of a run, and
 * every call so far fell back to the DSP queue estimate ("salida estimada
 * por la cola del DSP" and never "eco del DAC: pico"), so the peak was
 * under ECHO_MIN. Three symbols give a clear single peak at the middle one,
 * which is the symbol txDelta is measured from. Odd, so the centre is a
 * whole symbol.
 */
#define PROBE_SYMS 3
#define PROBE_MID (PROBE_SYM + PROBE_SYMS / 2)
#define RRC_DELAY ((DMR_TX_RRC_TAPS - 1) / 2) /* Symbol impulse to centre */
#define ECHO_WINDOW 28800                     /* 600 ms to find the probe */
#define ECHO_MIN 1000                         /* Weakest probe accepted */
#define CAL_STEP 5                            /* Automatic search, 2.5 ms */
/*
 * Bursts per step of the in-call sweep. It used to be 6 (one superframe,
 * 360 ms) and that made the sweep useless: MMDVM only re-correlates the
 * sync within +-1 sample of where it last found it, and needs
 * MAX_SYNC_LOST_FRAMES = 13 frames (780 ms) without it before it gives up
 * and searches the whole window again (g4klx/MMDVM, DMRSlotRX.cpp). With
 * 360 ms steps the repeater stayed locked on the first position the sweep
 * happened to offer and never looked at the rest, so the 2026-11-14 21:29
 * run walked all 24 offsets twice over and proved nothing. 20 bursts is
 * 1,2 s: past the 780 ms plus time to re-acquire and decode a header.
 */
#define SWEEP_BURSTS 20
/*
 * Burst of the step at which the voice header is sent again. The header at
 * the start of a step is wasted: the repeater is still locked on the
 * previous step's position and does not look anywhere else until 13 frames
 * (780 ms) have gone by without a sync. 14 bursts is 840 ms, just past
 * that, so the header arrives while it is searching the whole window.
 */
#define SWEEP_HDR_BURST 14
/*
 * Halves to wait before measuring where our audio leaves the DAC: 0,8 s.
 *
 * It was raised to 50 (1,5 s) on 2026-11-14 on a theory about the
 * driver's time base filter that was never measured, and it cost more
 * than it could ever buy: on a repeater call nothing is scheduled until
 * this many halves have passed, so every aligned call lost another
 * 0,72 s of its start. In the logs of 2026-11-15 aligned calls threw away
 * 33 encoded bursts each instead of 21, and a 2,25 s press went out with
 * ZERO voice bursts -- BrandMeister logged it at 00:00. Back to the value
 * that was there before.
 */
/* 2026-11-16: 16 (0,48 s). After a wake-up ED4ZAH transmits only 1,5-2 s,
 * and the call has to reach it in that time; the play time base is good
 * from TB_PLAY_MIN_FRAMES past its start-up (csk6_codec.c), and Ajustes >
 * DMR > Latencia measures it at this same moment. */
#define SETTLE_HALVES 16
/*
 * What the codec's two time bases get wrong together, measured on the C62
 * itself on 2026-11-16 (Ajustes > DMR > Latencia: a burst on the speaker
 * found in the microphone, same DAC, ADC, streams and frames as the radio
 * channels): capture time of the burst minus the play time predicted for it
 * = -27.6 ms, 5 of 5 shots, spread under 0.1 ms. The prediction adds the
 * SDK's mLatency, 40 ms that are made up (AudioSystem.c "TBD: 20ms" plus
 * the 20 ms FIFO the play time base already counts); the real output plus
 * capture latency is about 12.4 ms. Uncorrected, every burst went out
 * 27.6 ms early.
 */
#define AUDIO_TB_ERR_US (-27600)
#define ANSWER_MS 8000                        /* Wait for the Parrot */
#define WAKE_BURSTS 4 /* BS_Dwn_Act CSBKs, 60 ms apart */
/* After the last burst, carrier on while the DSP's queue (some 180 ms of
 * audio) still plays: the transmitter used to go off with the last bursts,
 * wake-ups included, still queued */
#define TAIL_SYMS (450 * 48 / 10)             /* 450 ms */

/* AMBE+2 silence, 72 bits in DMR order (MMDVMHost): when the encoder has no
 * frames ready */
static const uint8_t ambeSilence[9] = {
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

/* C62 audio driver (csk6_codec.h) and radio driver */
extern "C" {
bool csk6codec_rtxInputBoundary(int half, uint64_t *index);
bool csk6codec_echoArm(uint32_t samples);
bool csk6codec_echoResult(uint64_t *index, int32_t *peak);
bool csk6codec_captureTime(uint64_t index, int64_t *us);
bool csk6codec_rtxOutputTime(uint32_t sample, int64_t *us);
uint64_t csk6codec_captureCount(void);
const char *csk6codec_echoDiag(void);
unsigned int csk6codec_channelRms(uint32_t rms[6], uint32_t *rate);
}
void radio_c62_paGate(bool on);
void radio_c62_gateStart();
bool radio_c62_gateWindow(int64_t onUs, int64_t offUs);
void radio_c62_gateStop(bool driveOn);
extern volatile uint16_t c62_tx_reg36; /* PA drive, for the TX log */

/*
 * Through a repeater the TX drive is on only around each burst (see
 * radio_C62.cpp), from where the DAC plays it: before it by GATE_PRE_US,
 * after it by GATE_POST_US, more after because the BK4819's TX filters
 * delay the RF. The margins also cover the error of the DAC time, which
 * is estimated from the DSP's output queue: 37 ms of carrier every 60 ms
 * instead of all of it, and the other timeslot left nearly free.
 */
/*
 * Generous margins: the window is commanded in real time from the estimated
 * output time, whose constant error is unknown (it is what the Parrot
 * search compensates for in the scheduling). Tight margins meant that error
 * could cut into the burst itself, leaving nothing on the air. 10 ms each
 * side still keeps the PA off for about 13 ms of every 60, so the
 * transmission stays bursty rather than a steady carrier.
 */
#define GATE_PRE_US 10000
#define GATE_POST_US 10000
static bool txGating;
static uint32_t txGateWins;

static dmr_tx_t txCall;
/*
 * Encrypted transmission, from the memory (c62_tmem_txPrivOf()): Basic
 * Privacy with the memory's key, or Enhanced Privacy with a keyring entry
 * (dmr_ep.h). Laboratory tests of one's own radios; no band is barred. A
 * memory set to encrypt whose key is missing does not transmit at all,
 * rather than in clear.
 */
static uint8_t txPriv;      /* 0 clear, 1 BP, 2 EP */
static uint16_t txBpKey;
static bool txBpAny;     /* Anytone's 16-bit BP, else Motorola's */
static bool txBpTyt;     /* TYT's: on the burst's coded frames */
static uint8_t txEpId;
static uint8_t txEpKey[DMR_EP_KEY_BYTES];
static uint8_t txAesKey[DMR_AES_KEY_BYTES]; /* txPriv 3: AES or DES */
static uint8_t txAesLen;                    /* its length, the algorithm */
static uint32_t txMi;       /* MI of the superframe being sent */
static uint64_t txMiHy;     /* Hytera Enhanced's, 40 bits (txPriv 5) */
static uint32_t txMiSeed;
static int txPis;           /* PI headers still to send */
static bool txPrivWarned;
/*
 * Repeater call sent free running, at its own 60 ms cadence, because the
 * repeater is not transmitting and so there is no timing reference. That is
 * how a radio opens an idle repeater: it keys up and the repeater locks
 * onto the incoming burst. Aligning to its slot only matters once it is on
 * the air.
 */
static bool txFree;
static uint32_t txSrc;      /* own ID sent: c62_dmrPrivId when encrypting */

/* The own ID a transmission sends: its own one when encrypted, if set */
static uint32_t txSrcId(const rtxStatus_t *rtx)
{
    return (rtx->dmrTxPriv != 0u && c62_dmrPrivId != 0u) ? c62_dmrPrivId
                                                        : c62_dmr.id;
}

/* The key the memory asks for is there */
static bool txPrivReady(const rtxStatus_t *rtx)
{
    const uint8_t m = rtx->dmrTxPriv;
    if (m == C62_TMEM_TX_BP)
        return rtx->dmrBpKey != 0u;
    if (m >= C62_TMEM_TX_EP1 && m <= C62_TMEM_TX_EP_MAX) {
        uint8_t id, key[DMR_EP_KEY_BYTES];
        const bool ok = dmrEp_entry(m - C62_TMEM_TX_EP1, &id, key);
        memset(key, 0, sizeof(key));
        return ok;
    }
    if (m == C62_TMEM_TX_HBP)
        return dmrHyBp_present();
    if (m >= C62_TMEM_TX_HEP1 && m <= C62_TMEM_TX_PRIV_MAX) {
        uint8_t id, key[DMR_EP_KEY_BYTES];
        const bool ok = dmrEp_entry(m - C62_TMEM_TX_HEP1, &id, key);
        memset(key, 0, sizeof(key));
        return ok;
    }
    if (m >= C62_TMEM_TX_AES1 && m <= C62_TMEM_TX_AES_MAX) {
        /* The key, with a DMR key ID (1..255) */
        uint16_t id;
        uint8_t key[DMR_AES_KEY_BYTES];
        uint8_t len;
        const bool ok = dmrAes_entry(m - C62_TMEM_TX_AES1, &id, key, &len)
                        && id >= 1u && id <= 255u;
        dmrAes_wipe(key, sizeof(key));
        return ok;
    }
    return true;
}

/* A new message indicator for each call: never the same twice running */
static uint32_t txNewMi(void)
{
    uint32_t x = txMiSeed ^ k_cycle_get_32() ^ ((uint32_t)getTick()
                                                * 2654435761u);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    txMiSeed = x;
    return x != 0u ? x : 1u;
}

/* The next three frames, enciphered: their 49 bits from the encoder, the
 * keystream (burst vidx of the superframe), then the frame FEC */
static bool txPrivFrames(uint8_t ambe[3][9], int vidx)
{
    uint8_t bits[3][49];
    if (!dmrTxEnc_pop49(bits))
        return false;
    uint8_t ks[3 * DMR_EP_FRAME_BYTES];
    if (txPriv == 2)
        dmrEp_keystream(txEpKey, txMi, (unsigned)vidx * sizeof(ks), ks,
                        sizeof(ks));
    else if (txPriv == 3)
        dmrAes_keystream(txAesKey, txAesLen, txMi,
                         (unsigned)vidx * sizeof(ks), ks,
                         sizeof(ks));
    bool ok = true;
    for (int k = 0; k < 3 && ok; k++) {
        uint8_t in[7] = { 0 };
        for (int i = 0; i < 49; i++)
            if (bits[k][i])
                in[i / 8] |= (uint8_t)(0x80u >> (i % 8));
        if (txPriv == 4) {
            /* Hytera Basic: on the frame's bits, before packing them */
            char cb[49];
            for (int i = 0; i < 49; i++)
                cb[i] = (char)bits[k][i];
            dmrHyBp_frame(cb, (unsigned)vidx * 3u + (unsigned)k);
            memset(in, 0, sizeof(in));
            for (int i = 0; i < 49; i++)
                if (cb[i] & 1)
                    in[i / 8] |= (uint8_t)(0x80u >> (i % 8));
            memset(cb, 0, sizeof(cb));
        } else if (txPriv == 5) {
            uint8_t hks[7];
            dmrHyEp_bits(txEpKey, txMiHy, (unsigned)vidx * 3u + (unsigned)k,
                         hks);
            dmrEp_packed(in, hks);
            memset(hks, 0, sizeof(hks));
        } else if (txPriv == 1 && txBpAny)
            dmrBpAny_packed(txBpKey, (unsigned)vidx * 3u + (unsigned)k, in);
        else if (txPriv == 1)
            dmrBp_packed((uint8_t)txBpKey, in);
        else
            dmrEp_packed(in, &ks[k * DMR_EP_FRAME_BYTES]);
        ok = dmrTxEnc_fec(in, ambe[k]);
    }
    memset(ks, 0, sizeof(ks));
    memset(bits, 0, sizeof(bits));
    return ok;
}
static dmr_mod_t txMod;
static pathId txPath = -1;
static streamId txStream = -1;
static stream_sample_t *txIdle;
static int64_t txHalf;       /* Stream half being written, from 1 */
static bool txRepeater;
static bool txEnding;        /* PTT released: the terminator is next */
static bool txTermSent;
static int64_t txTermEnd;    /* Symbol after the terminator */
static unsigned txHeaders;
static uint32_t txVoice, txSilent, txStart;
static int txPrevPrio;
static bool txNoIdWarned;
static uint8_t txBurst[DMR_TX_BURST_DIBITS];
static int64_t txBurstSym = -1; /* Stream symbol of txBurst's first */
static int64_t txNextSym = -1;  /* Of the next burst, -1 none scheduled */
static uint32_t txBurstK;       /* Bursts scheduled through the repeater */
static bool txEchoArmed, txEchoKnown;
static int64_t txDelta;         /* Capture sample minus stream sample */
static int32_t txEchoPeak;
static bool txEstimated;        /* From the output queue, not the echo */
static double txTgt0, txPeriod; /* Capture sample of burst 0's centre */
static uint32_t txHz;

/* Repeater timing, from reception: capture sample at the centre of symbol 0
 * of its TS1 bursts (TS2 ones count 30 ms earlier); bursts whose slot does
 * not fit (CACH misread) are dropped */
static int64_t rxCapOffset;
static bool rxMapOk;
static uint32_t rptSeq[2];
static uint64_t rptTs1Cap;
static bool rptTs1Ok;
/* rptTs1Cap as a time (uptime, us), taken while the capture that measured it
 * runs: the capture stops and restarts when the transmission starts, and its
 * sample count does not count the gap, so the sample alone moves by it */
static int64_t rptTs1Us;
static bool rptTs1UsOk;
/* rptTs1Us was taken while the capture time base had a whole window: just
 * after a restart it is still an estimate from a few frames */
static bool rptTs1UsTbOk;
/* When any repeater burst was last seen, accepted or not (ms, 0 never) */
static long long rptAnyMs;
/* Channel access through a repeater, while the PTT is held: 0 nothing yet,
 * 1 listening for its grid, 2 woken up and listening again, 3 given up
 * (free running). See txWanted(). */
static int txSeq;
static long long txSeqAt;
static bool txWakeNext; /* the transmission about to start is a wake-up */
static unsigned txWakeCount; /* wake-ups sent for the PTT press held now */
/* When the receive capture last (re)started, ms: its time base takes two
 * windows to come clean (see rptLive()) */
static long long rxStartMs;
/* Bursts rejected in a row as "the other slot" */
static unsigned rptRejRun;
static int64_t txTs1Cap;   /* rptTs1Cap in the capture after the restart */
static int64_t txTs1Shift; /* The gap, in samples */
static long long rptTs1Ms;
/*
 * For 6 s after each wake-up: where the repeater's TS1 falls against its
 * first burst after it, a few times a second. Does an MMDVM move its grid
 * in the seconds after it activates? A DM-1801 follows it during a call
 * (it hears the repeater between its bursts), the C62 cannot.
 */
static long long rptTraceUntil, rptTraceT0, rptTraceLastMs;
static uint64_t rptTraceFirst;
static bool rptTraceHave;
/* Bursts in step with the reference since it was taken: a sync found in
 * noise also starts a reference, and the repeater counts as heard only once
 * several more bursts agree with it */
static uint32_t rptGood;
#define RPT_GOOD_MIN 6
#define RPT_FRESH_MS 60000 /* the reference is projected, see rptFresh() */
static uint32_t rptAccepted, rptRejected;
static uint64_t rptRunFirst, rptRunLast;
static long long rptRunLastMs;
static bool rptRun;
static long long rptNextLog;

/* The TX summary, printed a little after the end of the transmission: the
 * DSP writes to the same serial line when its audio stops */
static char txSummary[288];
static long long txSummaryAt;

/* Waiting for the repeater's answer to a call */
static bool txAwait;
static long long txAwaitUntil;
static int8_t txAwaitCal;

/* Repeater wake-up: this transmission is one, how many were sent for the
 * PTT press held now, when the last one was, and whether we gave up */
static bool txWake;
static unsigned txWakeTries, txWakeBursts;
static long long txWakeAt;
static bool txWakeGaveUp;
static bool txBusyWarned; /* "Busy" said for the PTT press held now */
static bool txCcWarned;   /* "Other colour code" said for it */
/* Correction swept within a call (automatic mode): step, its value, bursts
 * sent with it; and whether the call being answered was one */
static bool txSweep, txAwaitSweep;
/* The call being judged was aligned to the repeater AND not swept, so
 * the correction it carried is the one that went on air and a verdict
 * may move it. A free running call ignores txCal altogether and a swept
 * one changes it several times a second, so neither may step the
 * stored value: doing so walked it at random, a step per unanswered
 * call, on calls that never used it. */
static bool txAwaitAligned;
/*
 * Someone else's voice on our slot when the PTT went down (0: free). An
 * MMDVM repeater relaying a call on a slot takes no other call on it, so
 * such a call says nothing about the timing: on 2026-11-16 three steps of
 * the automatic search went by while 2143971 was talking on TS1 TG 214.
 */
static uint32_t txBusySrc, txAwaitBusy;

/* Someone else's call on a slot (0 = TS1), its source; 0 if free */
static uint32_t slotBusySrc(int idx)
{
    dmr_slot_info_t si;
    dmr_proto_get_slot(idx, &si);
    return (si.activity == DMR_ACT_VOICE && si.ids_valid
            && si.src != c62_dmr.id) ? si.src : 0;
}
static int txSweepStep, txSweepCal;
static unsigned txSegBursts;
static long long txEndingAt;

static void rptPeriod(uint64_t cap)
{
    const long long now = getTick();
    if (!rptRun || now - rptRunLastMs > 2000 || cap <= rptRunLast) {
        rptRunFirst = rptRunLast = cap;
        rptRun = true;
    } else {
        rptRunLast = cap;
    }
    rptRunLastMs = now;
}

/* The repeater's frame against 2880 capture samples, parts per million */
static bool rptPpm(int32_t *ppm, uint32_t *frames)
{
    if (!rptRun)
        return false;
    const uint64_t span = rptRunLast - rptRunFirst;
    const uint32_t k = (uint32_t)((span + FRAME_SAMPLES / 2) / FRAME_SAMPLES);
    if (k < 50)
        return false;
    *ppm = (int32_t)(((int64_t)span - (int64_t)k * FRAME_SAMPLES) * 1000000
                     / ((int64_t)k * FRAME_SAMPLES));
    *frames = k;
    return true;
}

static void calText(char *buf, size_t len, int cal)
{
    const int a = cal < 0 ? -cal : cal;
    snprintf(buf, len, "%c%d,%d ms", cal < 0 ? '-' : '+', a / 2, (a % 2) * 5);
}

/* A call event line: did the repeater answer ours? */
static void txCheckAnswer(const char *line)
{
    if (!txAwait)
        return;
    /* Only the Parrot: our own ID also comes from other radios set to it.
     * The whole field, not a prefix: SRC is last on the line and carries no
     * delimiter, so "SRC 9990" also matched 9990123 and a station with such
     * an ID speaking inside the answer window froze the correction and
     * turned the search off. */
    {
        const char *p = strstr(line, "SRC 9990");
        if (p == NULL)
            return;
        p += 8; /* past "SRC 9990" */
        if (*p >= '0' && *p <= '9')
            return; /* a longer id that merely starts with 9990 */
    }
    txAwait = false;
    if (txAwaitSweep) {
        /* Which step got through is on the BrandMeister monitor, by time */
        printk("[DMR TX] el repetidor respondio a la llamada con barrido: "
               "mira en el monitor la hora y busca el paso en el log\n");
        return;
    }
    char cal[16];
    calText(cal, sizeof(cal), txAwaitCal);
    if (!txAwaitAligned) {
        /* Free running: the correction was not used, so the answer
         * says nothing about it */
        printk("[DMR TX] el repetidor respondio a una llamada libre: "
               "el ajuste %s no interviene y queda como estaba\n", cal);
        return;
    }
    printk("[DMR TX] el repetidor respondio con el ajuste %s: queda fijo\n",
           cal);
    c62_dmr.txCal = txAwaitCal;
    c62_dmr.txCalAuto = 0;
}

/*
 * The call went unanswered: say so and, with the automatic search on,
 * move the correction one step. Reached both when the wait runs out and
 * when the next transmission starts, because txUpdate() used to drop a
 * judgement still pending: keying again within ANSWER_MS threw the call
 * away, no step was taken and the search never moved. Measured
 * 2026-11-14: seven calls in a row, the correction stuck at +0,0 ms and
 * not one verdict on the log.
 */
/*
 * The automatic search for Ajuste TX DMR: one value per unanswered aligned
 * call, locked as soon as the Parrot answers (txCheckAnswer()).
 *
 * Why a correction is needed at all: the capture's time stamps are taken
 * when an audio frame reaches us, not when it was sampled
 * (csk6_codec.c: timebase_update(0, now_us() - samples_us(cap_count))),
 * and AudioRecord gives no latency to take off -- only AudioTrack has one,
 * and it is used. The BK4819's own receive and transmit filters are not
 * accounted for either. All of it makes the repeater's grid look later
 * than it is, so the bursts aimed at it land late by a fixed amount of a
 * few ms, while an awake MMDVM only looks for the sync within about
 * +-1,9 ms (SCAN_START..SCAN_END). On 2026-11-15 about half of the aligned
 * calls into an active ED4ZAH were answered at 0 ms: right at the edge.
 *
 * The amount belongs to this radio's audio path and only a measurement
 * gives it. So: 1 ms steps, earlier first, around zero. The old search
 * went up in 2,5 ms steps through the whole frame, and needed some twenty
 * calls to reach -2,5 ms.
 */
static const int8_t calSeq[] = { 0, -2, -4, -6, 2, -8, 4, -10, 6, -12, 8 };

static int calNext(int cal)
{
    const size_t n = sizeof(calSeq) / sizeof(calSeq[0]);
    for (size_t i = 0; i < n; i++) {
        if (calSeq[i] == cal)
            return calSeq[(i + 1) % n];
    }
    return calSeq[0];
}

static void txAwaitGiveUp(void)
{
        txAwait = false;
        char cal[16], next[16];
        calText(cal, sizeof(cal), txAwaitCal);
        if (txAwaitBusy != 0) {
            printk("[DMR TX] sin respuesta, pero el TS%u estaba ocupado por "
                   "%lu: el ajuste %s se queda como estaba\n",
                   (unsigned)c62_dmr.slot, (unsigned long)txAwaitBusy, cal);
        } else if (txAwaitSweep) {
            printk("[DMR TX] sin respuesta del repetidor en ningun paso del "
                   "barrido (revisa la potencia: 5 W)\n");
        } else if (!txAwaitAligned) {
            /* Free running, or the call never reached the aligned
             * path: moving the correction from here walks a value this
             * call never used */
            printk("[DMR TX] sin respuesta, pero la llamada no fue "
                   "alineada: el ajuste %s se queda como estaba\n",
                   cal);
        } else if (c62_dmr.txCalAuto) {
            const int v = calNext(txAwaitCal);
            c62_dmr.txCal = (int8_t)v;
            calText(next, sizeof(next), v);
            printk("[DMR TX] sin respuesta del repetidor con el ajuste %s; "
                   "siguiente %s\n", cal, next);
        } else {
            printk("[DMR TX] sin respuesta del repetidor con el ajuste %s "
                   "(fijo: pulsa 0 en Ajustes > DMR > Ajuste para que lo "
                   "busque solo, llamada a llamada, contra el Parrot)\n",
                   cal);
        }
}

/*
 * Carry the repeater's TS1 reference into the capture that is running now.
 * The capture stops and restarts when a transmission starts (the microphone
 * opens and the routes are rebuilt) and its sample counter does not count
 * the gap, so the stored sample has to be moved by the time the gap took.
 * Both ways of learning the output delay need this, not just one.
 */
/*
 * The capture stops and restarts when the transmission starts, and its
 * sample count does not count the gap: the TS1 reference, a capture sample,
 * has to move back by it. The gap is measured from the reference's uptime.
 *
 * It is the sum of every gap since the reference was taken (each call of
 * ours restarts the capture into transmit and back), and zero when nothing
 * restarted in between -- a call started straight after a wake-up, for
 * one. A version that second-guessed the measurement (believing only
 * 5..30 ms, then replacing near-zero values with a typical gap) put calls
 * tens of ms off their slot on 2026-11-15, so it is used as measured.
 */
static void txFreezeTs1(void)
{
    txTs1Cap = (int64_t)rptTs1Cap;
    txTs1Shift = 0;
    int64_t tNew;
    bool measured = false;
    if (rptTs1UsOk && csk6codec_captureTime(rptTs1Cap, &tNew)) {
        txTs1Shift = (rptTs1Us - tNew) * 48 / 1000;
        measured = true;
    }
    const char *why = measured ? "" : " (sin medida)";
    txTs1Cap += txTs1Shift;
    const int64_t su = txTs1Shift * 125 / 6;
    printk("[DMR TX] captura reiniciada: referencia del TS1 movida "
           "%s%ld.%03ld ms%s%s\n", su < 0 ? "-" : "+",
           (long)((su < 0 ? -su : su) / 1000),
           (long)((su < 0 ? -su : su) % 1000),
           rptTs1UsOk ? "" : " (sin hora de referencia)", why);
}

/* Everything learnt about one repeater's timing, dropped when the channel
 * or the mode changes: rptFresh() would otherwise keep saying yes for a
 * minute and the first call to a different repeater would be scheduled
 * against the old one's grid, anywhere in its 60 ms frame. */
static void rptForget(void)
{
    rptTs1Ok = false;
    rptTs1UsOk = false;
    rptGood = 0;
    rptSeq[0] = rptSeq[1] = 0;
    rxMapOk = false;
    rptRun = false;
    rptTs1Ms = 0;
    rptAccepted = rptRejected = 0;
}

static void rptTiming(void)
{
    const long long now = getTick();
    if (txSummary[0] && now >= txSummaryAt) {
        printk("%s\n", txSummary);
        txSummary[0] = 0;
    }
    if (txAwait && now >= txAwaitUntil)
        txAwaitGiveUp();
    if (!rxMapOk)
        return;
    for (int slot = 0; slot < 2; slot++) {
        uint32_t sample, seq;
        if (!dmr_demod_slot_start(slot, &sample, &seq) || seq == rptSeq[slot])
            continue;
        rptSeq[slot] = seq;
        rptAnyMs = now;
        const int64_t cap = (int64_t)sample + rxCapOffset;
        const uint64_t ts1 = (uint64_t)(cap - (slot ? FRAME_SAMPLES / 2 : 0));
        if (!rptTs1Ok || now - rptTs1Ms > 2000) {
            rptTs1Cap = ts1;
            rptTs1Ok = true;
            rptGood = 0; /* A new reference: may be noise until confirmed */
            rptRejRun = 0;
            rptAccepted++;
        } else {
            int64_t d = ((int64_t)ts1 - (int64_t)rptTs1Cap) % FRAME_SAMPLES;
            if (d > FRAME_SAMPLES / 2)
                d -= FRAME_SAMPLES;
            if (d < -FRAME_SAMPLES / 2)
                d += FRAME_SAMPLES;
            if (d < -240 || d > 240) { /* 5 ms off: the other slot */
                rptRejected++;
                /*
                 * Four in a row: it is the reference that is wrong (taken
                 * from a misread slot, or just before the capture
                 * restarted), not the bursts. Dropped, the next burst takes
                 * it again; kept, every good burst went on being thrown
                 * away for two seconds (2026-11-15 23:43, after a wake-up:
                 * "64 de otro slot descartadas", and the wait ran out).
                 */
                if (++rptRejRun >= 4) {
                    rptTs1Ok = false;
                    rptRejRun = 0;
                }
                continue;
            }
            rptRejRun = 0;
            rptTs1Cap = ts1;
            rptAccepted++;
            rptGood++;
            if (slot == 0)
                rptPeriod(ts1);
            if (now < rptTraceUntil) {
                if (!rptTraceHave) {
                    rptTraceHave = true;
                    rptTraceFirst = ts1;
                    rptTraceT0 = now;
                    rptTraceLastMs = now;
                    printk("[DMR RPT] tras despertar: primera rafaga\n");
                } else if (now - rptTraceLastMs >= 250) {
                    rptTraceLastMs = now;
                    int64_t d = ((int64_t)ts1 - (int64_t)rptTraceFirst)
                              % FRAME_SAMPLES;
                    if (d > FRAME_SAMPLES / 2)
                        d -= FRAME_SAMPLES;
                    if (d < -FRAME_SAMPLES / 2)
                        d += FRAME_SAMPLES;
                    const int64_t us = d * 125 / 6;
                    printk("[DMR RPT] tras despertar: +%lld ms, TS1 %s%lld.%03lld"
                           " ms respecto a la primera\n",
                           (long long)(now - rptTraceT0), us < 0 ? "-" : "+",
                           (long long)((us < 0 ? -us : us) / 1000),
                           (long long)((us < 0 ? -us : us) % 1000));
                }
            }
        }
        rptTs1Ms = now;
        /* Its time even before the capture's time base has a whole window
         * (just after a transmission): close enough, and without it the
         * restart's gap, some 10 ms, would go unaccounted */
        int64_t us;
        rptTs1UsTbOk = csk6codec_captureTime(rptTs1Cap, &us);
        rptTs1UsOk = true;
        rptTs1Us = us;
    }
    if (now < rptNextLog)
        return;
    rptNextLog = now + 5000;
    if (rptAccepted == 0 && rptRejected == 0)
        return;
    int32_t ppm = 0;
    uint32_t frames = 0;
    const bool havePpm = rptPpm(&ppm, &frames);
    const uint32_t us = (uint32_t)((rptTs1Cap % FRAME_SAMPLES) * 125u / 6u);
    char l[200];
    snprintf(l, sizeof(l), "[DMR RPT] TS1 en %lu.%03lu ms del ciclo de 60 ms "
             "(%lu rafagas, %lu de otro slot descartadas), periodo %+ld ppm "
             "(%lu tramas), fase fijada %lu veces", (unsigned long)(us / 1000),
             (unsigned long)(us % 1000), (unsigned long)rptAccepted,
             (unsigned long)rptRejected, (long)(havePpm ? ppm : 0),
             (unsigned long)frames,
             (unsigned long)dmr_proto_phase_sets());
    printk("%s\n", l);
    rptAccepted = rptRejected = 0;
}

/* Stream symbol of burst k through the repeater */
static int64_t txSymFor(uint32_t k)
{
    const double q = txTgt0 + (double)k * txPeriod - (double)txDelta
                   - RRC_DELAY;
    return (int64_t)llround(q / DMR_TX_SPS);
}
#endif

/**
 * Call events (start, end, IDs, alias) on the console.
 */
static void printLine(const char *line)
{
#ifdef CONFIG_DMR_TX
    txCheckAnswer(line);
#endif
#ifdef __ZEPHYR__
    printk("DMR %s\n", line); /* Unbuffered, safe from any thread */
#else
    printf("DMR %s\n", line);
#endif
}

/**
 * Receiver figures on the console every 5 s, to tell a missing signal from
 * a failed lock or failed decoding: input mean and RMS (ADC units), lock and
 * polarity, syncs, outer symbol level and offset, blocks lost, bursts
 * decoded and what they carried.
 */
static void printStatus(const dmr_demod_status_t &ds, float mean, float msq)
{
    static long long next;
    static float accMean, accMsq;
    static unsigned blocks;
    accMean += mean;
    accMsq += msq;
    blocks++;

    long long now = getTick();
    if (now < next)
        return;
    next = now + 5000;
#ifdef CONFIG_DMR_TX
    /* The capture layout the DSP took (echo reference of the DAC or not),
     * now and then: at power on it gets mixed with the DSP's own output */
    static unsigned diagCount;
    if (diagCount++ % 12 == 0)
        printk("[DMR] captura: %s\n", csk6codec_echoDiag());
    {
        /* Which capture channel carries the radio, the echo reference... */
        uint32_t rms[6];
        uint32_t rate;
        const unsigned nch = csk6codec_channelRms(rms, &rate);
        char l[120];
        int len = snprintf(l, sizeof(l), "[DMR] captura %lu muestras/s, rms "
                           "por canal:", (unsigned long)rate);
        for (unsigned c = 0; c < nch && len > 0 && (size_t)len < sizeof(l);
             c++)
            len += snprintf(l + len, sizeof(l) - (size_t)len, " %u:%lu", c,
                            (unsigned long)rms[c]);
        printk("%s\n", l);
    }
#endif

    float m = accMean / blocks;
    float var = accMsq / blocks - m * m;
    int rms = 0;
    while ((float)(rms + 1) * (float)(rms + 1) <= var && rms < 40000)
        rms++;
    accMean = accMsq = 0.0f;
    blocks = 0;

    dmr_proto_stats_t st;
    dmr_proto_get_stats(&st);
    char line[320];
    int len = snprintf(line, sizeof(line),
             "estado: entrada media %d rms %d | %s%s%s%s syncs %lu nivel %d "
             "dc %d perdidos %lu | rafagas %lu validas %lu LC %lu fin %lu "
             "EMB %lu/%lu CSBK %lu idle %lu",
             (int)m, rms, ds.locked ? "ENGANCHADO" : "buscando",
             ds.bs ? " BS" : "", ds.ms ? " MS" : "",
             ds.inverted ? " inv" : "", (unsigned long)ds.syncs,
             (int)ds.level, (int)ds.dc, (unsigned long)ds.overruns,
             (unsigned long)st.frames, (unsigned long)st.frames_valid,
             (unsigned long)st.lc_headers, (unsigned long)st.terminators,
             (unsigned long)st.emb_lc_ok,
             (unsigned long)(st.emb_lc_ok + st.emb_lc_bad),
             (unsigned long)st.csbk, (unsigned long)st.idle);
    /* Voice bursts B..F and their EMB: where the embedded LC (alias) stops */
    if (len > 0 && (size_t)len < sizeof(line))
        len += snprintf(line + len, sizeof(line) - (size_t)len,
                 " | vozBF %lu EMBmal %lu LCSS %lu/%lu/%lu/%lu",
                 (unsigned long)st.voice_bf, (unsigned long)st.emb_bad,
                 (unsigned long)st.lcss[0], (unsigned long)st.lcss[1],
                 (unsigned long)st.lcss[2], (unsigned long)st.lcss[3]);
    /* What the embedded LCs carried: voice LC, talker alias, other */
    if (len > 0 && (size_t)len < sizeof(line))
        snprintf(line + len, sizeof(line) - (size_t)len,
                 " | ELC voz %lu alias %lu/%lu otro %lu",
                 (unsigned long)st.elc_voice, (unsigned long)st.elc_ta_hdr,
                 (unsigned long)st.elc_ta_blk, (unsigned long)st.elc_other);
    printLine(line);
}

OpMode_DMR_RX::OpMode_DMR_RX()
    : startRx(false), startTx(false), dataErrors(0), basebandPath(-1),
      basebandId(-1)
{
}

#ifdef CONFIG_DMR_TX
/* The repeater was heard recently enough to transmit in its slots */
__attribute__((unused)) static bool rptFresh(void)
{
    /*
      * The reference keeps its worth long after the repeater goes quiet:
      * the bursts are scheduled from it with the measured drift (rptPpm()),
      * and at the usual +24 ppm a whole minute only piles up about 1.4 ms,
      * nothing beside a 30 ms slot. Three seconds meant that calling a
      * repeater that had just stopped always fell back to free running.
      */
    return rptTs1Ok && rptGood >= RPT_GOOD_MIN
        && getTick() - rptTs1Ms <= RPT_FRESH_MS;
}

/*
 * Is the repeater's grid live: heard right now, from enough bursts in
 * agreement, timed against a settled capture time base?
 *
 * ETSI TS 102 361-1 cl. 4.4.1: "a MS shall synchronize to an outbound
 * channel and base its inbound timing entirely on the outbound timing. If
 * the BS is not currently transmitting, a MS wishing to access the system
 * shall send a 'BS activation' signalling to the BS asynchronously and wait
 * for the outbound channel to be established before synchronizing and
 * continuing with further transmission." A DM-1801 does exactly that and,
 * from the same spot, never fails into ED4ZAH -- also when it is idle.
 *
 * This radio used to transmit at once on whatever reference it had: one
 * projected from minutes ago, or carried across several capture restarts.
 * Every aligned call that worked on 2026-11-15 was made with the repeater
 * transmitting steadily; the ones that failed had a reference just taken
 * after it woke, about to go stale, or disturbed by a TS2 call starting or
 * ending at the PTT. So now it only aims at a grid it is hearing.
 */
#define RPT_LIVE_GOOD 8    /* bursts in agreement with the reference */
#define RPT_LIVE_MS 600    /* and the last of them this recent */
#define LISTEN_MS 300      /* PTT: listen this long if the repeater is quiet */
#define LISTEN_ACTIVE_MS 2500 /* this long if it is transmitting */
#define WAKE_LISTEN_MS 5000   /* after a wake-up has gone out */
/*
 * The capture's time base is the minimum over a window of 64 frames and of
 * the window before it: the first window after a restart is the stretch
 * where the DSP delivers late, so it takes two to come clean. Every aligned
 * call that reached the repeater on 2026-11-15 was aimed with the capture
 * running for more than 10 s; the one aimed right after a wake-up, with the
 * capture under a second old, did not.
 */
/* The capture no longer restarts around a transmission (releaseRxPath()):
 * only the receive stream does, and a few bursts settle it. 1,5 s here
 * plus the output's settling put our first burst 3-4 s after a wake-up,
 * by when ED4ZAH had gone quiet again (it transmits 1,5-2 s). */
#define RX_SETTLE_MS 300
#define N_WAKEUP 2            /* ETSI TS 102 361-1 annex: suggested value */
/*
 * A list of data bursts (dmr_sms.h: our text message, or an answer one to
 * us asks for), sent as a call would be, PTT or not: the same listening
 * and wake-up of a repeater, its slot timing, then the bursts one a frame
 * and a short tail. The next burst is fetched ahead, so that the last one
 * is known as such.
 */
static bool txData;
static uint8_t dataDt, dataB[12];
#define DATA_TAIL_SYMS (240 * 48 / 10)

/* The slot of this transmission: the trunked call's, else Ajustes > DMR */
static uint8_t txSlotUse(void)
{
#ifdef CONFIG_DMR_TRUNK
    if (txT3 != 0)
        return txT3Slot;
#endif
    return c62_dmr.slot;
}

static bool rptLive(void)
{
    const long long now = getTick();
    return rptTs1Ok && rptTs1UsOk && rptTs1UsTbOk
        && rptGood >= RPT_LIVE_GOOD && now - rptTs1Ms <= RPT_LIVE_MS
        && now - rxStartMs >= RX_SETTLE_MS;
}

#ifdef CONFIG_DMR_TRUNK
/*
 * Registered: transmit when the engine has bursts waiting, on the system's
 * grid only (a random access out of its slot is noise to the TSCC), or
 * with the PTT down on a call the radio may talk on (aligned when the grid
 * is heard, else free running, as any repeater call).
 */
static bool t3TxWanted(const rtxStatus_t *rtx)
{
    uint32_t hz, dst;
    uint8_t slot, cc;
    bool group;
    if (rtx->txDisable)
        return false;
    if (dmrTrunk_sendPending(&hz, &slot, &cc)) {
        if (txT3WaitFrom == 0)
            txT3WaitFrom = getTick();
        if (rptLive()) {
            txT3 = 1;
            txT3Hz = hz;
            txT3Slot = slot;
            txT3Cc = cc;
            txT3WaitFrom = 0;
            return true;
        }
        if (getTick() - txT3WaitFrom > 1000) {
            printk("[T3] sin rejilla del control en 1 s: rafaga descartada\n");
            dmrTrunk_sendDone(false);
            txT3WaitFrom = 0;
        }
        return false;
    }
    txT3WaitFrom = 0;
    if (platform_getPttStatus()
        && dmrTrunk_talk(&hz, &slot, &cc, &dst, &group)) {
        txT3 = 2;
        txT3Hz = hz;
        txT3Slot = slot;
        txT3Cc = cc;
        txT3Dst = dst;
        txT3Group = group;
        return true;
    }
    txT3 = 0;
    return false;
}
#endif

bool OpMode_DMR_RX::txWanted(const rtxStatus_t *rtx)
{
#ifdef CONFIG_DMR_TRUNK
    /* Tier III listening: never transmits, on the control channel or on a
     * call's channel. Registered: only what the system and the call ask */
    if (c62_t3.mode == C62_T3_LISTEN)
        return false;
    if (c62_t3.mode == C62_T3_REG && dmrTrunk_running())
        return t3TxWanted(rtx);
    txT3 = 0;
#endif
    /* A text message or an answer waiting: wanted as the PTT would be */
    dmrSms_setOwnId(c62_dmr.id);
    dmrSms_tick((uint32_t)getTick());
    const bool dataWant = dmrSms_pending() && !platform_getPttStatus();
    if ((!platform_getPttStatus() && !dataWant) || rtx->txDisable) {
        txWakeTries = 0;
        txWakeGaveUp = false;
        txSeq = 0;
        txWakeNext = false;
        txWakeCount = 0;
        txBusyWarned = false;
        txCcWarned = false;
        txPrivWarned = false;
        c62_dmrBothBusy = false;
        c62_dmrSlotBusy = 0;
        c62_dmrOtherCc = -1;
        return false;
    }
    if (txSrcId(rtx) == 0) {
        if (!txNoIdWarned)
            printk("[DMR TX] sin ID DMR: ponlo en Ajustes > Radio > ID DMR\n");
        txNoIdWarned = true;
        return false;
    }
    txNoIdWarned = false;
    /* Encrypted without its key: not at all, never in clear instead (data
     * goes in clear) */
    if (!dataWant && !txPrivReady(rtx)) {
        if (!txPrivWarned)
            printk("[DMR TX] la memoria emite cifrado (%s) y falta su clave: "
                   "no transmito\n",
                   rtx->dmrTxPriv == C62_TMEM_TX_BP ? "BP" : "EP");
        txPrivWarned = true;
        return false;
    }
    if (rtx->txFrequency == rtx->rxFrequency)
        return true; /* simplex */
    /*
     * The repeater heard in the last 500 ms uses another colour code: it
     * would ignore the call (ETSI TS 102 361-1: a BS only repeats bursts of
     * its own colour code). Said on the screen, nothing sent. Either slot
     * tells it; only bursts whose slot type decoded carry a colour code
     * (gmtii 0e5e1531, checked against our dmr_proto).
     */
    {
        dmr_demod_status_t ds;
        dmr_demod_get_status(&ds);
        int other = -1;
        for (int i = 0; i < 2; i++) {
            dmr_slot_info_t si;
            dmr_proto_get_slot(i, &si);
            if (si.activity != DMR_ACT_NONE && si.cc >= 0
                && (uint32_t)(ds.now_ms - si.last_ms) < 500u
                && si.cc != (int)c62_dmr.cc)
                other = si.cc;
        }
        c62_dmrOtherCc = (int8_t)other;
        if (other >= 0) {
            if (!txCcWarned)
                printk("[DMR TX] el repetidor usa CC %d y el nuestro es %u: "
                       "no transmito\n", other, (unsigned)c62_dmr.cc);
            txCcWarned = true;
            return false;
        }
    }
    /*
     * A fixed TX slot carrying someone else's call: wait, as AUTO does
     * with both slots busy, and start as soon as it frees with the PTT
     * still down (polite to all, ETSI TS 102 361-2 channel access). The
     * repeater would not take the call meanwhile anyway.
     */
    if (!c62_dmrTxAuto) {
        const uint32_t b = slotBusySrc(c62_dmr.slot == 2u ? 1 : 0);
        if (b != 0) {
            if (!txBusyWarned)
                printk("[DMR TX] TS%u ocupado por %lu: espero a que quede "
                       "libre\n", (unsigned)c62_dmr.slot, (unsigned long)b);
            txBusyWarned = true;
            c62_dmrSlotBusy = c62_dmr.slot;
            return false;
        }
        c62_dmrSlotBusy = 0;
    }
    /*
     * TX slot AUTO (P2, Ajustes > DMR > Slot TX): TS1, TS2 if TS1 carries
     * someone else's call, and while both do, nothing: the screen says so
     * and the call starts as soon as one frees, the PTT still down.
     */
    if (c62_dmrTxAuto) {
        const uint32_t b1 = slotBusySrc(0), b2 = slotBusySrc(1);
        if (b1 != 0 && b2 != 0) {
            if (!txBusyWarned)
                printk("[DMR TX] TS1 (%lu) y TS2 (%lu) ocupados: no "
                       "transmito hasta que uno quede libre\n",
                       (unsigned long)b1, (unsigned long)b2);
            txBusyWarned = true;
            c62_dmrBothBusy = true;
            return false;
        }
        c62_dmrBothBusy = false;
        const uint8_t pick = b1 == 0 ? 1u : 2u;
        if (c62_dmr.slot != pick) {
            c62_dmr.slot = pick;
            printk("[DMR TX] slot automatico: TS%u%s\n", (unsigned)pick,
                   pick == 2u ? " (TS1 ocupado)" : "");
        }
    }
    /*
     * Through a repeater. This is called twice on the way into a
     * transmission (from RX, then from OFF), so every answer below has to
     * hold for both calls: a wake-up stays wanted until it has started.
     */
    if (txWakeNext)
        return true;
    if (rptLive())
        return true; /* aligned, on the grid heard right now */
    const long long now = getTick();
    const bool active = rptAnyMs != 0 && now - rptAnyMs <= 2000;
    switch (txSeq) {
    case 0:
        txSeq = 1;
        txSeqAt = now;
        printk("[DMR TX] PTT: escucho al repetidor antes de transmitir\n");
        return false;
    case 1:
        if (now - txSeqAt < (active ? LISTEN_ACTIVE_MS : LISTEN_MS))
            return false;
        if (active) {
            /* Heard, but its grid never settled: nothing to wake */
            txSeq = 3;
            printk("[DMR TX] el repetidor transmite pero su rejilla no se "
                   "asienta: llamada con temporizacion libre\n");
            return true;
        }
        /*
         * Quiet: wake it up with BS_Dwn_Act (ETSI TS 102 361-2 cl. 5.1.1.1:
         * a hibernating BS stays so unless the burst is a CSBK with its
         * colour code). An idle MMDVM listens for exactly that: its idle
         * receiver passes CSBKs only and the host wakes on a BS_Dwn_Act; a
         * voice header is not even looked at. On 2026-11-15 23:33 two
         * wake-ups of voice headers left ED4ZAH asleep ("syncs 0") and a
         * DM-1801 woke it at once.
         */
        txSeq = 2;
        txSeqAt = now; /* moved to the end of the wake-up by txEnd() */
        txWakeNext = true;
        txWakeCount = 1;
        printk("[DMR TX] no oigo al repetidor: lo despierto\n");
        return true;
    case 2:
        if (now - txSeqAt < WAKE_LISTEN_MS)
            return false;
        if (txWakeCount < N_WAKEUP) {
            /* Figure 5.33: another wake-up, up to N_Wakeup */
            txWakeCount++;
            txSeqAt = now;
            txWakeNext = true;
            printk("[DMR TX] el repetidor no contesta: lo despierto otra "
                   "vez\n");
            return true;
        }
        txSeq = 3;
        printk("[DMR TX] el repetidor no contesta al despertar: llamada con "
               "temporizacion libre\n");
        return true;
    default:
        return true; /* given up: free running */
    }
}

/* Stop transmitting and go back to receive */
void OpMode_DMR_RX::txEnd(rtxStatus_t *rtx)
{
    if (txStream >= 0)
        audioStream_stop(txStream); /* Plays what is queued, then stops */
    if (txPath >= 0)
        audioPath_release(txPath);
    txStream = -1;
    txPath = -1;
    /* All bursts played (the tail lasts longer than the DSP's queue): the
     * drive stays off until the transmitter goes off */
    if (txGating) {
        radio_c62_gateStop(false);
        txGating = false;
        printk("[DMR TX] PA cortado entre rafagas: %lu ventanas de %u ms "
               "por cada 60 ms\n", (unsigned long)txGateWins,
               (unsigned)((GATE_PRE_US + GATE_POST_US) / 1000 + 29));
    }
    dmrTxEnc_stop();
    memset(txEpKey, 0, sizeof(txEpKey));
    dmrAes_wipe(txAesKey, sizeof(txAesKey));
#ifdef CONFIG_DMR_TRUNK
    radio_c62_setTxFrequency(0);
    if (txT3 == 1)
        dmrTrunk_sendDone(true);
    const bool wasT3 = txT3 != 0;
    txT3 = 0;
#else
    const bool wasT3 = false;
#endif
    const bool wasData = txData;
    if (txData) {
        dmrSms_txDone(true);
        txData = false;
    }
    uint32_t bursts, maxUs, dropped;
    dmrTxEnc_stats(&bursts, &maxUs, &dropped);
    printk("[DMR TX] fin: %lu ms, %lu rafagas de voz (%lu sin audio a "
           "tiempo), codificador %lu rafagas, max %lu us, descartadas %lu\n",
           (unsigned long)((uint32_t)getTick() - txStart),
           (unsigned long)txVoice, (unsigned long)txSilent,
           (unsigned long)bursts, (unsigned long)maxUs,
           (unsigned long)dropped);

    /* Timing, for the log: where the bursts left the DAC against the
     * repeater's TS1, in the 60 ms cycle */
    int len = 0;
    if (txEchoKnown && txEstimated)
        len = snprintf(txSummary, sizeof(txSummary),
                       "[DMR TX] salida estimada por la cola del DSP, desfase "
                       "%lld muestras", (long long)txDelta);
    else if (txEchoKnown)
        len = snprintf(txSummary, sizeof(txSummary),
                       "[DMR TX] eco del DAC: pico %ld, desfase %lld muestras",
                       (long)txEchoPeak, (long long)txDelta);
    else
        len = snprintf(txSummary, sizeof(txSummary),
                       "[DMR TX] eco del DAC no encontrado (pico %ld)",
                       (long)txEchoPeak);
    if (txEchoKnown && rptTs1Ok && txRepeater && !txFree && txBurstSym >= 0
        && len > 0) {
        const int64_t c = txBurstSym * DMR_TX_SPS + RRC_DELAY + txDelta;
        int64_t d = (c - txTs1Cap) % FRAME_SAMPLES;
        if (d < 0)
            d += FRAME_SAMPLES;
        const int64_t us = d * 125 / 6;
        if ((size_t)len < sizeof(txSummary)) {
            len += snprintf(txSummary + len,
                            sizeof(txSummary) - (size_t)len,
                            "; ultima rafaga %ld.%03ld ms tras el TS1 "
                            "del repetidor", (long)(us / 1000),
                            (long)(us % 1000));
        }
    }
    if (txRepeater && len > 0 && (size_t)len < sizeof(txSummary)) {
        char cal[16];
        calText(cal, sizeof(cal), c62_dmr.txCal);
        snprintf(txSummary + len, sizeof(txSummary) - (size_t)len,
                 "; repetidor, TS%u, ajuste %s (%s), TX %lu Hz",
                 (unsigned)txSlotUse(), cal,
                 c62_dmr.txCalAuto ? "busqueda automatica" : "fijo",
                 (unsigned long)txHz);
    }
    txSummaryAt = getTick() + 600;

    /* Through the repeater: wait for its answer to judge the correction */
    if (txWake)
        snprintf(txSummary, sizeof(txSummary), "[DMR TX] despertar enviado "
                 "a %lu Hz; escuchando el repetidor", (unsigned long)txHz);
    if (txRepeater && !txWake && !wasT3 && !wasData
        && (txVoice > 0 || txHeaders < DMR_TX_HEADERS)) {
        txAwait = true;
        txAwaitUntil = getTick() + ANSWER_MS;
        txAwaitCal = c62_dmr.txCal;
        txAwaitSweep = txSweep;
        txAwaitAligned = txRepeater && !txWake && !txFree && txEchoKnown
                      && !txSweep;
        txAwaitBusy = txBusySrc;
    }
    /*
     * The capture restarts on the way back to receive, with a gap it does
     * not count, so the reference in capture samples is off by it now. Take
     * it again from the next bursts, at once: kept, they would be thrown
     * away as "the other slot" for two seconds, and the wait after a
     * wake-up would run out before the grid came back live. Nothing aims
     * at an old reference any more (rptLive()). After the summary above,
     * which reads it.
     */
    if (txRepeater) {
        rptTs1Ok = false;
        rptTs1UsOk = false;
        rptTs1UsTbOk = false;
        rptGood = 0;
        rptRun = false;
    }
    /* The wait for the repeater after a wake-up starts now, when it has
     * gone out: counted from the request, the wake-up's own second or so
     * on the air (stream start, bursts, tail) left almost nothing of it */
    if (txWake) {
        txSeqAt = getTick();
        rptTraceUntil = getTick() + 6000;
        rptTraceHave = false;
    }
    c62_dmrWaking = false;
    k_thread_priority_set(k_current_get(), txPrevPrio);
    rtx->opStatus = OFF;
    startRx = true;
}

/*
 * One half buffer (30 ms, 144 symbols) of transmission each call. Bursts
 * are placed by symbol, anywhere in the stream: in simplex every 288
 * symbols from the second half written, through a repeater at its slots.
 */
void OpMode_DMR_RX::txUpdate(rtxStatus_t *rtx)
{
    if (startTx) {
        startTx = false;
        txPrevPrio = k_thread_priority_get(k_current_get());
        txRepeater = rtx->txFrequency != rtx->rxFrequency;
#ifdef CONFIG_DMR_TRUNK
        /* A trunked system: always a base station's grid */
        if (txT3 != 0)
            txRepeater = true;
#endif
        txWake = txWakeNext;
        c62_dmrWaking = txWake;
        txWakeNext = false;
        txWakeBursts = 0;
        txWakeAt = getTick(); /* read by the safety cut even if unused */
        txBusySrc = txRepeater ? slotBusySrc(txSlotUse() == 2 ? 1 : 0) : 0;
#ifdef CONFIG_DMR_TRUNK
        if (txT3 != 0)
            txBusySrc = 0; /* The system's own slot: it decides */
#endif
        if (txBusySrc != 0)
            printk("[DMR TX] TS%u ocupado: habla %lu; el repetidor no "
                   "aceptara esta llamada\n", (unsigned)txSlotUse(),
                   (unsigned long)txBusySrc);
        /*
         * Aligned only on a grid heard right now (rptLive()); txWanted()
         * has already listened, and woken the repeater if it was quiet. A
         * wake-up goes free running: the repeater has no grid to offer.
         */
        txFree = txWake || (txRepeater && !rptLive());
        /* A judgement still pending is resolved now, not dropped */
        if (txAwait)
            txAwaitGiveUp();
        txSweep = false;
        txPriv = 0;
        txPis = 0;
        /* Data, not voice: the first burst of the list now (none: nothing) */
        txData = !txWake && !platform_getPttStatus() && dmrSms_pending()
#ifdef CONFIG_DMR_TRUNK
                 && txT3 == 0
#endif
            ;
        if (txData && !dmrSms_txNext(&dataDt, dataB)) {
            dmrSms_txDone(false);
            txData = false;
            rtx->opStatus = OFF;
            startRx = true;
            return;
        }
        if (txT3 != 0 || txData) {
            /* A trunked call, and data, go in clear */
        } else if (rtx->dmrTxPriv == C62_TMEM_TX_BP && rtx->dmrBpKey != 0u) {
            txPriv = 1;
            txBpKey = rtx->dmrBpKey;
            txBpAny = rtx->dmrBpAnytone != 0u;
            txBpTyt = rtx->dmrBpTyt != 0u;
        } else if (rtx->dmrTxPriv >= C62_TMEM_TX_EP1
                   && rtx->dmrTxPriv <= C62_TMEM_TX_EP_MAX
                   && dmrEp_entry(rtx->dmrTxPriv - C62_TMEM_TX_EP1, &txEpId,
                                  txEpKey)) {
            txPriv = 2;
            txMi = txNewMi();
            txPis = 1;
        } else if (rtx->dmrTxPriv == C62_TMEM_TX_HBP
                   && dmrHyBp_present()) {
            txPriv = 4;
        } else if (rtx->dmrTxPriv >= C62_TMEM_TX_HEP1
                   && rtx->dmrTxPriv <= C62_TMEM_TX_PRIV_MAX
                   && dmrEp_entry(rtx->dmrTxPriv - C62_TMEM_TX_HEP1, &txEpId,
                                  txEpKey)) {
            txPriv = 5;
            /* A 40-bit MI, never 0 */
            txMiHy = (((uint64_t)txNewMi() << 8) ^ txNewMi())
                     & 0xFFFFFFFFFFull;
            if (txMiHy == 0u)
                txMiHy = 1u;
            txPis = 1;
        } else if (rtx->dmrTxPriv >= C62_TMEM_TX_AES1
                   && rtx->dmrTxPriv <= C62_TMEM_TX_AES_MAX) {
            uint16_t id = 0;
            if (dmrAes_entry(rtx->dmrTxPriv - C62_TMEM_TX_AES1, &id, txAesKey,
                             &txAesLen)
                && id >= 1u && id <= 255u) {
                txPriv = 3;
                txEpId = (uint8_t)id;
                txMi = txNewMi();
                txPis = 1;
            } else {
                dmrAes_wipe(txAesKey, sizeof(txAesKey));
            }
        }
        /* Encrypted: the own ID for it (Settings, dmr.privid), if set */
        txSrc = (txPriv != 0 && c62_dmrPrivId != 0u) ? c62_dmrPrivId
                                                     : c62_dmr.id;
#ifdef CONFIG_DMR_TRUNK
        if (txT3 != 0)
            dmr_tx_setup(&txCall, txSrc, txT3Dst, txT3Group, txT3Cc);
        else
#endif
        dmr_tx_setup(&txCall, txSrc, c62_dmr.tg, !c62_dmr.privateCall,
                     c62_dmr.cc);
        if (txPriv == 4 || txPriv == 5)
            dmr_tx_privacy_fid(&txCall, 0x68u); /* Hytera's */
        else if (txPriv != 0)
            dmr_tx_privacy(&txCall);
        /* Settings > Radio > Polar. DMR TX flips the default */
        dmr_mod_init(&txMod, DMR_TX_GAIN,
                     c62_dmr.txFlip ? !DMR_TX_INVERT : DMR_TX_INVERT);
        memset(baseband, 0, sizeof(baseband));
#ifdef CONFIG_DMR_TRUNK
        /* The engine's bursts: the first one now (none left: nothing) */
        if (txT3 == 1 && !dmrTrunk_sendNext(txT3Slot, &t3Dt, t3B)) {
            dmrTrunk_sendDone(false);
            txT3 = 0;
            rtx->opStatus = OFF;
            startRx = true;
            return;
        }
        const bool micOk = txWake || txT3 == 1 || txData || dmrTxEnc_start();
#else
        const bool micOk = txWake || txData || dmrTxEnc_start();
#endif
        /* The receive path stays open through the transmission (see
         * releaseRxPath()): only if the microphone failed is it let go,
         * with everything else, on the way back */
        if (!micOk) {
            releaseRxPath();
            printk("[DMR TX] no se pudo abrir el micro\n");
            rtx->opStatus = OFF;
            startRx = true;
            return;
        }
        /*
         * The probe's echo: armed before the stream starts.
         *
         * UNREACHABLE on this radio, and it is not an oversight. The echo
         * reference is a channel of the DSP's capture, and the DSP refuses
         * it at our sample rate:
         *   [ERR] AudioStreamInVenus: DAC echo capture only supports
         *                             16000 Hz recording
         * We capture at 48 kHz because the demodulator wants ten samples
         * per symbol at 4800 bauds. So echo_channel stays -1, echoArm()
         * returns false and the output time is always the estimate from
         * the DSP's queue below. Opening the capture at 16 kHz, or any
         * other way of giving the transmitter a measured output time,
         * means changing what the demodulator needs first.
         */
        txEchoArmed = csk6codec_echoArm(ECHO_WINDOW);
        printk("[DMR TX] captura: %s\n", csk6codec_echoDiag());
        txEchoKnown = false;
        txEstimated = false;
        txEchoPeak = -1;
        txTs1Cap = (int64_t)rptTs1Cap;
        txTs1Shift = 0;
        txPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
        if (txPath >= 0)
            txStream = audioStream_start(txPath, baseband, 2 * HALF_SAMPLES,
                                         DMR_IN_RATE,
                                         STREAM_OUTPUT | BUF_CIRC_DOUBLE);
        if (txStream < 0) {
            printk("[DMR TX] no se pudo abrir la salida al transmisor\n");
            txEnd(rtx);
            return;
        }
        txIdle = outputStream_getIdleBuffer(txStream);
        txHalf = 1; /* The stream plays its first half while we fill this */
        /* Above the encoder thread (13): the transmitter must be fed every
         * 30 ms, while a burst of three frames takes it up to 35 ms */
        k_thread_priority_set(k_current_get(), 12);
        txHz = rtx->txFrequency;
#ifdef CONFIG_DMR_TRUNK
        if (txT3 != 0)
            txHz = txT3Hz;
        radio_c62_setTxFrequency(txT3 != 0 ? txT3Hz : 0);
#endif
        radio_enableTx();
        if (txRepeater && !txWake && !txFree)
            radio_c62_paGate(false); /* No power until our slot is known */
        if (txFree) {
            /* Drive full on, not gated: gateStart() would ramp the
             * excitation down and leave it off, because nothing calls
             * gateWindow() when the gating is disabled. That cost about
             * 13 dB on the air (BrandMeister measured -118 dBm against the
             * -105 of a DM-1801 next to it). */
            radio_c62_gateStop(true);
            txGating = false;
            txGateWins = 0;
            radio_c62_paGate(true);
        }
        txEnding = false;
        txTermSent = false;
        txHeaders = DMR_TX_HEADERS;
#ifdef CONFIG_DMR_TRUNK
        if (txT3 == 1)
            txHeaders = 0; /* Only the engine's bursts */
#endif
        if (txData) {
            txHeaders = 0; /* Only the list's bursts */
            printk("[DMR TX] datos: %s\n",
                   dmrSms_txState(NULL, NULL) == DMR_SMS_TX_QUEUED
                       ? "mensaje de texto" : "respuesta a un mensaje");
        }
        txVoice = txSilent = 0;
        txBurstSym = -1;
        txBurstK = 0;
        /* Simplex from the second half written; through a repeater once its
         * slot is known; a wake-up at once (any timing goes) */
        txNextSym = (txRepeater && !txFree) ? -1 : 2 * HALF_SYMS;
        if (txWake)
            printk("[DMR TX] despertando al repetidor: %u BS_Dwn_Act (intento "
                   "%u de %u)\n", (unsigned)WAKE_BURSTS,
                   (unsigned)txWakeCount, (unsigned)N_WAKEUP);
        else if (txFree && txRepeater)
            printk("[DMR TX] llamada con temporizacion libre (sin alinear a "
                   "su slot)\n");
        printk("[DMR TX] excitacion del PA: REG_36 = 0x%04x (%s)\n",
               (unsigned)c62_tx_reg36,
               c62_tx_reg36 == 0xdfbfu ? "a tope"
                                       : (c62_tx_reg36 == 0xdf3fu
                                              ? "APAGADA"
                                              : "parcial"));
        txStart = (uint32_t)getTick();
        printk("[DMR TX] inicio: ID %lu -> %s %lu, CC %u, TS%u, %lu Hz, "
               "%lu mW, polaridad %s, %s\n",
               (unsigned long)txSrc,
               txT3 != 0 ? (txT3 == 1 ? "troncal" : (txT3Group ? "grupo T3"
                                                    : "privada T3"))
                         : (c62_dmr.privateCall ? "privada" : "grupo"),
               (unsigned long)(txT3 != 0 ? txT3Dst : c62_dmr.tg),
               (unsigned)(txT3 != 0 ? txT3Cc : c62_dmr.cc),
               (unsigned)txSlotUse(), (unsigned long)txHz,
               (unsigned long)rtx->txPower, c62_dmr.txFlip ? "invertida" : "normal",
               txRepeater ? "repetidor" : "simplex");
        if (txPriv == 1)
            printk("[DMR TX] cifrado Basic Privacy (clave de la memoria)\n");
        else if (txPriv == 2)
            printk("[DMR TX] cifrado Enhanced Privacy (ARC4), entrada %u del "
                   "llavero, ID de clave %u, MI %08lx\n",
                   (unsigned)(rtx->dmrTxPriv - C62_TMEM_TX_EP1 + 1),
                   (unsigned)txEpId, (unsigned long)txMi);
        else if (txPriv == 3)
            printk("[DMR TX] cifrado %s, clave %u, ID de clave %u, "
                   "MI %08lx\n",
                   txAesLen == DMR_KEY_DES ? "DES"
                   : (txAesLen == DMR_KEY_AES128 ? "AES-128" : "AES-256"),
                   (unsigned)(rtx->dmrTxPriv - C62_TMEM_TX_AES1 + 1),
                   (unsigned)txEpId, (unsigned long)txMi);
        else if (txPriv == 4)
            printk("[DMR TX] cifrado Hytera Basic (su clave)\n");
        else if (txPriv == 5)
            printk("[DMR TX] cifrado Hytera Enhanced, entrada EP %u, ID de "
                   "clave %u, MI %010llx\n",
                   (unsigned)(rtx->dmrTxPriv - C62_TMEM_TX_HEP1 + 1),
                   (unsigned)txEpId, (unsigned long long)txMiHy);
    }

    /* Data goes out whole; voice ends with the PTT */
    if (!txEnding && ((!txData && !platform_getPttStatus())
                      || rtx->txDisable)) {
        txEnding = true;
        txEndingAt = getTick();
    }
    /* Safety: whatever the schedule, never on air more than 1 s after the
     * PTT was released (a wake-up lasts 0.2 s, a terminator 60 ms) */
    if (txEnding && getTick() - txEndingAt > 1000) {
        printk("[DMR TX] corte de seguridad: la transmision no termino\n");
        txEnd(rtx);
        return;
    }
    if (txWake && getTick() - txWakeAt > 2200) {
        printk("[DMR TX] corte de seguridad: despertar demasiado largo\n");
        txEnd(rtx);
        return;
    }

    /* Where the probe left the DAC, in capture samples */
    if (txEchoArmed && !txEchoKnown) {
        uint64_t idx;
        int32_t peak;
        if (csk6codec_echoResult(&idx, &peak)) {
            txEchoArmed = false;
            txEchoPeak = peak;
            if (peak >= ECHO_MIN) {
                txEchoKnown = true;
                txDelta = (int64_t)idx
                        - ((int64_t)PROBE_MID * DMR_TX_SPS + RRC_DELAY);
                /* idx is a sample of the capture running now, so the TS1
                 * reference has to be moved into it too */
                txFreezeTs1();
            }
        }
    }

    if (txRepeater && !txWake && !txFree && txNextSym < 0
        && !txTermSent) {
        if (txEnding) {
            txEnd(rtx);
            return;
        }
        /*
         * Without the DAC's echo: where our audio leaves, estimated from the
         * DSP's output queue (csk6codec_rtxOutputTime()), once it settled
         * (the DSP fills its own buffer at the start), against the capture
         * time base. Its constant error goes into the correction searched
         * with the Parrot.
         */
        if (!txEchoKnown && !txEchoArmed && txHalf >= SETTLE_HALVES) {
            const uint32_t qref = (uint32_t)(txHalf * HALF_SAMPLES);
            const uint64_t cnow = csk6codec_captureCount();
            int64_t outUs, capUs;
            if (csk6codec_rtxOutputTime(qref, &outUs)
                && csk6codec_captureTime(cnow, &capUs)) {
                txDelta = (int64_t)cnow
                        + (outUs - capUs + AUDIO_TB_ERR_US) * 48 / 1000
                        - (int64_t)qref;
                txEchoKnown = true;
                txEstimated = true;
                txFreezeTs1();
            } else if (txHalf > SETTLE_HALVES + 40) {
                printk("[DMR TX] sin medida de tiempo de la salida\n");
                txEnd(rtx);
                return;
            }
        }
        if (txEchoKnown) {
            /* The first slot of ours far enough ahead */
            int32_t ppm = 0;
            uint32_t frames;
            if (!rptPpm(&ppm, &frames))
                ppm = 0;
            /*
              * A whole number of symbols per frame. txSymFor() rounds the
              * burst's position to a symbol (10 samples, 208 us), so a
              * fractional period makes that rounding jump a whole symbol
              * every half second or so. An MMDVM repeater only looks for
              * the sync within about +-42 us of where it expects it, and
              * the voice sync only comes round once every 360 ms (burst
              * A), so those jumps cost it the sync and after 13 bursts it
              * drops the call: the header is registered and the call never
              * sustains. The repeater's own drift (+24 ppm, 8.6 us per
              * superframe) is smooth and it tracks that by itself.
              */
            txPeriod = FRAME_SAMPLES
                     * (double)llround(DMR_TX_SPS * (1.0 + ppm * 1e-6))
                     / DMR_TX_SPS;
            /*
             * Which physical 30 ms window our burst goes in, counted from
             * the repeater's outbound TS1 burst (txTs1Cap): ALIGNED channel
             * timing (ETSI TS 102 361-1 table 5.1), TS1 at the outbound TS1
             * position, TS2 half a frame later.
             *
             * From 2026-11-15 TS1 was put half a frame later ("offset"
             * timing) because BrandMeister logged a TS1 call on slot 2.
             * The latency measurement of 2026-11-16 explains that monitor
             * line instead: every burst left 27.6 ms early (AUDIO_TB_ERR_US),
             * so an aligned TS1 burst landed 2.4 ms into the slot before,
             * slot 2. The extra half frame then put TS1 2.4 ms late: in the
             * right slot, but outside the MMDVM's +-1.9 ms acquisition
             * window, hence roughly half the calls getting in.
             */
            const double base = (double)txTs1Cap
                              + (txSlotUse() == 2 ? FRAME_SAMPLES / 2 : 0)
                              + c62_dmr.txCal * (DMR_IN_RATE / 2000);
            const double minC = (double)(txHalf * HALF_SYMS * DMR_TX_SPS
                                         + RRC_DELAY + txDelta);
            const double k = ceil((minC - base) / txPeriod);
            txTgt0 = base + k * txPeriod;
            txBurstK = 0;
            txNextSym = txSymFor(0);
            /*
             * Every term of the burst position, one by one, in samples.
             * The summary at the end of the call reports "N ms after the
             * repeater's TS1" computed against this very reference, so it
             * says what we aimed at and can never reveal an error in the
             * aim itself. These are the ingredients: whichever of them
             * moves between calls is the one that is not trustworthy.
             *   ts1    reference taken from the repeater, as received
             *   mov    what it had to move by for the restarted capture
             *   ranura 0 for logical channel 1, half a frame for 2
             *   ajuste the manual correction
             *   tramas whole frames added to reach the first slot ahead
             * The four of them plus the frames ARE the target. The output
             * delay is not one of them: it only turns capture samples into
             * stream symbols afterwards, so it is printed apart.
             */
            printk("[DMR TX] posicion: ts1 %lld + mov %lld + ranura %ld + "
                   "ajuste %ld + %lld tramas -> objetivo %lld; salida %lld "
                   "(%s) -> primera rafaga en el simbolo %lld\n",
                   (long long)(txTs1Cap - txTs1Shift),
                   (long long)txTs1Shift,
                   (long)(txSlotUse() == 2 ? FRAME_SAMPLES / 2 : 0),
                   (long)(c62_dmr.txCal * (DMR_IN_RATE / 2000)),
                   (long long)(k * txPeriod),
                   (long long)txTgt0,
                   (long long)txDelta, txEstimated ? "estimada" : "medida",
                   (long long)txNextSym);
            /*
              * Carrier on throughout, no drive gating between the bursts.
              * A real MS does burst its transmitter, but our window is
              * commanded in real time from an output time estimate whose
              * error is unknown, so it could sit off while the burst went
              * out: with gating on, nothing ever reached the repeater,
              * while every call that did reach it (BrandMeister, 2026-11-14
              * 15:43 and 16:10) went out with a steady carrier. Put the
              * gating back only once that estimate can be measured.
              */
            /* Drive full on: see the note in the free running path, the
             * gate leaves the excitation off when it is not driven */
            radio_c62_gateStop(true);
            txGating = false;
            txGateWins = 0;
            radio_c62_paGate(true);
            /*
              * No sweeping inside a call: each step resent a voice header
              * and restarted the superframe every 360 ms, so the repeater
              * only ever saw a call being started over and over (it showed
              * up on BrandMeister with a 00:00 duration) and the Parrot had
              * nothing to answer. The search now moves one step per PTT
              * press instead (rptTiming()), so every transmission is a real
              * call the repeater can accept.
              */
            /*
             * Put back 2026-11-14 night: the two calls that did show up on
             * BrandMeister that afternoon (15:43 and 16:10) were sweeping.
             * Removing it was mine and it was wrong: the sweep's calls last
             * 00:00 because each step restarts the call, so the Parrot has
             * nothing to answer and the radio cannot detect the answer
             * itself, but the call IS registered, and the monitor's clock
             * against the "paso N" lines of the log says which step got
             * through. One press of the PTT covers the whole range that way
             * instead of twenty-five.
             *
             * Only with the automatic search on (key 0 on Ajustes > DMR >
             * Ajuste TX DMR): with a fixed correction the call stays whole
             * and the Parrot can answer.
             */
            /* Not while encrypting: every step forces voiceIdx to 0,
             * which runs the message indicator on without a PI header to
             * announce it, and the receiver advances its own by time. The
             * keystreams part company at the first step. */
            /* Off: with the automatic search on, it swept within the
             * call and never locked anything; the search is now one value
             * per call (calNext()), locked by the Parrot's answer */
            txSweep = false;
            txSweepStep = 0;
            txSweepCal = c62_dmr.txCal;
            txSegBursts = 0;
            if (txSweep) {
                char cal[16];
                calText(cal, sizeof(cal), txSweepCal);
                printk("[DMR TX] paso 0: ajuste %s\n", cal);
            }
        }
    }

    if (txStream < 0 || txIdle == NULL) {
        /* The output endpoint failed (the DSP stalled, the stream was
         * terminated): writing through txIdle would be a write through
         * NULL, and the loop never blocks again once sync() errors out */
        printk("[DMR TX] se perdio la salida de audio\n");
        txEnd(rtx);
        return;
    }

    /* This half, symbol by symbol */
    const int64_t h0 = txHalf * HALF_SYMS;
    for (int s = 0; s < HALF_SYMS; s++) {
        const int64_t sym = h0 + s;
        if (txNextSym >= 0 && sym >= txNextSym) {
#ifdef CONFIG_DMR_TRUNK
            if (txT3 == 1) {
                /* The engine's burst fetched ahead, then the next one for
                 * this slot's next frame: none, this one is the last */
                dmr_tx_block(txT3Cc, t3Dt, t3B, txBurst);
                if (!dmrTrunk_sendNext(txT3Slot, &t3Dt, t3B)) {
                    txTermSent = true;
                    txTermEnd = sym + DMR_TX_BURST_DIBITS + T3_TAIL_SYMS;
                }
            } else
#endif
            if (txWake) {
                /* BS_Dwn_Act CSBKs, 60 ms apart, nothing else: the only
                 * burst a sleeping repeater wakes on (see txWanted()) */
                dmr_tx_wakeup(&txCall, 0xFFFFFFu, txSrc, txBurst);
                if (++txWakeBursts >= WAKE_BURSTS) {
                    txTermSent = true;
                    txTermEnd = sym + DMR_TX_BURST_DIBITS + TAIL_SYMS;
                }
            } else if (txData) {
                /* The list's burst fetched ahead, then the next: none, this
                 * one is the last */
                dmr_tx_block(c62_dmr.cc, dataDt, dataB, txBurst);
                if (!dmrSms_txNext(&dataDt, dataB)) {
                    txTermSent = true;
                    txTermEnd = sym + DMR_TX_BURST_DIBITS + DATA_TAIL_SYMS;
                }
            } else if (txHeaders > 0) {
                dmr_tx_header(&txCall, txBurst);
                txHeaders--;
            } else if (txPis > 0 && !txEnding) {
                /* Enhanced Privacy: the PI header, between the voice
                 * headers and the voice */
                if (txPriv == 5)
                    dmr_tx_pi_hytera(&txCall, txEpId, txMiHy, txBurst);
                else
                dmr_tx_pi_alg(&txCall, txPriv == 3 ? dmrAes_dmrAlg(txAesLen)
                                                   : 0x21u,
                          txEpId, txMi,
#ifdef CONFIG_DMR_TRUNK
                          txT3 != 0 ? txT3Dst :
#endif
                          c62_dmr.tg, txBurst);
                txPis--;
            } else if (txEnding) {
                dmr_tx_terminator(&txCall, txBurst);
                txTermSent = true;
                txTermEnd = sym + DMR_TX_BURST_DIBITS + TAIL_SYMS;
            } else {
                uint8_t ambe[3][9];
                const int vidx = txCall.voiceIdx;
                /* Each new superframe of Enhanced Privacy: the next MI */
                if (txPriv == 5 && vidx == 0 && txVoice > 0)
                    txMiHy = dmrHyEp_nextMi(txMiHy);
                if ((txPriv == 2 || txPriv == 3) && vidx == 0 && txVoice > 0)
                    txMi = txPriv == 3 ? dmrAes_nextMi(txMi)
                                       : dmrEp_nextMi(txMi);
                /* Silence when the encoder is late goes as it is, in clear,
                 * as the receivers expect it (dmrEp_frame()) */
                /* TYT's BP: clear frames, the burst enciphered after */
                const bool tytTx = txPriv == 1 && txBpTyt;
                if (!((txPriv != 0 && !tytTx) ? txPrivFrames(ambe, vidx)
                                              : dmrTxEnc_pop(ambe))) {
                    for (int k = 0; k < 3; k++)
                        memcpy(ambe[k], ambeSilence, 9);
                    txSilent++;
                }
                dmr_tx_voice(&txCall, ambe, txBurst);
                if (tytTx)
                    dmrBpTyt_burst(txBpKey, txBurst);
                txVoice++;
                /*
                 * Sweep: after each superframe the next correction, 2.5 ms
                 * later, starting with a voice header so that the repeater
                 * takes the call from there. The step and its time are on
                 * the log, to match with the BrandMeister monitor.
                 */
                if (txSweep && ++txSegBursts >= SWEEP_BURSTS) {
                    txSegBursts = 0;
                    txSweepStep++;
                    int v = txSweepCal + CAL_STEP;
                    if (v > C62_DMR_CAL_MAX) {
                        /* The range is exactly one 60 ms frame, so MIN and
                         * MAX are the same position: wrapping to MIN spent
                         * a whole step of the sweep emitting again where it
                         * had just been, and the log showed two steps that
                         * cannot be told apart on the monitor's clock. */
                        v = C62_DMR_CAL_MIN + CAL_STEP;
                    }
                    txTgt0 += (double)(v - txSweepCal)
                            * (DMR_IN_RATE / 2000);
                    txSweepCal = v;
                    /*
                     * Moving the target backwards would put the next burst
                     * inside the one just started: the range is exactly one
                     * 60 ms frame, so wrapping it moved txTgt0 by a whole
                     * frame and txSymFor(k+1) landed on this very symbol,
                     * cutting the burst to one symbol of its 132. Walk k on
                     * until the schedule clears the burst in flight.
                     */
                    for (unsigned guard = 0;
                         guard < 64u
                         && txSymFor(txBurstK + 1)
                                < sym + DMR_TX_BURST_DIBITS;
                         guard++) {
                        txBurstK++;
                    }
                    txHeaders = 1;
                    txCall.voiceIdx = 0;
                    char cal[16];
                    calText(cal, sizeof(cal), v);
                    printk("[DMR TX] paso %d: ajuste %s\n", txSweepStep, cal);
                } else if (txSweep && txSegBursts == SWEEP_HDR_BURST) {
                    /* See SWEEP_HDR_BURST: the repeater has had time to
                     * drop its old lock and is searching again */
                    txHeaders = 1;
                    txCall.voiceIdx = 0;
                }
            }
            txBurstSym = sym;
            /* Its drive window, from the stream samples of its first and
             * last symbol pulses to their uptime at the DAC */
            if (txGating) {
                const uint32_t s0 = (uint32_t)(sym * DMR_TX_SPS);
                const uint32_t s1 = (uint32_t)((sym + DMR_TX_BURST_DIBITS)
                                                   * DMR_TX_SPS
                                               + 2 * RRC_DELAY);
                int64_t t0, t1;
                if (csk6codec_rtxOutputTime(s0, &t0)
                    && csk6codec_rtxOutputTime(s1, &t1)
                    && radio_c62_gateWindow(t0 - GATE_PRE_US,
                                            t1 + GATE_POST_US)) {
                    txGateWins++;
                } else {
                    /* Rather the carrier throughout than lost bursts */
                    radio_c62_gateStop(true);
                    txGating = false;
                    printk("[DMR TX] corte de PA entre rafagas desactivado: "
                           "sin hora de salida (%lu rafagas cortadas)\n",
                           (unsigned long)txGateWins);
                }
            }
            if (txTermSent)
                txNextSym = -1;
            else if (txRepeater && !txWake && !txFree)
                txNextSym = txSymFor(++txBurstK);
            else /* Simplex, or the wake-ups: every 60 ms */
                txNextSym = sym + FRAME_SAMPLES / DMR_TX_SPS;
        }
        uint8_t probe = 1; /* Dibit 01: +3 */
        const uint8_t *d = NULL;
        if (sym >= PROBE_SYM && sym < PROBE_SYM + PROBE_SYMS && !txWake
#ifdef CONFIG_DMR_TRUNK
            && txT3 == 0 /* never a tone into a trunked system's slots */
#endif
            )
            d = &probe;
        else if (txBurstSym >= 0 && sym >= txBurstSym
                 && sym < txBurstSym + DMR_TX_BURST_DIBITS)
            d = &txBurst[sym - txBurstSym];
        dmr_mod_dibits(&txMod, d, 1, txIdle + s * DMR_TX_SPS);
    }
    outputStream_sync(txStream, true);
    txIdle = outputStream_getIdleBuffer(txStream);
    txHalf++;
#ifdef CONFIG_DMR_TRUNK
    /* Our own voice is the call's activity: the radio never hears its own
     * slot, and TV_Inactive would end the call while it talks */
    if (txT3 == 2)
        dmrTrunk_activity(txT3Slot - 1);
#endif

    /* The terminator, and a half of silence after it, are queued */
    if (txTermSent && h0 >= txTermEnd)
        txEnd(rtx);
}
#else
bool OpMode_DMR_RX::txWanted(const rtxStatus_t *)
{
    return false;
}

void OpMode_DMR_RX::txUpdate(rtxStatus_t *rtx)
{
    rtx->opStatus = OFF;
    startRx = true;
}

void OpMode_DMR_RX::txEnd(rtxStatus_t *)
{
}

/* Nothing of a repeater is tracked without the transmitter */
static void rptForget(void)
{
}
#endif

OpMode_DMR_RX::~OpMode_DMR_RX()
{
    disable();
}

/*
 * Ajustes > DMR > Escuchar (c62_dmrCalls.filter), as OpenGD77's
 * checkTalkGroupFilter() (HR-C6000.c, 2022): a private call to us, or with
 * the radio we are in a private call with, is always heard; other private
 * calls only with Todos. Group calls: Todos every one; Mi TG the talkgroup
 * we transmit to; Lista the list's talkgroups and that one. The all-call
 * (ETSI TS 102 361-2: ID 0xFFFFFF) is always heard, and Tier III follows
 * its own setting (T3 sigue).
 */
static bool c62CallHeard(bool group, uint32_t dst, uint32_t src)
{
#ifdef CONFIG_DMR_TRUNK
    if (dmrTrunk_running())
        return true;
#endif
    if (dst == 0xFFFFFFu)
        return true;
    if (!group) {
        if (c62_dmr.id != 0u && dst == c62_dmr.id)
            return true;
        if (c62_dmr.privateCall && (src == c62_dmr.tg || dst == c62_dmr.tg))
            return true;
        return c62_dmrCalls.filter == C62_DMR_FILTER_ALL;
    }
    switch (c62_dmrCalls.filter) {
        case C62_DMR_FILTER_TG:
            return dst == c62_dmr.tg;
        case C62_DMR_FILTER_LIST:
            if (dst == c62_dmr.tg && !c62_dmr.privateCall)
                return true;
            for (unsigned i = 0; i < C62_TGL_COUNT; i++) {
                const uint32_t v = c62_dmrCalls.list[i].id;
                if (v != 0u && (v & C62_TGL_PRIVATE) == 0u
                    && C62_TGL_ID(v) == dst)
                    return true;
            }
            return false;
        default:
            return true;
    }
}

void OpMode_DMR_RX::enable()
{
    dmr_voice_set_filter(c62CallHeard);
    /* Entering the mode: nothing learnt about any repeater carries over,
     * whatever the frequencies happen to be */
    cfgDirty = true;
    cfgRxHz = cfgTxHz = 0;
    dmr_demod_init();
    dmr_proto_set_printer(printLine);
    dmrSpeech_start();
    startRx = true;
}

void OpMode_DMR_RX::disable()
{
#ifdef CONFIG_DMR_TX
    if (txStream >= 0 || txPath >= 0) {
        rtxStatus_t dummy;
        txEnd(&dummy);
    }
#endif
    startRx = false;
    startTx = false;
#ifdef CONFIG_DMR_TRUNK
    dmrTrunk_stop();
#endif
    stopSampling();
    dmrSpeech_stop();
    radio_disableRtx();
    platform_ledOff(GREEN);
    dmr_demod_reset();
}

/*
 * The receive path, kept open through every transmission (the wake-up's
 * too) and released only when the mode stops (stopSampling()) or the
 * microphone could not be opened: the DSP capture then never restarts.
 * Every restart starts its time base afresh, off by several ms for a
 * while (as the play time base, csk6_codec.c TB_PLAY_SKIP_MS), and the
 * repeater's grid is read through it: on 2026-11-16 every call made just
 * after the C62's own wake-up failed, while one made after a DM-1801 woke
 * the repeater (the C62 had not transmitted, its capture still running)
 * got in at once on TS1 (03:10:03).
 *
 * Earlier: released once the microphone's path was open.
 *
 * Releasing it at the PTT stopped the DSP capture until the encoder opened
 * the microphone, some ms later, and restarted it: its sample count skips
 * the gap, so txFreezeTs1() had to move the repeater's TS1 reference by a
 * gap measured through two time bases, the new one barely settled. On
 * 2026-11-15 that move was -10 to -14.5 ms and every call the Parrot
 * answered had one, while the two calls without it did not get in: the
 * move's own error, a couple of ms either way, decided each call, against
 * the MMDVM's +-1.9 ms window. Both paths end at the MCU and are
 * compatible (audio_checkPathCompatibility()), so they can overlap.
 */
void OpMode_DMR_RX::releaseRxPath()
{
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandPath = -1;
}

void OpMode_DMR_RX::startSampling()
{
    if (basebandPath < 0)
        basebandPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(basebandPath, baseband, 2 * HALF_SAMPLES,
                                   DMR_IN_RATE, STREAM_INPUT | BUF_CIRC_DOUBLE);
}

void OpMode_DMR_RX::stopSampling()
{
    if (basebandId >= 0)
        audioStream_terminate(basebandId);
    if (basebandPath >= 0)
        audioPath_release(basebandPath);
    basebandId = -1;
    basebandPath = -1;
}

void OpMode_DMR_RX::update(rtxStatus_t *const rtx, const bool newCfg)
{
    if (newCfg
        && (rtx->rxFrequency != cfgRxHz || rtx->txFrequency != cfgTxHz)) {
        /* A different channel, not merely a reconfiguration: only then is
         * what we learnt about a repeater worthless. Acted on below. */
        cfgDirty = true;
    }
    cfgRxHz = rtx->rxFrequency;
    cfgTxHz = rtx->txFrequency;
    /* The tuned memory's Basic Privacy key (0 none) */
    dmr_voice_set_bp_scan(rtx->dmrBpScan, rtx->rxFrequency);
    dmr_voice_set_bp_key(rtx->dmrBpKey, rtx->dmrBpAnytone != 0u);
    dmr_voice_set_bp_tyt(rtx->dmrBpTyt != 0u);
    /*
     * Through a repeater only our own slot is heard, as on any DMR radio:
     * left on automatic, the voice followed whichever slot spoke first, and
     * on 2026-11-16 a TG 214 QSO on TS1 kept the Parrot's answer on TS2
     * (decoded: "TS2 ... PRIVATE ID 2144723 SRC 9990") off the speaker.
     * Simplex (same frequency both ways) has no slots to choose.
     */
    {
        static int lastPref = -2;
        int pref = (rtx->txFrequency != rtx->rxFrequency
                          && c62_dmrRxSlot != C62_DMR_RX_AUTO)
                       ? (c62_dmrRxSlot == 2u ? 1 : 0)
                       : DMR_VOICE_PREF_AUTO;
#ifdef CONFIG_DMR_TRUNK
        /* Tier III: started on this channel, stopped when set off; the
         * voice of the call followed is on the slot its grant named */
        if (c62_t3.mode != C62_T3_OFF) {
            if (!dmrTrunk_running() || trunkCtrlHz != rtx->rxFrequency) {
                dmrTrunk_start(rtx->rxFrequency, rtx->txFrequency);
                trunkCtrlHz = rtx->rxFrequency;
                trunkRetune = true;
            }
        } else if (dmrTrunk_running()) {
            dmrTrunk_stop();
            if (trunkHz != rtx->rxFrequency) {
                radio_c62_setRxFrequency(rtx->rxFrequency);
                dmr_demod_reset();
            }
            trunkHz = rtx->rxFrequency;
        }
        if (dmrTrunk_running()) {
            dmrTrunkStatus_t ts;
            dmrTrunk_getStatus(&ts);
            pref = (ts.state == 3u && ts.slot >= 1u) ? (int)ts.slot - 1
                                                     : DMR_VOICE_PREF_AUTO;
        }
        if (newCfg)
            trunkRetune = true;
        dmrTrunk_ptt(platform_getPttStatus());
#endif
        if (pref != lastPref) {
            lastPref = pref;
            dmr_voice_set_pref(pref);
        }
    }

    switch (rtx->opStatus) {
        case OFF:
            radio_disableRtx();
            if (cfgDirty) {
                /* This case goes straight to TX without touching the
                 * receive path below, and a call started here would be
                 * scheduled against the previous channel's repeater */
                cfgDirty = false;
                dmr_demod_reset();
                rptForget();
            }
            if (txWanted(rtx)) {
                rtx->opStatus = TX;
                startTx = true;
                return;
            }
            if (startRx) {
                rtx->opStatus = RX;
                return;
            }
            sleepFor(0, 30);
            return;

        case RX:
            break;

        case TX:
            txUpdate(rtx);
            return;

        default:
            rtx->opStatus = OFF;
            startRx = true;
            return;
    }

    /* A new frequency is a new channel: start from scratch. Before the
     * PTT check, or a configuration change arriving in the same call with
     * the PTT already down would transmit on the new channel with the old
     * repeater's timing. */
    if (cfgDirty) {
        cfgDirty = false;
        dmr_demod_reset();
        rptForget();
    }

    /* PTT: stop receiving, OFF starts the transmission. Only the stream:
     * the receive path stays until the microphone's is open (txUpdate()),
     * so that the capture never stops in between (see releaseRxPath()). */
    if (txWanted(rtx)) {
        if (basebandId >= 0)
            audioStream_terminate(basebandId);
        basebandId = -1;
        startRx = true;
        rtx->opStatus = OFF;
        return;
    }

    if (startRx) {
        startSampling();
        radio_enableRx();
        startRx = false;
        rxStartMs = getTick();
#ifdef CONFIG_DMR_TRUNK
        /* radio_enableRx() tuned the channel's frequency */
        trunkHz = rtx->rxFrequency;
        trunkRetune = true;
#endif
    }

    if (audioPath_getStatus(basebandPath) != PATH_OPEN) {
        sleepFor(0, 10);
        return;
    }

    /*
     * Wait for the next half buffer of samples. A failed read means we were
     * late and samples were lost; the framer finds the bursts again by
     * itself. Do not lose more by sleeping, unless the stream keeps failing.
     */
    dataBlock_t block = inputStream_getData(basebandId);
    if (block.data == NULL) {
        if (dataErrors++ == 0)
            dmr_demod_note_overrun();
        else
            sleepFor(0, 10);
        return;
    }
    dataErrors = 0;

    /* The demodulator takes any scale: levels come from the sync words */
    size_t n = block.len;
    if (n > HALF_SAMPLES)
        n = HALF_SAMPLES;
    float sum = 0.0f, sum2 = 0.0f;
    for (size_t i = 0; i < n; i++) {
        const float x = (float)block.data[i];
        sum += x;
        sum2 += x * x;
    }
#ifdef CONFIG_DMR_TX
    /* This block ends at the driver's last boundary of the radio input */
    {
        uint64_t boundary;
        const int half = (block.data == baseband) ? 0 : 1;
        if (csk6codec_rtxInputBoundary(half, &boundary) && boundary >= n) {
            rxCapOffset = (int64_t)(boundary - n) - (int64_t)dmr_demod_fed();
            rxMapOk = true;
        }
    }
#endif
    for (size_t i = 0; i < n; i += FLOAT_CHUNK) {
        float chunk[FLOAT_CHUNK];
        const size_t m = (n - i < FLOAT_CHUNK) ? n - i : FLOAT_CHUNK;
        for (size_t k = 0; k < m; k++)
            chunk[k] = (float)block.data[i + k];
        dmr_demod_feed_disc(chunk, (int)m);
    }
#ifdef CONFIG_DMR_TX
    rptTiming();
#endif

#ifdef CONFIG_DMR_TRUNK
    /* Tier III: the level for the engine, then where it wants to listen */
    if (dmrTrunk_running()) {
        uint32_t hz;
        uint8_t ts;
        dmrTrunk_level((int)radio_getRssi());
        if (dmrTrunk_poll(&hz, &ts) && hz != 0
            && (hz != trunkHz || trunkRetune)) {
            radio_c62_setRxFrequency(hz);
            /* Another carrier: its own bursts, slots and timing */
            if (hz != trunkHz) {
                dmr_demod_reset();
                rptForget();
            }
            trunkHz = hz;
            trunkRetune = false;
        }
    }
#endif

    dmr_demod_status_t ds;
    dmr_demod_get_status(&ds);
    printStatus(ds, n ? sum / n : 0.0f, n ? sum2 / n : 0.0f);
    if (ds.locked)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);
}
