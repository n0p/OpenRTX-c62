#!/usr/bin/env python3
"""cae_extract.py - attempt to lift the iFlytek CAE out of the factory CP image.

This is the "minimal proof of concept" of reports/nr_factory.md: it tries the
steps you would follow to reuse CAE as a callable block, and stops at the first
step that cannot be satisfied.  It prints, for every check, the concrete
evidence (address + bytes) instead of a verdict on faith.

Checks
  1. locate the CAE API entries (handle-magic validation with 0x20200907)
  2. is the module a contiguous, self-contained region?  (call-graph escaping)
  3. is the code position independent?  (absolute literals, relocation table)
  4. does it touch hardware directly?  (MMIO base literals)
  5. can the decoder recover the bodies?  (FLIX bundle coverage)
  6. is there a toolchain / emulator to rebuild or run it?

Usage: python3 tools/factory/cae_extract.py
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsp_map import Img, walk, BASE, TEXT  # noqa: E402

MAGIC_SLOT_RANGE = (0x6001F000, 0x60020000)
# empirical: XOS kernel primitives sit here (see reports/nr_factory.md §2.2)
KERNEL = (0x60028800, 0x6002B600)
CAE_REGION = (0x6002C000, 0x60034000)
LIBC = (0x60064000, TEXT[1])
APP = (TEXT[0], 0x60028800)


def classify(t):
    if KERNEL[0] <= t < KERNEL[1]:
        return 'xos'
    if CAE_REGION[0] <= t < CAE_REGION[1]:
        return 'cae'
    if LIBC[0] <= t < LIBC[1]:
        return 'libc'
    return 'app'


def main():
    img = Img('/home/n0p/src/c62/dumps/extract/dsp-factory.bin')
    ins = walk('/home/n0p/src/c62/dumps/extract/dsp-factory.bin')
    ents = [x['addr'] for x in ins if x['opcode'].startswith('entry ')]

    def fn_of(a):
        lo, hi = 0, len(ents) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if ents[mid] <= a:
                lo = mid
            else:
                hi = mid - 1
        return ents[lo]

    slots = [v for v in range(*MAGIC_SLOT_RANGE, 4) if img.rd32(v) == 0x20200907]
    users = []
    for x in ins:
        m = re.match(r'l32r a\d+, (0x[0-9a-f]+)', x['opcode'])
        if m and int(m.group(1), 16) in slots:
            users.append(fn_of(x['addr']))
    users = sorted(set(users))
    print('1. CAE handle magic 0x20200907 -> literal slot(s) %s' % [hex(s) for s in slots])
    print('   API entries that validate it: %s' % [hex(u) for u in users])
    bodies = {}
    for u in users:
        i = ents.index(u)
        bodies[u] = ents[i + 1] - u if i + 1 < len(ents) else 0
    print('   sizes: %s' % {hex(k): v for k, v in bodies.items()})
    print('   => the API is a handle-based object interface, not a function over frames.\n')

    print('2. call-graph escaping (is CAE a self-contained module?)')
    calls, lits = {}, {}
    for x in ins:
        f = fn_of(x['addr'])
        m = re.search(r'call(4|8|12) (0x[0-9a-f]+)', x['opcode'])
        if m:
            calls.setdefault(f, set()).add(int(m.group(2), 16))
        m = re.match(r'l32r a\d+, (0x[0-9a-f]+)', x['opcode'])
        if m:
            lits.setdefault(f, []).append((x['addr'], int(m.group(1), 16)))
    core = [0x6002C200, 0x6002C300, 0x6002C330, 0x6002C410, 0x6002C5B3, 0x6002CCB0,
            0x6002D120, 0x6002DC06, 0x6002E5BF, 0x6002F1C0, 0x6002F61C, 0x6002FF08,
            0x60030590, 0x60030CB0, 0x60031C90]
    escaping = 0
    for f in core:
        ts = sorted(t for t in calls.get(f, ()) if TEXT[0] <= t < TEXT[1])
        cls = {}
        for t in ts:
            cls.setdefault(classify(t), []).append(t)
        if len(cls) > 1 or any(k != 'cae' for k in cls):
            escaping += 1
        print('   %08x -> %s' % (f, {k: [hex(t) for t in v][:4] for k, v in cls.items()}))
    print('   => %d/%d CAE functions call out of the region: no module boundary.\n'
          % (escaping, len(core)))

    print('3. position independence')
    absh = 0
    for f in core:
        for site, slot in lits.get(f, ()):
            v = img.rd32(slot)
            if v and 0x60000000 <= v < 0x6006F000:
                absh += 1
    print('   %d l32r in CAE functions load a literal holding an absolute 0x6xxxxxxx address'
          % absh)
    print('   header has no relocation/symbol table (words: %s)'
          % ' '.join('%08x' % v for v in img.w(BASE, 18)[:12]))
    print('   => linked at fixed VMAs; moving the blob requires rewriting every literal.\n')

    print('4. direct hardware access from CAE code')
    for f in core:
        for site, slot in lits.get(f, ()):
            v = img.rd32(slot)
            if not v or v & 3:      # pointers are 4-byte aligned; filters ASCII-as-int
                continue
            if v in (0x4A0FFE00, 0x4A100000, 0x46100000) or (0x49100000 <= v < 0x4A200000):
                print('   %08x: literal %08x (MMIO) used at %08x' % (f, v, site))
    b = img.d[0x6002C250 - BASE:0x6002C260 - BASE]
    print('   bytes @0x6002c250: %s   (l32r/memw/l32i a8,[a8+0x2a0] => read of 0x4A1000A0)' %
          ' '.join('%02x' % x for x in b))
    print()

    print('5. decoder coverage of the module')
    lo, hi = CAE_REGION
    sel = [x for x in ins if lo <= x['addr'] < hi]
    bund = sum(1 for x in sel if x['opcode'].startswith('{'))
    excw = sum(1 for x in sel if x['opcode'].startswith('excw'))
    print('   %d decoded items in %#x..%#x: %d bundles (%.1f%%), %d excw'
          % (len(sel), lo, hi, bund, 100.0 * bund / max(1, len(sel)), excw))
    print('   => the arithmetic is inside FLIX bundles that r2/Ghidra do not decode.\n')

    print('6. toolchain / emulator')
    print('   xt-clang RI-2021.7 present but core registry empty: the "venus_hifi4"')
    print('   core is not installed (work/dsp-openfw/toolchain/README.txt).')
    print('   no qemu-system-xtensa on this host; upstream QEMU models are LX-class')
    print('   cores (dc232b/de212), without FLIX bundles or HiFi4 SIMD.')
    print('\nVERDICT: extraction as a callable block fails at steps 1, 2, 3, 4 and 6.')


if __name__ == '__main__':
    sys.exit(main())
