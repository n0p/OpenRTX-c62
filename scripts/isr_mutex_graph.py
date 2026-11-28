#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Call-graph search for "a mutex taken from an interrupt" in a Zephyr ELF.

The kernel aborts with

    ASSERTION FAIL [!arch_is_in_isr()] @ zephyr/kernel/mutex.c:101

when k_mutex_lock() runs with IPSR != 0, so the question "which ISR does it"
is a graph question on the linked image: start at every entry the kernel can
enter in interrupt context, follow the calls, stop at a function that holds
the assertion.

What counts as an interrupt entry:

  * every slot of .sw_isr_table (what IRQ_CONNECT and the gen_isr_tables
    script put in the vector table),
  * the same handlers again from .intList, which carries the IRQ number and
    the name of the interrupt controller entry, for the report,
  * k_timer expiry functions.  They are called from the system clock ISR, so
    a timer callback is interrupt context: the firmware's own k_timer
    objects are read out of the image and their expiry_fn field is a root.
  * functions the tree marks as ISR callbacks explicitly (--extra-root).

How calls are followed: direct bl/b/blx-immediate, and the literal-pool
pattern GCC emits for indirect calls (ldr rN, [pc, #k] ; blx rN), which
resolves to a function pointer stored in .rodata.  A call through a struct
field (a driver op table) cannot be resolved here; those sites are counted
and printed, because a path that ends in one of them is not a proof.

Usage:
  scripts/isr_mutex_graph.py zephyr.elf [-v] [--max-depth N]
  scripts/isr_mutex_graph.py zephyr.elf --to k_mutex_lock
"""

import argparse
import subprocess
import sys
from collections import deque

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS, CS_OP_IMM
from capstone.arm_const import (ARM_REG_PC, ARM_REG_R0, ARM_REG_R1, ARM_REG_R2,
                                ARM_REG_R3, ARM_REG_R12, ARM_REG_LR)

# capstone numbers the ARM registers itself (r0 is 66, pc is 11), so the
# registers a call clobbers are spelled out here rather than as literals.
CALL_CLOBBER = (ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3,
                ARM_REG_R12, ARM_REG_LR)
from elftools.elf.elffile import ELFFile

MUTEX_ASSERT = b"mutexes cannot be used inside ISRs"

# Functions that lock a mutex, by symbol name.  The assertion lives in
# z_impl_k_mutex_lock; GCC also emits constprop/isra copies of it inside the
# kernel and in callers, so the string reference is what actually identifies
# them (see mutex_entry_points()).
MUTEX_SYMS = (
    "z_impl_k_mutex_lock",
    "k_mutex_lock",
    "pthread_mutex_lock",
    "z_impl_k_condvar_wait",
)


def build_symtab(elf):
    """{addr: (name, size, type)} for every symbol with an address."""
    syms = {}
    for secname in (".symtab",):
        sec = elf.get_section_by_name(secname)
        if sec is None:
            continue
        for s in sec.iter_symbols():
            if s["st_value"] == 0 or s["st_info"]["type"] not in (
                "STT_FUNC", "STT_OBJECT", "STT_NOTYPE"
            ):
                continue
            addr = s["st_value"] & ~1
            name = s.name or "<anon>"
            # Prefer a named symbol over an empty one at the same address
            if addr not in syms or (syms[addr][0].startswith("$")
                                    and not name.startswith("$")):
                syms[addr] = (name, s["st_size"], s["st_info"]["type"])
    return syms


def sections(elf):
    out = {}
    for sec in elf.iter_sections():
        if sec["sh_flags"] & 0x2:  # SHF_ALLOC
            out[sec.name] = sec
    return out


_MEM = []


def load_mem(elf):
    """Snapshot of every allocated section, for cheap address lookups."""
    global _MEM
    _MEM = []
    for sec in elf.iter_sections():
        if sec["sh_type"] in ("SHT_NOBITS",) or not (sec["sh_flags"] & 0x2):
            continue
        if sec["sh_size"] == 0:
            continue
        _MEM.append((sec["sh_addr"], sec["sh_addr"] + sec["sh_size"], sec.data()))
    _MEM.sort()


def read_bytes(sections_map, elf, addr, count):
    for start, end, data in _MEM:
        if start <= addr < end:
            off = addr - start
            return data[off:off + count]
    return b""


def mutex_entry_points(elf, secs):
    """Functions whose body references the mutex assertion, i.e. lock sites."""
    found = set()
    for sec in elf.iter_sections():
        if sec["sh_type"] != "SHT_PROGBITS":
            continue
        data = sec.data()
        if MUTEX_ASSERT not in data:
            continue
        # Find every address that holds a pointer to that string
        base = sec["sh_addr"]
        msg_off = data.find(MUTEX_ASSERT)
        msg_addr = base + msg_off
        for other in elf.iter_sections():
            if other["sh_type"] == "SHT_NOBITS" or other.name.startswith(".debug"):
                continue
            d = other.data()
            ptr = msg_addr.to_bytes(4, "little")
            pos = d.find(ptr)
            while pos != -1:
                if pos % 4 == 0:
                    found.add(other["sh_addr"] + pos)
                pos = d.find(ptr, pos + 4)
    return found, msg_addr


def dwarf_k_timers(elf):
    """(name, address, size) of every global whose type is struct k_timer."""
    out = []
    try:
        dwarf = elf.get_dwarf_info()
    except Exception:
        return out

    def die_at(cu, ref):
        # DWARF 4 type references are relative to the CU header in .debug_info
        return cu.get_DIE_from_refaddr(cu.cu_offset + ref)

    def type_name(cu, die, depth=0):
        while die is not None and depth < 8:
            if die.tag == "DW_TAG_base_type" or die.tag == "DW_TAG_structure_type" \
                    or die.tag == "DW_TAG_typedef":
                n = die.attributes.get("DW_AT_name")
                if n is not None and die.tag != "DW_TAG_typedef":
                    return n.value.decode()
            ref = die.attributes.get("DW_AT_type")
            if ref is None:
                return None
            die = die_at(cu, ref.value)
            depth += 1
        return None

    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag != "DW_TAG_variable":
                continue
            loc = die.attributes.get("DW_AT_location")
            name = die.attributes.get("DW_AT_name")
            if loc is None or name is None or loc.form != "DW_FORM_exprloc":
                continue
            expr = loc.value
            # DW_OP_addr: fixed global (0x03 + 4-byte address)
            if len(expr) != 5 or expr[0] != 0x03:
                continue
            addr = int.from_bytes(expr[1:5], "little")
            tname = type_name(cu, die_at(cu, die.attributes["DW_AT_type"].value)
                              if "DW_AT_type" in die.attributes else None)
            if tname in ("k_timer", "struct k_timer"):
                out.append((name.value.decode(), addr, 0x40))
    return out


def reach(edges, roots):
    seen = set()
    q = deque()
    for r in roots:
        if r not in seen:
            seen.add(r)
            q.append(r)
    while q:
        cur = q.popleft()
        for nxt, _kind, _at in edges.get(cur, ()):
            if nxt not in seen:
                seen.add(nxt)
                q.append(nxt)
    return seen


def reaches_set(edges, goals):
    rev = {}
    for s, ds in edges.items():
        for d, _k, _a in ds:
            rev.setdefault(d, set()).add(s)
    seen = set(goals)
    q = deque(goals)
    while q:
        cur = q.popleft()
        for p in rev.get(cur, ()):
            if p not in seen:
                seen.add(p)
                q.append(p)
    return seen


def address_taken(elf, syms):
    """Every function whose address is stored in an allocated data section.

    This is the over-approximation of the targets of an indirect call: a code
    pointer that the linker wrote into the image has to come from one of these
    words.  A call through a computed offset into a table is covered too,
    because the table's words are what it lands on.
    """
    is_func = {a for a, v in syms.items() if v[2] == "STT_FUNC"}
    taken = set()
    for start, end, data in _MEM:
        for off in range(0, max(0, len(data) - 3), 4):
            val = int.from_bytes(data[off:off + 4], "little")
            addr = val & ~1
            if addr in is_func:
                taken.add(addr)
    return taken


def audit(elf, syms, secs, edges, roots, goals, unresolved):
    """Does any ISR path end in a mutex lock, including indirect calls?"""
    r = reach(edges, roots)
    m = reaches_set(edges, goals)
    taken = address_taken(elf, syms)
    direct = r & m

    print("== audit")
    print("   functions reachable from an interrupt entry (direct calls): %d" % len(r))
    print("   functions that reach a mutex lock (direct calls):           %d" % len(m))
    print("   functions whose address is taken in the image:              %d"
          % len(taken))
    print("   in both the ISR set and the mutex set:                      %d" % len(direct))
    for f in sorted(direct):
        print("     ! %s" % syms[f][0])

    # Indirect sites inside the ISR-reachable set, resolved against every
    # address-taken function.  If none of those targets reaches a lock, then
    # the assertion cannot come from an interrupt entry this tool knows.
    cand = sorted((taken & m))
    print("   address-taken functions that reach a mutex lock:            %d"
          % len(cand))
    for f in sorted(cand):
        print("     ~ %-44s 0x%08x" % (syms[f][0], f))
    sites = [(f, at) for f in r for at in unresolved.get(f, [])]
    print("   indirect call sites inside the ISR set:                     %d"
          % len(sites))
    for f, at in sorted(sites):
        val = read_bytes(secs, elf, at, 2)
        print("     ~ %-40s 0x%08x" % (syms[f][0], at))
    print("   -> an ISR can only reach one of those through a code pointer "
          "read from the image;")
    print("      resolve the indirect sites in the ISR set by hand to close "
          "the argument.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--to", default=None,
                    help="stop at this symbol instead of the mutex lock")
    ap.add_argument("--max-depth", type=int, default=40)
    ap.add_argument("--extra-root", action="append", default=[])
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--edges-out", default=None,
                    help="write the whole call graph as src dst triples")
    ap.add_argument("--audit", action="store_true",
                    help="sound check: can any indirect call from an ISR reach "
                         "a mutex lock through a code pointer stored in the image?")
    args = ap.parse_args()

    with open(args.elf, "rb") as f:
        elf = ELFFile(f)
        syms = build_symtab(elf)
        secs = sections(elf)
        load_mem(elf)

        funcs = sorted((a, v) for a, v in syms.items() if v[2] == "STT_FUNC")
        starts = [a for a, _ in funcs]

        def func_at(addr):
            lo, hi = 0, len(starts) - 1
            best = None
            while lo <= hi:
                mid = (lo + hi) // 2
                if starts[mid] <= addr:
                    best = mid
                    lo = mid + 1
                else:
                    hi = mid - 1
            if best is None:
                return None
            a = starts[best]
            name, size, _ = syms[a]
            if size and addr >= a + size:
                return None
            return (a, name)

        # ---- mutex lock entry points
        lock_sites, msg_addr = mutex_entry_points(elf, secs)
        lock_funcs = {}
        for addr in lock_sites:
            f = func_at(addr)
            if f:
                lock_funcs.setdefault(f[0], f[1])
        # Named ones too, in case the string landed in a shared literal pool
        for a, (name, size, typ) in syms.items():
            if typ != "STT_FUNC":
                continue
            if any(name == m or name.startswith(m + ".") or name.startswith(m + ".constprop")
                   for m in MUTEX_SYMS):
                lock_funcs[a] = name

        # ---- isr roots
        roots = {}
        # struct _isr_table_entry { const void *arg; void (*isr)(const void *); }
        table = secs.get("sw_isr_table")
        if table is not None:
            d = table.data()
            for i in range(0, len(d), 8):
                isr = int.from_bytes(d[i + 4:i + 8], "little")
                f = func_at(isr & ~1) if isr else None
                if f and f[1] != "z_irq_spurious":
                    roots.setdefault(f[0], "sw_isr_table[%d]" % (i // 8))

        # k_timer objects, from the debug info: an expiry function runs in the
        # system clock ISR, so every k_timer with a callback is a root.  The
        # objects are looked up by type name so that a vtable or a driver's op
        # table full of code pointers is not mistaken for one.
        for name, addr, size in dwarf_k_timers(elf):
            raw = read_bytes(secs, elf, addr, size)
            for off in range(0, len(raw) - 3, 4):
                val = int.from_bytes(raw[off:off + 4], "little")
                f = func_at(val & ~1) if val else None
                if f and (val & 1):
                    roots.setdefault(f[0], "k_timer callback of %s" % name)
                    break

        for name in args.extra_root:
            for a, (n, size, typ) in syms.items():
                if n == name and typ == "STT_FUNC":
                    roots[a] = "extra root"

        # ---- disassemble and build the graph
        text = None
        for sec in elf.iter_sections():
            if sec.name == "text":
                text = sec
        if text is None:
            text = secs.get(".text")
        code = text.data()
        base = text["sh_addr"]
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        md.detail = True
        # skipdata: .text holds literal pools and padding that capstone cannot
        # decode; without this a single pass stops at the first one.
        md.skipdata = True

        edges = {}
        unresolved = {}
        callers_of_mutex = {}

        insns = [i for i in md.disasm(code, base)
                 if not i.mnemonic.startswith(".")]

        def stream():
            return iter(insns)

        #
        # Two facts have to be tracked to follow an indirect call: the value
        # of a register that came out of the literal pool (the address of a
        # global) and the word that global holds at an offset -- a driver op
        # table (`backend->process`, `dev->api->transceive`) is a code pointer
        # stored in .rodata, and the kernel's log path, which is where the
        # radio's own fault dump died, is reached exactly that way:
        #
        #     ldr r3, =log_backend_uart ; ldr r3, [r3, #8] ; blx r3
        #
        # So each function is walked with a small register-value map, reset at
        # its first instruction.  A value is known when it comes from an
        # immediate, from the literal pool, or from a word of the image read at
        # a known address; a call clobbers r0-r3 and r12.
        def u32(addr):
            raw = read_bytes(secs, elf, addr, 4)
            return int.from_bytes(raw, "little") if len(raw) == 4 else None

        def fn_of(val):
            if val is None:
                return None
            f = func_at(val & ~1)
            return f if (f and (val & 1)) else None

        reads, writes = {}, {}
        for insn in insns:
            src = func_at(insn.address)
            if src is None:
                continue
            if src[0] != reads.get("__cur"):
                reads.clear()
                writes.clear()
                reads["__cur"] = src[0]

            ops = insn.operands
            mnem = insn.mnemonic.split(".")[0]
            _, wregs = insn.regs_access()
            wrote = set(wregs)

            # ---- calls first: they consume the register values
            if mnem in ("bl", "b") and ops and ops[0].type == CS_OP_IMM:
                dst = ops[0].imm & ~1
                tgt = func_at(dst)
                if tgt is not None and (mnem == "bl" or dst == tgt[0]):
                    edges.setdefault(src[0], set()).add(
                        (tgt[0], mnem, insn.address))
            elif mnem in ("blx", "bx") and ops:
                op = ops[0]
                # bx lr is a return, not a call through a pointer
                if mnem == "bx" and op.type != CS_OP_IMM and op.reg == ARM_REG_LR:
                    reads.clear()
                    continue
                tgt = fn_of(op.imm) if op.type == CS_OP_IMM else fn_of(
                    reads.get(op.reg))
                if tgt is not None:
                    edges.setdefault(src[0], set()).add(
                        (tgt[0], mnem + "*", insn.address))
                else:
                    unresolved.setdefault(src[0], []).append(insn.address)

            # ---- then the data flow
            newval = {}
            if mnem == "ldr" and len(ops) == 2 and ops[1].type == 3:
                dst, mem = ops[0], ops[1]
                if mem.mem.base == ARM_REG_PC:    # literal pool
                    # Thumb reads the PC as (address + 4) rounded down to a
                    # word, which is what the assembler encoded the offset
                    # against.
                    lit = ((insn.address + 4) & 0xFFFFFFFC) + mem.mem.disp
                    newval[dst.reg] = u32(lit)
                else:
                    baseval = reads.get(mem.mem.base)
                    if baseval is not None:
                        newval[dst.reg] = u32(baseval + mem.mem.disp)
            elif mnem in ("mov", "movw") and len(ops) == 2 and \
                    ops[1].type == CS_OP_IMM:
                newval[ops[0].reg] = ops[1].imm
            elif mnem in ("add", "addw", "sub", "subw") and len(ops) == 3 and \
                    ops[2].type == CS_OP_IMM and ops[1].type == 1:
                baseval = reads.get(ops[1].reg)
                if baseval is not None:
                    newval[ops[0].reg] = (baseval + ops[2].imm) & 0xFFFFFFFF

            if mnem in ("bl", "blx"):
                for r in CALL_CLOBBER:
                    reads.pop(r, None)
            for r in wrote:
                reads.pop(r, None)
                if r not in newval:
                    pass
            reads.update(newval)

        if args.edges_out:
            with open(args.edges_out, "w") as fh:
                for s, ds in edges.items():
                    for d, kind, at in ds:
                        fh.write("%s\t%s\t%s\t%x\n" %
                                 (syms.get(s, ("?",))[0],
                                  syms.get(d, ("?",))[0], kind, at))

        # ---- search
        stop = {a for a, (n, s, t) in syms.items() if t == "STT_FUNC" and
                args.to and (n == args.to or n.startswith(args.to + "."))}
        goal = stop if args.to else set(lock_funcs)

        def name_of(a, fallback=None):
            n = syms.get(a, (fallback or "?",))[0]
            return n

        found = []
        for root, why in sorted(roots.items()):
            # BFS
            prev = {root: None}
            q = deque([(root, 0)])
            hit = None
            while q:
                cur, depth = q.popleft()
                if cur in goal and cur != root:
                    hit = cur
                    break
                if depth >= args.max_depth:
                    continue
                for nxt, kind, at in sorted(edges.get(cur, ())):
                    if nxt not in prev:
                        prev[nxt] = (cur, kind, at)
                        q.append((nxt, depth + 1))
            if hit is None:
                continue
            path = []
            cur = hit
            while cur is not None:
                step = prev[cur]
                if step is None:
                    path.append((cur, None, None))
                    break
                path.append((cur, step[1], step[2]))
                cur = step[0]
            path.reverse()
            found.append((root, why, path))

        print("== interrupt entries found: %d" % len(roots))
        for root, why in sorted(roots.items(), key=lambda kv: kv[1]):
            print("   %-28s %s" % (name_of(root), why))
        print("== mutex lock sites: %d" % len(goal))
        for g in sorted(goal):
            print("   %-40s %s" % (name_of(g), hex(g)))
        print("== indirect call sites not resolved: %d functions, %d sites"
              % (len(unresolved), sum(len(v) for v in unresolved.values())))
        if args.verbose:
            for f, sites in sorted(unresolved.items(), key=lambda kv: -len(kv[1]))[:20]:
                print("   %-40s %d sites" % (name_of(f), len(sites)))

        if args.audit:
            audit(elf, syms, secs, edges, roots, goal, unresolved)
        print("== paths from an interrupt entry to a mutex lock: %d" % len(found))
        if not found:
            print("   none: no interrupt entry reaches k_mutex_lock through "
                  "direct calls")
        for root, why, path in found:
            print("\n  %s  [%s]" % (name_of(root), why))
            for a, kind, at in path:
                if kind is None:
                    print("    %s" % name_of(a))
                else:
                    print("    %-8s %-44s at 0x%x" % (kind, name_of(a), at))
            # source lines for the calls
            addrs = [at for (a, kind, at) in path if at]
            if addrs:
                out = subprocess.run(
                    ["arm-none-eabi-addr2line", "-e", args.elf, "-f", "-C"] +
                    [hex(a - 2 if a else 0) for a in addrs],
                    capture_output=True, text=True).stdout.splitlines()
                pairs = list(zip(out[0::2], out[1::2]))
                print("    --- call sites ---")
                for (fn, loc), at in zip(pairs, addrs):
                    print("    0x%08x  %s  %s" % (at, fn.strip(), loc.strip()))
        return 0


if __name__ == "__main__":
    sys.exit(main())
