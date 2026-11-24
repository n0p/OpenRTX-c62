#!/usr/bin/env python3
"""dsp_map.py - map the factory CSK6011B CP image (TALKIE / rpcsrv / iFlytek CAE).

Reads dumps/extract/dsp-factory.bin (VMA base 0x60000000) and prints:
  * reset stub / store-table (XtOS)
  * algorithm module descriptors (rate, frame, channels)
  * rpcsrv command (37) and parameter (35) dispatch tables
  * iFlytek CAE anchors: version/error strings, handle magic checks,
    API wrapper addresses, hardware (NPU/VAD) literal references
  * the call/string-reference graph of the CAE region

The instruction stream is walked with radare2's xtensa disassembler
(`r2 -a xtensa -c 'pDj N @ addr'`).  r2 decodes the base ISA and the 24-bit
instructions, but not FLIX bundle slots: it prints them as `excw`.  Every
figure derived from instruction text therefore *undercounts*; the script
prints the coverage so the numbers can be judged.

Usage:  python3 tools/factory/dsp_map.py [-i image] [--no-walk]
"""
import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

BASE = 0x60000000
TEXT = (0x6001F0B0, 0x6006ED6C)          # .text of this build
CAE_MAGIC = 0x20200907                    # CAE handle signature
CAE_VERSION_STR = 0x60003D64              # "6011.call.v1101.3.3.4 beta"
CAE_API_HINT = (0x6002C200, 0x60034000)   # CAE wrappers + core (empirical)


class Img:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self.base = BASE

    def w(self, vma, n=1):
        o = vma - self.base
        return struct.unpack('<%dI' % n, self.d[o:o + 4 * n])

    def rd32(self, vma):
        o = vma - self.base
        if 0 <= o <= len(self.d) - 4:
            return struct.unpack('<I', self.d[o:o + 4])[0]

    def cstr(self, vma, maxlen=90):
        o, s = vma - self.base, b''
        while 0 <= o < len(self.d) and self.d[o] not in (0, 10, 13) and len(s) < maxlen:
            s += bytes([self.d[o]])
            o += 1
        return s.decode('latin-1')


def walk(img_path, r2='r2'):
    """Sequentially disassemble .text with r2, following bundle sizes."""
    tmp = tempfile.mkdtemp(prefix='dspwalk-')
    addr, end, n = TEXT[0], TEXT[1], 0
    ins = []
    while addr < end and n < 400:
        f = os.path.join(tmp, '%03d.json' % n)
        subprocess.run([r2, '-a', 'xtensa', '-b', '32', '-m', hex(BASE), '-q',
                        '-c', 'pDj 4000 @ %s' % hex(addr),
                        img_path],
                       stdout=open(f, 'w'), stderr=subprocess.DEVNULL, check=False)
        try:
            a = json.load(open(f))
        except Exception:
            break
        if not a:
            break
        ins.extend(a)
        addr = a[-1]['addr'] + a[-1]['size']
        n += 1
    seen, out = set(), []
    for x in sorted(ins, key=lambda x: x['addr']):
        if x['addr'] not in seen:
            seen.add(x['addr'])
            out.append(x)
    return out


def header(img):
    print('== header (18 words)')
    hdr = img.w(BASE, 18)
    print('  ' + ' '.join('%08x' % v for v in hdr))
    print('  first word = j %#x  (jump over header)' % (BASE + ((hdr[0] >> 6) & 0x3FFFF) + 4))
    st = hdr[11]
    print('  store-table @ %#x  vecbase @ %#x' % (st, hdr[1]))
    print('\n== store-table')
    for i in range(60):
        a, b, c = img.w(st + i * 12, 3)
        if (a, b, c) == (0, 0, 0):
            break
        print('  %08x..%08x  size %6d  src %08x' % (a, b, b - a, c))


def modules(img):
    print('\n== algorithm module descriptors (.data)')
    pat = b'MODULE_NAME\x00'
    o = img.d.find(pat)
    while o != -1:
        base = BASE + o - 0x20                    # descriptor starts 0x20 before the tag
        p = img.w(base, 6)
        stack, rate = img.rd32(base + 0x40), img.rd32(base + 0x54)
        frame, ch, bits = (img.rd32(base + 0x58), img.rd32(base + 0x5C), img.rd32(base + 0x60))
        ver = img.cstr(base + 0x64, 20)
        print('  %08x init=%08x proc=%08x aux=%08x set=%08x reset=%08x  '
              'stack=%d rate=%d frame=%d ch=%d bits=%d  %s'
              % (base, p[0], p[1], p[2], p[3], p[4], stack, rate, frame, ch, bits, ver))
        o = img.d.find(pat, o + 1)


