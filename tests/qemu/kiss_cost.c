/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Bare-metal Cortex-M33 cost harness for the packet modem, run under QEMU with
 * the instruction counter of work/dsp-probe (scripts/kiss_cost.sh).
 *
 * For each modem every stage is bracketed by two frame_marker() calls, so the
 * snapshot deltas the plugin dumps are the cost of exactly that stage:
 *
 *   arm     pkt_engine_init() + pkt_tx_begin(): frame build, flags, TXDELAY
 *   gen     pkt_tx_block() over the whole burst   (reported per 10 ms block)
 *   re-arm  pkt_engine_init() + pkt_rx_begin()
 *   dem     pkt_rx_block() over the same audio    (reported per 10 ms block)
 *   kiss    decode + re-encode of one KISS frame
 *
 * The audio is generated here by the same modulator the radio uses, so the
 * demodulator sees a real signal (a 330 byte AX.25 frame, flags included) and
 * not noise: the cost of the decisions is the cost of the path that locks.
 * `rx_frames` in the report is the proof that it locked: a zero there means
 * the numbers describe a demodulator that never found a frame.
 *
 * Nothing is written to the console between two markers: semihosting a line
 * costs more than some of the stages and would be charged to whichever stage
 * it fell in. The summaries are collected and printed after the last marker.
 *
 * Four things this file learned the hard way (all of them were why the first
 * version reported marker=0 and produced no numbers):
 *
 *   - The image must be linked at 0x10000000, not 0x00000000. mps2-an505 boots
 *     from the vector table at 0x10000000 and ignores the ELF entry point, so
 *     linked at 0x0 it started at PC=0, took an INVSTATE UsageFault and
 *     executed the vector table as code.
 *   - It must end with exit(), not a WFI loop: QEMU only runs the plugin's
 *     atexit dump when the guest exits.
 *   - -fno-ipa-icf is needed on the command line: without it GCC folds this
 *     empty function into every other empty one and the snapshots land in the
 *     wrong places (13 of them instead of 12).
 *   - SYS_WRITE (0x05) is silently dropped by this QEMU (11.1.3) while
 *     SYS_WRITEC (0x03) and SYS_WRITE0 (0x04) work, so newlib's printf prints
 *     nothing at all. The report goes out through SYS_WRITE0, see out_str().
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/APRS/afsk.h"
#include "protocols/APRS/ax25.h"
#include "protocols/APRS/hdlc.h"
#include "protocols/KISS/afsk9600.h"
#include "protocols/KISS/kiss.h"
#include "protocols/KISS/packet_modem.h"

#define FS       48000u
#define BLOCK    480u          /* 10 ms */
#define MAXLEN   (FS * 3u)     /* three seconds of audio is plenty */

volatile uint32_t sink;

static int16_t audio[MAXLEN];
static uint8_t frame[HDLC_MAX_LEN];
static uint8_t kissBuf[2u * KISS_MAX_PAYLOAD + 8u];
static pkt_engine_t engine;

__attribute__((noinline, used))
void frame_marker(void)
{
    __asm__ volatile("" ::: "memory");
}

/* ------------------------------------------------------------ console --- */

/* SYS_WRITE0: r1 = pointer to a NUL-terminated string, no handle. */
static void out_str(const char *s)
{
    __asm__ volatile("mov r0, #4\n\tmov r1, %0\n\tbkpt 0xab"
                     :: "r"(s) : "r0", "r1", "memory");
}

