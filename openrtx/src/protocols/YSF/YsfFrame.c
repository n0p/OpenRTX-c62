/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file YsfFrame.c
 * @brief System Fusion frame layer, see YsfFrame.h.
 */

#include "protocols/YSF/YsfFrame.h"

#ifdef CONFIG_YSF

#include <string.h>

/*
 * The Golay(24,12) decoder is the one lib/dmr_vocoder already carries for the
 * AMBE+2 channel frame: the FICH uses the same extended Golay code, so sharing
 * it saves 4 KiB of table in flash.
 */
#include "dmr_decoder.h"

/** Bits in one frame. */
#define FRAME_BITS 960
/** Bits of the frame sync. */
#define SYNC_BITS 40
/** Bits of the FICH, as transmitted. */
#define FICH_BITS 200
/** Bits of the payload. */
#define PAYLOAD_BITS 720
/** Bits of the FICH after the convolutional decoder. */
#define FICH_INFO_BITS 48

#define PN95_SEED 0x1C9

static struct ysfDecoded out;
static struct ysfCallInfo call;
static struct ysfText text;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Expand dibits to bits, MSB first, the order they go on the air in. */
static void dibits_to_bits(const uint8_t *dibits, int count, uint8_t *bits)
{
    for (int i = 0; i < count; i++)
    {
        bits[2 * i] = (uint8_t)((dibits[i] >> 1) & 1);
        bits[2 * i + 1] = (uint8_t)(dibits[i] & 1);
    }
}

/**
 * Undo the block interleaver: it was written row by row into a
 * rows x cols matrix and read column by column.
 */
static void deinterleave(const uint8_t *in, int rows, int cols, uint8_t *outb)
{
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            outb[j + i * cols] = in[i + j * rows];
}

/**
 * CRC-16 as System Fusion defines it: polynomial 0x1021, registers start at
 * zero, the result is inverted.  Checking a message that carries its own CRC
 * therefore gives zero.
 */
static uint16_t crc16(const uint8_t *bits, int len)
{
    uint32_t crc = 0;

    for (int i = 0; i < len; i++)
    {
        crc = ((crc << 1) | (bits[i] & 1u)) & 0x1FFFFu;
        if (crc & 0x10000u)
            crc = (crc & 0xFFFFu) ^ 0x1021u;
    }

    return (uint16_t)((crc ^ 0xFFFFu) & 0xFFFFu);
}

/** PN(9,5) whitening: 9-bit LFSR, S0 ^ S4 feedback, output S0. */
static uint8_t pn95_bit(uint16_t *reg)
{
    uint8_t bit = (uint8_t)(*reg & 1u);
    uint8_t feedback = (uint8_t)(((*reg >> 4) ^ *reg) & 1u);

    *reg = (uint16_t)((*reg >> 1) | ((uint16_t)feedback << 8));

    return bit;
}

/** De-whiten n bits in place (the sequence is its own inverse). */
static void dewhiten(uint8_t *bits, int n)
{
    uint16_t reg = PN95_SEED;

    for (int i = 0; i < n; i++)
        bits[i] ^= pn95_bit(&reg);
}

/* ------------------------------------------------------------------ */
/* Convolutional code: rate 1/2, K = 5, generators 0o31 / 0o27         */
/* ------------------------------------------------------------------ */

/*
 * The register is shifted as reg = (reg << 1) | bit and the two output bits
 * are the parities of reg & 0x19 and reg & 0x17, which is the form every
 * working System Fusion implementation uses (op25, MMDVM, dsd-fme).  Encoding
 * is terminated with four zero bits, so the trellis starts and ends at state
 * zero.
 */