def tables(img):
    print('\n== rpcsrv dispatch tables (rodata)')
    for base, n, label in ((0x60000D20, 37, 'command'), (0x60000DD0, 35, 'parameter')):
        print('  %s table @ %#x, %d entries' % (label, base, n))
        vals = img.w(base, n)
        runs = {}
        for i, v in enumerate(vals):
            runs.setdefault(v, []).append(i)
        for v, ids in runs.items():
            print('    -> %08x : ids %s' % (v, ids))


def cae(img, ins=None):
    print('\n== iFlytek CAE anchors')
    for a in (0x60002E51, 0x60002EE9, 0x60002F39, 0x6000310D, 0x60003209,
              0x6000326D, 0x600032ED, 0x60003399, 0x6000340D, 0x60003439,
              0x60003CE0, 0x60003CFC, CAE_VERSION_STR, 0x60013C70, 0x60013C9C):
        print('  %08x %r' % (a, img.cstr(a, 60)))

    print('\n  handle magic %#x stored at:' % CAE_MAGIC)
    slots = [v for v in range(0x6001F000, 0x60020000, 4) if img.rd32(v) == CAE_MAGIC]
    print('   ', [hex(s) for s in slots])
    if ins:
        print('\n  code that loads the magic (i.e. validates a CAE handle):')
        for x in ins:
            m = re.match(r'l32r a\d+, (0x[0-9a-f]+)', x['opcode'])
            if m and int(m.group(1), 16) in slots:
                print('    %08x  %s' % (x['addr'], x['opcode']))

    print('\n  nonzero bytes just before each API wrapper (entry instructions):')
    for a in (0x6002C200, 0x6002C300, 0x6002C330, 0x6002C360, 0x6002C410, 0x6002CCB0, 0x6002F61C):
        print('    %08x  %s' % (a, ' '.join('%02x' % b for b in
                                            img.d[a - BASE:a - BASE + 12])))

    print('\n  hardware literals referenced from the CAE region (l32r -> MMIO):')
    if ins:
        for x in [i for i in ins if CAE_API_HINT[0] <= i['addr'] < CAE_API_HINT[1]]:
            m = re.match(r'l32r a\d+, (0x[0-9a-f]+)', x['opcode'])
            if not m:
                continue
            v = img.rd32(int(m.group(1), 16))
            if v and not (v & 3) and (0x4A000000 <= v < 0x4B000000 or 0x49100000 <= v < 0x4A000000
                      or 0x48000000 <= v < 0x49000000 or v == 0x46100000
                      or 0x200C0000 <= v < 0x20100000):
                print('    %08x -> %08x' % (x['addr'], v))


def coverage(ins):
    if not ins:
        return
    bundles = sum(1 for x in ins if x['opcode'].startswith('{'))
    excw = sum(1 for x in ins if x['opcode'].startswith('excw'))
    ill = sum(1 for x in ins if x['opcode'].startswith('ill'))
    n = len(ins)
    print('\n== decoder coverage')
    print('  %d decoded items: %d FLIX bundles (%.1f%%), %d excw (%.1f%%), %d ill (%.1f%%)'
          % (n, bundles, 100.0 * bundles / n, excw, 100.0 * excw / n, ill, 100.0 * ill / n))
    print('  entries found: %d' % sum(1 for x in ins if x['opcode'].startswith('entry ')))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-i', '--image', default='/home/n0p/src/c62/dumps/extract/dsp-factory.bin')
    ap.add_argument('--r2-image', default=None, help='image copy for the r2 walk')
    ap.add_argument('--no-walk', action='store_true')
    a = ap.parse_args()
    img = Img(a.image)
    header(img)
    modules(img)
    tables(img)
    ins = None
    if not a.no_walk:
        ins = walk(a.image)
        coverage(ins)
    cae(img, ins)


if __name__ == '__main__':
    sys.exit(main())
