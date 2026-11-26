// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * des.c         Crypthings
 * DES Alg
 *
 * From dsd-neo (crypt-des.c, ISC licence): the tables, permute(),
 * rotate_sub_keys(), des_prepare_round_keys() and des_cipher() as they are;
 * the C62 adds c62Des_ofb() (des.h). Encryption only.
 *-----------------------------------------------------------------------------*/

#include <stdint.h>
#include <string.h>
#include "des.h"

#define DSD_MEMSET memset
#define DSD_MEMCPY memcpy

static uint8_t SLUT1[64] = {0xE0, 0x00, 0x40, 0xF0, 0xD0, 0x70, 0x10, 0x40, 0x20, 0xE0, 0xF0, 0x20, 0xB0,
                            0xD0, 0x80, 0x10, 0x30, 0xA0, 0xA0, 0x60, 0x60, 0xC0, 0xC0, 0xB0, 0x50, 0x90,
                            0x90, 0x50, 0x00, 0x30, 0x70, 0x80, 0x40, 0xF0, 0x10, 0xC0, 0xE0, 0x80, 0x80,
                            0x20, 0xD0, 0x40, 0x60, 0x90, 0x20, 0x10, 0xB0, 0x70, 0xF0, 0x50, 0xC0, 0xB0,
                            0x90, 0x30, 0x70, 0xE0, 0x30, 0xA0, 0xA0, 0x00, 0x50, 0x60, 0x00, 0xD0};

static uint8_t SLUT2[64] = {0x0F, 0x03, 0x01, 0x0D, 0x08, 0x04, 0x0E, 0x07, 0x06, 0x0F, 0x0B, 0x02, 0x03,
                            0x08, 0x04, 0x0E, 0x09, 0x0C, 0x07, 0x00, 0x02, 0x01, 0x0D, 0x0A, 0x0C, 0x06,
                            0x00, 0x09, 0x05, 0x0B, 0x0A, 0x05, 0x00, 0x0D, 0x0E, 0x08, 0x07, 0x0A, 0x0B,
                            0x01, 0x0A, 0x03, 0x04, 0x0F, 0x0D, 0x04, 0x01, 0x02, 0x05, 0x0B, 0x08, 0x06,
                            0x0C, 0x07, 0x06, 0x0C, 0x09, 0x00, 0x03, 0x05, 0x02, 0x0E, 0x0F, 0x09};

static uint8_t SLUT3[64] = {0xA0, 0xD0, 0x00, 0x70, 0x90, 0x00, 0xE0, 0x90, 0x60, 0x30, 0x30, 0x40, 0xF0,
                            0x60, 0x50, 0xA0, 0x10, 0x20, 0xD0, 0x80, 0xC0, 0x50, 0x70, 0xE0, 0xB0, 0xC0,
                            0x40, 0xB0, 0x20, 0xF0, 0x80, 0x10, 0xD0, 0x10, 0x60, 0xA0, 0x40, 0xD0, 0x90,
                            0x00, 0x80, 0x60, 0xF0, 0x90, 0x30, 0x80, 0x00, 0x70, 0xB0, 0x40, 0x10, 0xF0,
                            0x20, 0xE0, 0xC0, 0x30, 0x50, 0xB0, 0xA0, 0x50, 0xE0, 0x20, 0x70, 0xC0};

static uint8_t SLUT4[64] = {0x07, 0x0D, 0x0D, 0x08, 0x0E, 0x0B, 0x03, 0x05, 0x00, 0x06, 0x06, 0x0F, 0x09,
                            0x00, 0x0A, 0x03, 0x01, 0x04, 0x02, 0x07, 0x08, 0x02, 0x05, 0x0C, 0x0B, 0x01,
                            0x0C, 0x0A, 0x04, 0x0E, 0x0F, 0x09, 0x0A, 0x03, 0x06, 0x0F, 0x09, 0x00, 0x00,
                            0x06, 0x0C, 0x0A, 0x0B, 0x01, 0x07, 0x0D, 0x0D, 0x08, 0x0F, 0x09, 0x01, 0x04,
                            0x03, 0x05, 0x0E, 0x0B, 0x05, 0x0C, 0x02, 0x07, 0x08, 0x02, 0x04, 0x0E};

