#ifndef DMR_BP_AUTO_H
#define DMR_BP_AUTO_H

#include <stdint.h>
#include "dmr_ep.h"

#define DMR_BP_AUTO_HITS 3u
#define DMR_BP_AUTO_HAMMING 2u

static inline uint8_t dmr_bp_auto_feed(const char data[49],
                                       uint8_t votes[256])
{
#ifndef C62_DMR_BP_AUTO
    /*
     * Basic Privacy key recovery is OFF in this build: the macro is not
     * defined by the build system, so this returns no key and nothing is
     * deciphered by detection. The rest of the subsystem stays compiled
     * but inert (it only ever sees "no key found").
     */
    (void)data;
    (void)votes;
    return 0;
#else
    if ((data[48] & 1) != 1)
        return 0;

    uint64_t cipher = 0;
    for (unsigned i = 0; i < 48; ++i)
        cipher = (cipher << 1) | (uint64_t)(data[i] & 1);

    const uint64_t implied_ks = cipher ^ UINT64_C(0xF801A99F8CE0);
    for (unsigned k = 1; k <= 255; ++k) {
        const uint64_t delta = implied_ks ^ dmrBp_keystream((uint8_t)k);
        if ((unsigned)__builtin_popcountll(delta) > DMR_BP_AUTO_HAMMING)
            continue;
        if (votes[k] < DMR_BP_AUTO_HITS)
            ++votes[k];
        if (votes[k] >= DMR_BP_AUTO_HITS)
            return (uint8_t)k;
    }
    return 0;
#endif
}

#endif