static void out_u32(uint32_t v)
{
    char b[11];
    int i = 10;
    b[i] = '\0';
    do
    {
        b[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    out_str(&b[i]);
}

/* ------------------------------------------------------------ the run --- */

typedef struct
{
    const char *name;
    uint32_t frameLen, audioLen, blocks, kissIn, kissOut, kissDec, rxFrames,
             rxFcsErr;
    bool ran;
} result_t;

static result_t results[2];
static size_t nresults;

static size_t buildFrame(void)
{
    static uint8_t info[256];
    for (size_t i = 0; i < sizeof(info); i++)
        info[i] = (uint8_t)('A' + (i % 26));

    ax25_frame_t f;
    memset(&f, 0, sizeof(f));
    ax25_addr_set(&f.dst, "APRS");
    ax25_addr_set(&f.src, "N0CALL-1");
    f.nDigis  = 0;
    f.control = 0x03;
    f.pid     = 0xF0;
    f.info    = info;
    f.infoLen = sizeof(info);

    return ax25_build_ui(frame, sizeof(frame), &f);
}

static void onFrame(const uint8_t *payload, size_t len, void *user)
{
    sink += (uint32_t)(len + (payload[0] & 0));
    (void)user;
}

static void runModem(pkt_modem_t mode, const char *name, size_t frameLen)
{
    pkt_engine_t *e = &engine;
    result_t *r = &results[nresults++];

    r->name = name;
    r->ran  = false;

    /* arm: frame build, flags, TXDELAY */
    if (!pkt_engine_init(e, mode, FS))
        return;
    if (!pkt_tx_begin(e, frame, frameLen - 2u, 300, 20))
        return;
    frame_marker();                 /* armed */

    /* gen: modulate, one 10 ms block at a time */
    size_t n = 0;
    while (n + BLOCK <= MAXLEN)
    {
        size_t w = pkt_tx_block(e, audio + n, BLOCK);
        if (w == 0)
            break;
        n += w;
        if (pkt_tx_busy(e) == false)
            break;
    }
    size_t audioLen = n;
    frame_marker();                 /* burst complete */

    /* re-arm as receiver, then demodulate the audio just made */
    pkt_engine_init(e, mode, FS);
    pkt_rx_begin(e, onFrame, NULL);
    frame_marker();                 /* receiving */

    for (size_t i = 0; i + BLOCK <= audioLen; i += BLOCK)
        pkt_rx_block(e, audio + i, BLOCK);
    frame_marker();                 /* audio consumed */

    /* KISS: decode and re-encode the frame the host would send */
    kiss_dec_t dec;
    kiss_dec_init(&dec, false);
    size_t kn = kiss_enc_data(kissBuf, sizeof(kissBuf), 0, frame, frameLen - 2u,
                              false);
    size_t got = 0;
    for (size_t i = 0; i < kn; i++)
        got += (size_t)kiss_dec_byte(&dec, kissBuf[i]);
    size_t on = kiss_enc_data(kissBuf, sizeof(kissBuf), 0, frame, frameLen - 2u,
                              false);
    sink += (uint32_t)(on + got);
    frame_marker();                 /* KISS done */

    r->frameLen = (uint32_t)frameLen;
    r->audioLen = (uint32_t)audioLen;
    r->blocks   = (uint32_t)(audioLen / BLOCK);
    r->kissIn   = (uint32_t)kn;
    r->kissOut  = (uint32_t)on;
    r->kissDec  = (uint32_t)got;
    r->rxFrames = e->rxFrames;
    r->rxFcsErr = e->rxFcsErr;
    r->ran      = true;
}

int main(void)
{
    size_t frameLen = buildFrame();

    frame_marker();                 /* 0: start-up and frame build done */
    runModem(PKT_MODEM_AFSK1200, "AFSK1200", frameLen);
    runModem(PKT_MODEM_AFSK9600, "AFSK9600", frameLen);
    frame_marker();                 /* 11: last */

    /* Console, after the last marker: it must not be charged to a stage. */
    for (size_t i = 0; i < nresults; i++)
    {
        result_t *r = &results[i];
        if (!r->ran)
        {
            out_str("kiss_cost ");
            out_str(r->name);
            out_str(": not runnable at this rate\n");
            continue;
        }
        out_str("kiss_cost ");
        out_str(r->name);
        out_str(": frame ");
        out_u32(r->frameLen);
        out_str(" bytes, audio ");
        out_u32(r->audioLen);
        out_str(" samples, blocks ");
        out_u32(r->blocks);
        out_str(", kiss ");
        out_u32(r->kissIn);
        out_str(" bytes, kiss_dec ");
        out_u32(r->kissDec);
        out_str(", rx_frames ");
        out_u32(r->rxFrames);
        out_str(", rx_fcserr ");
        out_u32(r->rxFcsErr);
        out_str("\n");
    }

    /*
     * Exit the emulator: the plugin dumps its profile from an atexit hook.
     * A WFI loop would leave the guest running and the profile would only
     * appear when the script's timeout killed QEMU.
     */
    exit(0);
}