static uint8_t SLUT5[64] = {0x20, 0xE0, 0xC0, 0xB0, 0x40, 0x20, 0x10, 0xC0, 0x70, 0x40, 0xA0, 0x70, 0xB0,
                            0xD0, 0x60, 0x10, 0x80, 0x50, 0x50, 0x00, 0x30, 0xF0, 0xF0, 0xA0, 0xD0, 0x30,
                            0x00, 0x90, 0xE0, 0x80, 0x90, 0x60, 0x40, 0xB0, 0x20, 0x80, 0x10, 0xC0, 0xB0,
                            0x70, 0xA0, 0x10, 0xD0, 0xE0, 0x70, 0x20, 0x80, 0xD0, 0xF0, 0x60, 0x90, 0xF0,
                            0xC0, 0x00, 0x50, 0x90, 0x60, 0xA0, 0x30, 0x40, 0x00, 0x50, 0xE0, 0x30};

static uint8_t SLUT6[64] = {0x0C, 0x0A, 0x01, 0x0F, 0x0A, 0x04, 0x0F, 0x02, 0x09, 0x07, 0x02, 0x0C, 0x06,
                            0x09, 0x08, 0x05, 0x00, 0x06, 0x0D, 0x01, 0x03, 0x0D, 0x04, 0x0E, 0x0E, 0x00,
                            0x07, 0x0B, 0x05, 0x03, 0x0B, 0x08, 0x09, 0x04, 0x0E, 0x03, 0x0F, 0x02, 0x05,
                            0x0C, 0x02, 0x09, 0x08, 0x05, 0x0C, 0x0F, 0x03, 0x0A, 0x07, 0x0B, 0x00, 0x0E,
                            0x04, 0x01, 0x0A, 0x07, 0x01, 0x06, 0x0D, 0x00, 0x0B, 0x08, 0x06, 0x0D};

static uint8_t SLUT7[64] = {0x40, 0xD0, 0xB0, 0x00, 0x20, 0xB0, 0xE0, 0x70, 0xF0, 0x40, 0x00, 0x90, 0x80,
                            0x10, 0xD0, 0xA0, 0x30, 0xE0, 0xC0, 0x30, 0x90, 0x50, 0x70, 0xC0, 0x50, 0x20,
                            0xA0, 0xF0, 0x60, 0x80, 0x10, 0x60, 0x10, 0x60, 0x40, 0xB0, 0xB0, 0xD0, 0xD0,
                            0x80, 0xC0, 0x10, 0x30, 0x40, 0x70, 0xA0, 0xE0, 0x70, 0xA0, 0x90, 0xF0, 0x50,
                            0x60, 0x00, 0x80, 0xF0, 0x00, 0xE0, 0x50, 0x20, 0x90, 0x30, 0x20, 0xC0};

static uint8_t SLUT8[64] = {0x0D, 0x01, 0x02, 0x0F, 0x08, 0x0D, 0x04, 0x08, 0x06, 0x0A, 0x0F, 0x03, 0x0B,
                            0x07, 0x01, 0x04, 0x0A, 0x0C, 0x09, 0x05, 0x03, 0x06, 0x0E, 0x0B, 0x05, 0x00,
                            0x00, 0x0E, 0x0C, 0x09, 0x07, 0x02, 0x07, 0x02, 0x0B, 0x01, 0x04, 0x0E, 0x01,
                            0x07, 0x09, 0x04, 0x0C, 0x0A, 0x0E, 0x08, 0x02, 0x0D, 0x00, 0x0F, 0x06, 0x0C,
                            0x0A, 0x09, 0x0D, 0x00, 0x0F, 0x03, 0x03, 0x05, 0x05, 0x06, 0x08, 0x0B};

//initial permutation IP (on input)
static uint8_t initial_register_permutation[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                                   62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                                   57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                                   61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};

//D-Box Expansion (sometimes noted as E BIT-SELECTION TABLE)
static uint8_t message_expansion[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                                        12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                        22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};

// (sometimes noted as simply 'P')
static uint8_t right_half_permutation[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                                             2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};

//inverse initial PI, a.k.a. final permutation (on output)
static uint8_t final_register_permutation[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                                 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                                 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                                 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};

//initial key permutation
static uint8_t pc1_key_permutation[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                                          35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                                          46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};

