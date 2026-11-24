/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Minimal QEMU TCG plugin: counts executed guest instructions (per TB). */
#include <stdio.h>
#include <stdint.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t insn_count;

static void tb_exec(unsigned int cpu_index, void *udata)
{
    insn_count += (uint64_t)(uintptr_t)udata;
}

static void tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    (void)userdata;
    size_t n = qemu_plugin_tb_n_insns(tb);
    if (!n)
        return;
    qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS,
                                         (void *)(uintptr_t)n);
}

static void plugin_exit(void *p)
{
    (void)p;
    char buf[96];
    snprintf(buf, sizeof buf, "INSN_COUNT %llu\n", (unsigned long long)insn_count);
    qemu_plugin_outs(buf);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info; (void)argc; (void)argv;
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