/** Parity of a 5-bit window, as a 32-entry table. */
static const uint8_t parity32[32] = {
    0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

#define TRELLIS_STATES 16
#define TRELLIS_MAX_STEPS 200

static uint8_t trellisHist[TRELLIS_MAX_STEPS][TRELLIS_STATES];

/**
 * Hard-decision Viterbi.  Returns the number of differing bits along the
 * winning path (0 on a clean frame) and writes steps information bits.
 */
static int viterbi(const uint8_t *bits, int steps, uint8_t *decoded)
{
    uint16_t metric[TRELLIS_STATES];
    uint16_t next[TRELLIS_STATES];
    const uint16_t inf = 0x3FFF;

    for (int s = 0; s < TRELLIS_STATES; s++)
        metric[s] = inf;
    metric[0] = 0;

    for (int t = 0; t < steps; t++)
    {
        uint8_t r0 = bits[2 * t];
        uint8_t r1 = bits[2 * t + 1];

        for (int s = 0; s < TRELLIS_STATES; s++)
            next[s] = inf;

        for (int s = 0; s < TRELLIS_STATES; s++)
        {
            if (metric[s] >= inf)
                continue;

            for (int b = 0; b < 2; b++)
            {
                uint8_t reg = (uint8_t)(((s << 1) | b) & 0x1F);
                uint8_t o0 = parity32[reg & 0x19];
                uint8_t o1 = parity32[reg & 0x17];
                uint8_t ns = (uint8_t)(reg & 0x0F);
                uint16_t m = (uint16_t)(metric[s] + (o0 != r0) + (o1 != r1));

                if (m < next[ns])
                {
                    next[ns] = m;
                    trellisHist[t][ns] = (uint8_t)((s << 1) | b);
                }
            }
        }

        memcpy(metric, next, sizeof(metric));
    }

    int best = 0;
    for (int s = 1; s < TRELLIS_STATES; s++)
        if (metric[s] < metric[best])
            best = s;

    int state = best;
    for (int t = steps - 1; t >= 0; t--)
    {
        uint8_t v = trellisHist[t][state];
        decoded[t] = (uint8_t)(v & 1u);
        state = v >> 1;
    }

    return metric[best];
}

/* ------------------------------------------------------------------ */
/* FICH                                                               */
/* ------------------------------------------------------------------ */

/** Bit field of the FICH information, most significant first. */
struct ysfFich
{
    uint8_t fi, cs, cm, bn, bt, fn, ft, mr, vp, dt, st, sc;
};

static uint32_t field(const uint8_t *bits, int offset, int length)
{
    uint32_t v = 0;

    for (int i = 0; i < length; i++)
        v = (v << 1) | (bits[offset + i] & 1u);

    return v;
}

#define FICH_MAX_METRIC 24

static bool fich_decode(const uint8_t *dibits, struct ysfFich *fich)
{
    uint8_t inter[100];
    uint8_t bits[FICH_BITS];
    uint8_t decoded[100];
    uint8_t info[FICH_INFO_BITS];
    int errors = 0;

    /* 100 dibits, interleaved as 20 rows of 5 */
    deinterleave(dibits, 20, 5, inter);
    dibits_to_bits(inter, 100, bits);

    if (viterbi(bits, 100, decoded) > FICH_MAX_METRIC)
        return false;

    for (int g = 0; g < 4; g++)
    {
        uint32_t w = 0;
        uint32_t data = 0;

        for (int i = 0; i < 24; i++)
            w = (w << 1) | (decoded[g * 24 + i] & 1u);

        int n = dmr_golay24_correct(w, &data);
        if (n < 0)
            return false;

        errors += n;
        for (int i = 0; i < 12; i++)
            info[g * 12 + i] = (uint8_t)((data >> (11 - i)) & 1u);
    }

    if (crc16(info, FICH_INFO_BITS) != 0)
        return false;

    fich->fi = (uint8_t)field(info, 0, 2);
    fich->cs = (uint8_t)field(info, 2, 2);
    fich->cm = (uint8_t)field(info, 4, 2);
    fich->bn = (uint8_t)field(info, 6, 2);
    fich->bt = (uint8_t)field(info, 8, 2);
    fich->fn = (uint8_t)field(info, 10, 3);
    fich->ft = (uint8_t)field(info, 13, 3);
    fich->mr = (uint8_t)field(info, 18, 3);
    fich->vp = (uint8_t)field(info, 21, 1);
    fich->dt = (uint8_t)field(info, 22, 2);
    /* The squelch flag is inverted: clear means "a DG-ID follows" */
    fich->st = (uint8_t)field(info, 24, 1);
    fich->sc = (uint8_t)field(info, 25, 7);

    (void)errors;

    return true;
}

/* ------------------------------------------------------------------ */
/* Data channel                                                       */
/* ------------------------------------------------------------------ */

/**
 * Copy a callsign field out of the decoded bytes, dropping what is not
 * printable and the trailing padding.
 */
static void copy_field(char *dst, const uint8_t *src, int len)
{
    int n = (len > YSF_CALLSIGN_LEN) ? YSF_CALLSIGN_LEN : len;

    for (int i = 0; i < n; i++)
    {
        uint8_t c = src[i];

        dst[i] = ((c >= 0x20) && (c < 0x7F)) ? (char)c : ' ';
    }

    /* Trim the right, System Fusion pads with spaces */
    int last = n - 1;
    while ((last >= 0) && (dst[last] == ' '))
        last--;
    dst[last + 1] = '\0';
}

/**
 * Pack information bits into bytes.  The data channel carries characters, and
 * the Viterbi output is one bit per byte.
 */
static void bits_to_bytes(const uint8_t *bits, int nbits, uint8_t *out)
{
    memset(out, 0, (size_t)((nbits + 7) / 8));

    for (int i = 0; i < nbits; i++)
        out[i >> 3] = (uint8_t)((out[i >> 3] << 1) | (bits[i] & 1u));
}

/**
 * Decode one data channel block: de-whiten, de-interleave, Viterbi, CRC.  The
 * whitening covers the information bits before the CRC, so the CRC settles it
 * and only then are the bits de-whitened for their content.
 *
 * @param dibits: 100 dibits (200-bit block) or 180 dibits (360-bit block)
 * @param blockBits: 200 or 360
 * @return the number of information bits, or 0 if the block is bad
 */
static int dch_decode(const uint8_t *dibits, int blockSteps, int rows, int cols,
                      uint8_t *info)
{
    uint8_t inter[400];
    uint8_t bits[400];
    uint8_t decoded[TRELLIS_MAX_STEPS];
    int nbits = blockSteps * 2;

    if ((nbits > (int)sizeof(inter)) || (rows * cols != nbits / 2))
        return 0;

    deinterleave(dibits, rows, cols, inter);
    dibits_to_bits(inter, blockSteps, bits);

    viterbi(bits, blockSteps, decoded);

    int dataBits = blockSteps - 4 - 16; /* tail, then CRC */

    if (dataBits <= 0)
        return 0;
    if (crc16(decoded, (int)(blockSteps - 4)) != 0)
        return 0;

    memcpy(info, decoded, (size_t)dataBits);
    dewhiten(info, dataBits);

    return dataBits;
}

/* ------------------------------------------------------------------ */
/* Text messages                                                      */
/* ------------------------------------------------------------------ */

static void text_reset(void)
{
    memset(&text, 0, sizeof(text));
    text.expected = 0xFF;
}

/**
 * Add one block of message text.  A message is carried in the data channel
 * with bn = 2, one field per frame: fn is its index, ft the index of the last
 * one.  Fields are 20 characters wide in V/D mode 1 and 10 in V/D mode 2.
 */
static void text_block(const uint8_t *info, int length)
{
    /* A new message starts whenever the sequence restarts or the last one
     * was already delivered */
    if ((call.fn == 0) || text.complete)
    {
        memset(text.text, 0, sizeof(text.text));
        text.length = 0;
        text.received = 0;
        text.complete = false;
        text.expected = call.ft;
    }

    for (int i = 0; i < length; i++)
    {
        uint8_t c = info[i];

        if (text.length >= YSF_TEXT_MAX)
            break;
        text.text[text.length++] = ((c >= 0x20) && (c < 0x7F)) ? (char)c : ' ';
    }

    text.received++;

    if (call.fn == call.ft)
    {
        while ((text.length > 0) && (text.text[text.length - 1] == ' '))
            text.length--;
        text.text[text.length] = '\0';

        if (text.length > 0)
        {
            text.complete = true;
            text.messages++;
            out.textChanged = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Voice                                                              */
/* ------------------------------------------------------------------ */

/** 72-bit AMBE+2 channel frame: the bits go to the vocoder as they are. */
static void voice72(const uint8_t *dibits, struct ysfVoiceSlot *slot)
{
    uint8_t bits[YSF_VOICE_MAX_BITS];

    dibits_to_bits(dibits, YSF_VOICE_MAX_BITS / 2, bits);
    slot->nbits = YSF_VOICE_MAX_BITS;
    memcpy(slot->bits, bits, YSF_VOICE_MAX_BITS);
}

/**
 * V/D mode 2 voice: 52 dibits, interleaved 26 x 4, whitened, then 27 bits sent
 * three times each and 22 bits sent once, plus one fixed bit that is checked.
 *
 * @return false if the fixed bit is set, which means the slot is corrupt
 */
static bool voice49(const uint8_t *dibits, struct ysfVoiceSlot *slot)
{
    uint8_t bits[104];
    uint8_t ordered[104];

    dibits_to_bits(dibits, 52, bits);

    /* air index t holds data index (t / 4) + (t % 4) * 26 */
    for (int t = 0; t < 104; t++)
        ordered[(t / 4) + (t % 4) * 26] = bits[t];

    dewhiten(ordered, 104);

    if (ordered[103] != 0)
        return false;

    slot->nbits = 49;

    for (int j = 0; j < 27; j++)
    {
        uint8_t votes = (uint8_t)((ordered[3 * j] << 2) |
                                  (ordered[3 * j + 1] << 1) |
                                  ordered[3 * j + 2]);
        /* majority of three: 000->0, 001->0, 010->0, 011->1,
         * 100->0, 101->1, 110->1, 111->1 */
        static const uint8_t majority[8] = {0, 0, 0, 1, 0, 1, 1, 1};

        slot->bits[j] = majority[votes & 7];
    }

    for (int j = 0; j < 22; j++)
        slot->bits[27 + j] = ordered[81 + j];

    return true;
}

/* ------------------------------------------------------------------ */
/* Frame                                                              */
/* ------------------------------------------------------------------ */

void ysfFrame_reset(void)
{
    memset(&out, 0, sizeof(out));
    memset(&call, 0, sizeof(call));
    text_reset();
}

const struct ysfCallInfo *ysfFrame_call(void)
{
    return &call;
}

const struct ysfText *ysfFrame_text(void)
{
    return &text;
}

const struct ysfDecoded *ysfFrame_decode(const struct ysfFrame *frame)
{
    struct ysfFich fich;

    out.voiceCount = 0;
    out.textChanged = false;
    out.fichOk = false;

    if (frame == NULL)
        return NULL;

    if (!fich_decode(&frame->dibits[YSF_SYNC_SYMBOLS], &fich))
    {
        call.badFich++;
        /*
         * A frame whose FICH did not verify still carries voice, and the
         * previous frame's layout is the best guess for it, but the frame
         * does not update what the transmission says.
         */
        return &out;
    }

    out.fichOk = true;
    call.frames++;
    call.fi = fich.fi;
    call.cm = fich.cm;
    call.bn = fich.bn;
    call.bt = fich.bt;
    call.fn = fich.fn;
    call.ft = fich.ft;
    call.dt = fich.dt;
    call.dgidValid = (fich.st == 0) && (fich.sc <= 99);
    call.dgid = fich.sc;

    /* A header opens the transmission: start from a clean state */
    if (fich.fi == 0)
    {
        memset(call.dest, 0, sizeof(call.dest));
        memset(call.source, 0, sizeof(call.source));
        memset(call.uplink, 0, sizeof(call.uplink));
        memset(call.downlink, 0, sizeof(call.downlink));
        text_reset();
    }

    const uint8_t *payload = &frame->dibits[YSF_SYNC_SYMBOLS + 100];

    if ((fich.fi == 0) || (fich.fi == 2) || (fich.dt == 1))
    {
        /*
         * Header, terminator and full-rate data: ten 36-dibit blocks, which
         * are two 180-dibit data channel blocks taken alternately.
         */
        uint8_t block[180];
        uint8_t info[192];

        for (int b = 0; b < 2; b++)
        {
            for (int i = 0; i < 5; i++)
                memcpy(&block[i * 36], &payload[(i * 2 + b) * 36], 36);

            int n = dch_decode(block, 180, 20, 9, info);
            if (n == 160)
            {
                uint8_t bytes[20];

                call.dataBlocks++;
                bits_to_bytes(info, 160, bytes);

                if (b == 0)
                {
                    copy_field(call.dest, bytes, 10);
                    copy_field(call.source, &bytes[10], 10);
                }
                else
                {
                    copy_field(call.downlink, bytes, 10);
                    copy_field(call.uplink, &bytes[10], 10);
                }
            }
        }

        return &out;
    }

    if (fich.dt == 3)
    {
        /* Voice FR: full-rate voice, 144 bits per slot, not AMBE+2 2450 */
        call.unsupported += YSF_VOICE_SLOTS;
        return &out;
    }

    if (fich.dt == 0)
    {
        /* V/D mode 1: five pairs of 36-dibit CSD and voice */
        uint8_t csd[180];
        uint8_t info[192];

        for (int i = 0; i < YSF_VOICE_SLOTS; i++)
        {
            memcpy(&csd[i * 36], &payload[i * 72], 36);
            voice72(&payload[i * 72 + 36], &out.voice[i]);
        }

        out.voiceCount = YSF_VOICE_SLOTS;
        call.voices += YSF_VOICE_SLOTS;

        int n = dch_decode(csd, 180, 20, 9, info);
        if (n == 160)
        {
            uint8_t bytes[20];

            call.dataBlocks++;
            bits_to_bytes(info, 160, bytes);

            if (fich.bn == 2)
            {
                text_block(bytes, 20);
            }
            else if (fich.bn == 0)
            {
                copy_field(call.dest, bytes, 10);
                copy_field(call.source, &bytes[10], 10);
            }
            else if (fich.bn == 1)
            {
                copy_field(call.uplink, bytes, 10);
                copy_field(call.downlink, &bytes[10], 10);
            }
        }

        return &out;
    }

    if (fich.dt == 2)
    {
        /* V/D mode 2: five pairs of 20-dibit DCH and 52-dibit voice */
        uint8_t dch[100];
        uint8_t info[96];

        for (int i = 0; i < YSF_VOICE_SLOTS; i++)
        {
            memcpy(&dch[i * 20], &payload[i * 72], 20);

            if (voice49(&payload[i * 72 + 20], &out.voice[out.voiceCount]))
            {
                out.voiceCount++;
                call.voices++;
            }
        }

        int n = dch_decode(dch, 100, 20, 5, info);
        if (n == 80)
        {
            uint8_t bytes[10];

            call.dataBlocks++;
            bits_to_bytes(info, 80, bytes);

            if (fich.bn == 2)
            {
                text_block(bytes, 10);
            }
            else
            {
                /* The data channel cycles its fields with the frame number */
                switch (fich.fn)
                {
                    case 0: copy_field(call.dest, bytes, 10); break;
                    case 1: copy_field(call.source, bytes, 10); break;
                    case 2: copy_field(call.uplink, bytes, 10); break;
                    case 3: copy_field(call.downlink, bytes, 10); break;
                    default: break;
                }
            }
        }

        return &out;
    }

    return &out;
}

#else /* CONFIG_YSF */

void ysfFrame_reset(void) {}
const struct ysfCallInfo *ysfFrame_call(void) { return NULL; }
const struct ysfText *ysfFrame_text(void) { return NULL; }
const struct ysfDecoded *ysfFrame_decode(const struct ysfFrame *frame)
{
    (void)frame;

    return NULL;
}

#endif /* CONFIG_YSF */