//sub c and d key permutation
static uint8_t pc2_key_permutation[48] = {
    //c bits
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    //d bits (with -28 offset to simply/only have one permute function)
    13, 24, 3, 9, 19, 27, 2, 12, 23, 17, 5, 20, 16, 21, 11, 28, 6, 25, 18, 14, 22, 8, 1, 4};

static uint8_t key_shift_sizes[17] = {1, 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

static uint8_t key_shift_bytes[17] = {0x80, 0x80, 0x80, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
                                      0x80, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x80};

static uint8_t key_shift_x[4] = {8, 8, 8, 4};

static uint8_t key_shift_y[4] = {1, 2, 3, 0};

//input is byte-wise array, output is byte-wise array
static void
permute(const uint8_t* input, uint8_t* output, const uint8_t* table, uint8_t start, uint8_t end) {

    for (uint8_t i = start; i < end; i++) {
        uint8_t tdiv = (table[i] - 1) / 8;
        uint8_t tmod = (table[i] - 1) % 8;
        uint8_t idiv = i / 8;
        uint8_t imod = i % 8;

        //need a way to simplify or get rid of all these shifts
        uint8_t bit = (((input[tdiv] & (0x80 >> tmod)) << tmod) >> imod);
        output[idiv] |= bit;
    }
}

static void
rotate_sub_keys(uint8_t* Kc, uint8_t* Kd, uint8_t shift_size, uint8_t shift_byte) {

    uint8_t sb1[4];
    DSD_MEMSET(sb1, 0, sizeof(sb1));
    uint8_t sb2[4];
    DSD_MEMSET(sb2, 0, sizeof(sb2));

    for (uint8_t i = 0; i < 4; i++) {
        sb1[i] = shift_byte & Kc[i];
        sb2[i] = shift_byte & Kd[i];
    }

    for (uint8_t i = 0; i < 4; i++) {

        uint8_t x = key_shift_x[i];
        uint8_t y = key_shift_y[i];

        Kc[i] <<= shift_size;
        Kd[i] <<= shift_size;

        Kc[i] |= (sb1[y] >> (x - shift_size));
        Kd[i] |= (sb2[y] >> (x - shift_size));
    }
}

static void
des_prepare_round_keys(const uint8_t* main_key, uint8_t Ks[16][7]) {
    uint8_t Kc[4];
    DSD_MEMSET(Kc, 0, sizeof(Kc));
    uint8_t Kd[4];
    DSD_MEMSET(Kd, 0, sizeof(Kd));

    permute(main_key, Ks[0], pc1_key_permutation, 0, 56);
    for (uint8_t i = 0; i < 3; i++) {
        Kc[i] = Ks[0][i];
    }
    Kc[3] = Ks[0][3] & 0xF0;
    for (uint8_t i = 0; i < 3; i++) {
        Kd[i] = (Ks[0][i + 3] & 0x0F) << 4;
        Kd[i] |= (Ks[0][i + 4] & 0xF0) >> 4;
    }
    Kd[3] = (Ks[0][6] & 0x0F) << 4;

    DSD_MEMSET(Ks, 0, (size_t)16U * 7U * sizeof(uint8_t));
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t shift_size = key_shift_sizes[i + 1];
        uint8_t shift_byte = key_shift_bytes[i + 1];
        rotate_sub_keys(Kc, Kd, shift_size, shift_byte);
        permute(Kc, Ks[i], pc2_key_permutation, 0, 24);
        permute(Kd, Ks[i], pc2_key_permutation, 24, 48);
    }
}

