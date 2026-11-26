/*
 * tiny-AES-c (Unlicense, UNLICENSE.tinyaes) a second time, for AES-128: the
 * same source with the 128-bit key size and its own names, beside the
 * AES-256 one (tinyaes.c). Only the OFB helper below is used outside.
 */
#define AES256 0
#define AES128 1
#define tinyaes_init_ctx tinyaes128_init_ctx
#define tinyaes_init_ctx_iv tinyaes128_init_ctx_iv
#define tinyaes_ctx_set_iv tinyaes128_ctx_set_iv
#define tinyaes_ECB_encrypt tinyaes128_ECB_encrypt
#define tinyaes_ECB_decrypt tinyaes128_ECB_decrypt
#define tinyaes_CBC_encrypt_buffer tinyaes128_CBC_encrypt_buffer
#define tinyaes_CBC_decrypt_buffer tinyaes128_CBC_decrypt_buffer
#define tinyaes_CTR_xcrypt_buffer tinyaes128_CTR_xcrypt_buffer
#include "tinyaes.c"

/* n bytes of AES-128 OFB keystream, the first `discard` blocks dropped */
void tinyaes128_ofb(const uint8_t key[16], const uint8_t iv[16],
                    unsigned discard, uint8_t *out, unsigned n);
void tinyaes128_ofb(const uint8_t key[16], const uint8_t iv[16],
                    unsigned discard, uint8_t *out, unsigned n)
{
    struct AES_ctx ctx;
    uint8_t blk[16];
    tinyaes128_init_ctx(&ctx, key);
    memcpy(blk, iv, 16);
    unsigned at = 0;
    for (unsigned b = 0; at < n; b++) {
        tinyaes128_ECB_encrypt(&ctx, blk);
        if (b < discard)
            continue;
        for (unsigned i = 0; i < 16 && at < n; i++)
            out[at++] = blk[i];
    }
    volatile uint8_t *v = (volatile uint8_t *)&ctx;
    for (size_t i = 0; i < sizeof(ctx); i++)
        v[i] = 0;
    v = (volatile uint8_t *)blk;
    for (size_t i = 0; i < sizeof(blk); i++)
        v[i] = 0;
}
