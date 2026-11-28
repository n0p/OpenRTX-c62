/* QEMU TCG plugin: per-instruction-address counters with frame snapshots.
 *
 * Same as insnhist, plus: when the guest executes the address given by
 * $INSNHIST_MARKER (hex), the current counter array is snapshotted, giving
 * per-frame instruction totals and per-frame per-address profiles in a single
 * run. Dump format (text): first line "NINSN NFRAMES", then NINSN lines
 * "<hexaddr>", then per frame one line "TOTAL" followed by NINSN counts.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAXI (1u << 20)
#define HCAP (1u << 21)
#define MAXF 256

static uint64_t counters[MAXI];
static uint64_t iaddr[MAXI];
static uint64_t htkey[HCAP];
static uint32_t htval[HCAP];
static size_t ninsn = 0;
static int overflow = 0;
static uint64_t total = 0;
static uint64_t marker_addr = 0;
static int marker_seen = 0;

static uint64_t snap_total[MAXF];
static size_t snap_len[MAXF];
static uint64_t *snap[MAXF];
static int nframes = 0;

static uint32_t slot_for(uint64_t addr)
{
    uint32_t h = (uint32_t)((addr >> 1) & (HCAP - 1));
    for (;;) {
        if (htval[h] == 0) {
            if (ninsn >= MAXI) { overflow = 1; return 0; }
            htval[h] = (uint32_t)(++ninsn);
            htkey[h] = addr;
            iaddr[ninsn - 1] = addr;
            return htval[h] - 1;
        }
        if (htkey[h] == addr)
            return htval[h] - 1;
        h = (h + 1) & (HCAP - 1);
    }
}

static void insn_exec(unsigned int vcpu, void *udata)
{
    (void)vcpu;
    counters[(uint32_t)(uintptr_t)udata]++;
    total++;
}

static void marker_exec(unsigned int vcpu, void *udata)
{
    (void)vcpu; (void)udata;
    if (nframes >= MAXF)
        return;
    uint64_t *s = malloc(ninsn * sizeof(uint64_t));
    if (!s)
        return;
    memcpy(s, counters, ninsn * sizeof(uint64_t));
    snap_total[nframes] = total;
    snap_len[nframes] = ninsn;
    snap[nframes] = s;
    nframes++;
}

static void tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    (void)udata;
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(insn);
        if (marker_addr && va == marker_addr) {
            marker_seen = 1;
            qemu_plugin_register_vcpu_insn_exec_cb(insn, marker_exec,
                                                   QEMU_PLUGIN_CB_NO_REGS, NULL);
            continue;
        }
        uint32_t idx = slot_for(va);
        if (overflow)
            return;
        qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                                               QEMU_PLUGIN_CB_NO_REGS,
                                               (void *)(uintptr_t)idx);
    }
}

static void plugin_exit(void *p)
{
    (void)p;
    const char *path = getenv("INSNHIST_OUT");
    FILE *f = path ? fopen(path, "w") : NULL;
    const char *mk = getenv("INSNHIST_MARKER");
    if (mk) marker_addr = strtoull(mk, NULL, 16);
    if (f) {
        fprintf(f, "%zu %d %llu\n", ninsn, nframes, (unsigned long long)total);
        for (size_t i = 0; i < ninsn; i++)
            fprintf(f, "%llx\n", (unsigned long long)iaddr[i]);
        for (int k = 0; k < nframes; k++) {
            fprintf(f, "%llu", (unsigned long long)snap_total[k]);
            for (size_t i = 0; i < ninsn; i++)
                fprintf(f, " %llu", i < snap_len[k]
                        ? (unsigned long long)snap[k][i] : 0ULL);
            fputc('\n', f);
        }
        fclose(f);
    }
    char buf[128];
    snprintf(buf, sizeof buf, "INSN_COUNT %llu distinct=%zu frames=%d marker=%d\n",
             (unsigned long long)total, ninsn, nframes, marker_seen);
    qemu_plugin_outs(buf);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info; (void)argc; (void)argv;
    const char *mk = getenv("INSNHIST_MARKER");
    if (mk) marker_addr = strtoull(mk, NULL, 16);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