static void
des_cipher(const uint8_t* main_key, const uint8_t* input_register, uint8_t* output_register, uint8_t de) {

    //starting and ending permutations
    uint8_t initial_permutation[8];
    DSD_MEMSET(initial_permutation, 0, sizeof(initial_permutation));
    uint8_t pre_end_permutation[8];
    DSD_MEMSET(pre_end_permutation, 0, sizeof(pre_end_permutation));

    //intermediate left, right, and expansion/selection arrays
    uint8_t left[4];
    DSD_MEMSET(left, 0, sizeof(left));
    uint8_t right[4];
    DSD_MEMSET(right, 0, sizeof(right));
    uint8_t rperm[4];
    DSD_MEMSET(rperm, 0, sizeof(rperm));
    uint8_t exp[6];
    DSD_MEMSET(exp, 0, sizeof(exp));
    uint8_t sel[4];
    DSD_MEMSET(sel, 0, sizeof(sel));

    //key sets
    uint8_t Ks[16][7];
    DSD_MEMSET(Ks, 0, sizeof(Ks));
    // codeql[cpp/weak-cryptographic-algorithm] DES/TDEA is required for active radio protocol interoperability.
    des_prepare_round_keys(main_key, Ks);

    //permute the input_register with the ip (initial_register_permutation) table
    permute(input_register, initial_permutation, initial_register_permutation, 0, 64);

    //copy the initial permutation to left and right half arrays
    DSD_MEMCPY(left, initial_permutation + 0, sizeof(left));
    DSD_MEMCPY(right, initial_permutation + 4, sizeof(right));

    //16 fiestel rounds
    for (uint8_t f = 0; f < 16; f++) {

        //reset rperm and exp arrays
        DSD_MEMSET(rperm, 0, sizeof(rperm));
        DSD_MEMSET(exp, 0, sizeof(exp));

        //expand right half from 32-bit to 48-bit
        permute(right, exp, message_expansion, 0, 48);

        //apply current iteration keyset to the expanded array (encryption cycles Ks forward, decryption goes in reverse)
        if (de == 1) {
            for (uint8_t i = 0; i < 6; i++) {
                exp[i] ^= Ks[f][i];
            }
        } else {
            for (uint8_t i = 0; i < 6; i++) {
                exp[i] ^= Ks[15 - f][i];
            }
        }

        //extract 6 bits for each sbox swap from the expanded array
        uint8_t e1 = exp[0] >> 2;
        uint8_t e2 = ((exp[0] & 0x3) << 4) | (exp[1] >> 4);
        uint8_t e3 = ((exp[1] & 0xF) << 2) | (exp[2] >> 6);
        uint8_t e4 = exp[2] & 0x3F;

        uint8_t e5 = exp[3] >> 2;
        uint8_t e6 = ((exp[3] & 0x3) << 4) | (exp[4] >> 4);
        uint8_t e7 = ((exp[4] & 0xF) << 2) | (exp[5] >> 6);
        uint8_t e8 = exp[5] & 0x3F;

        //select 32-bits of each 48-bit expansion via modified S Box Look Up Tables
        //swap the 6 bits for 4 bits via SLUT tables and OR them together into a selection array
        sel[0] = SLUT1[e1] | SLUT2[e2];
        sel[1] = SLUT3[e3] | SLUT4[e4];
        sel[2] = SLUT5[e5] | SLUT6[e6];
        sel[3] = SLUT7[e7] | SLUT8[e8];

        //permute the selected 32-bits into a new right half
        permute(sel, rperm, right_half_permutation, 0, 32);

        //xor new right half against left half
        for (uint8_t i = 0; i < 4; i++) {
            rperm[i] ^= left[i];
        }

        //swap old right half to left, set new right half to rperm
        DSD_MEMCPY(left, right, sizeof(left));
        DSD_MEMCPY(right, rperm, sizeof(right));

    } //end fiestel rounds f

    //combine right and left into the pre_end_permutation array
    DSD_MEMCPY(pre_end_permutation + 0, right, sizeof(right));
    DSD_MEMCPY(pre_end_permutation + 4, left, sizeof(left));

    //carry out the final permuation to get the completed output_register for this cipher iteration
    permute(pre_end_permutation, output_register, final_register_permutation, 0, 64);
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- != 0u)
        *v++ = 0u;
}

void c62Des_ofb(const uint8_t key[8], const uint8_t iv[8], unsigned discard,
                uint8_t *out, unsigned n)
{
    uint8_t in[8], o[8];
    memcpy(in, iv, 8);
    unsigned at = 0;
    for (unsigned b = 0; at < n; b++) {
        memset(o, 0, sizeof(o));
        des_cipher(key, in, o, 1);
        memcpy(in, o, 8);
        if (b < discard)
            continue;
        for (unsigned i = 0; i < 8 && at < n; i++)
            out[at++] = o[i];
    }
    wipe(in, sizeof(in));
    wipe(o, sizeof(o));
}
