#!/usr/bin/env python3
"""model_info.py - inspect the factory NPU denoiser container (thinker10).

The factory image's CMLP entry (dumps/extract/model-cmlp.bin, 429 040 B) is a
ListenAI "thinker10" model container: an int8 graph plus its weight payload and
a trailing name table.  The script prints the op set, the node count and the
layout, which is what tells you *what runtime* is needed to execute it (Thinker
with GRUInt/ConvTranspose2dInt, not a WASM/AOT runtime).

Usage: python3 tools/factory/model_info.py [model-cmlp.bin]
"""
import struct
import sys

OPS16 = ['Split', 'Conv2dInt', 'iqCat', 'Reshape', 'Transpose', 'iqAdd', 'LayerNormInt',
         'GRUInt', 'LinearInt', 'Requant', 'iqTanh', 'iqMul', 'ConvTranspose2dInt', 'Slice']


def find_op_table(d):
    """Locate the 14-name, 32-byte-stride op table."""
    for o in range(0, len(d) - 32 * len(OPS16)):
        if d[o:o + 6] != b'Split\x00':
            continue
        names = [d[o + 32 * i:o + 32 * i + 32].split(b'\x00')[0] for i in range(len(OPS16))]
        if all(n.decode('latin-1') in OPS16 for n in names):
            return o, [n.decode() for n in names]
    return None, []


def name_table(d, off, stride=0x40, maxn=2000, skip=0x18):
    out, o = [], off + skip
    while o < len(d) - stride and len(out) < maxn:
        s = d[o:o + stride].split(b'\x00')[0]
        if not s or not all(32 <= c < 127 for c in s):
            break
        out.append((o, s.decode('latin-1')))
        o += stride
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else '/home/n0p/src/c62/dumps/extract/model-cmlp.bin'
    d = open(path, 'rb').read()
    print('file            %s  (%d bytes)' % (path, len(d)))
    print('magic           %r' % d[:16].split(b'\x00')[0])
    words = struct.unpack('<12I', d[0x10:0x40])
    print('header words    %s' % ' '.join('%08x' % w for w in words))
    for i, w in enumerate(words):
        if 0 < w < len(d):
            print('   +%02x -> %#x (%d)  in file' % (0x10 + 4 * i, w, w))

    op_off, ops = find_op_table(d)
    if op_off:
        hdr = op_off - 0x10                       # counts sit just before the names
        nodes, nops = struct.unpack('<HH', d[hdr:hdr + 4])
        extra = struct.unpack('<II', d[hdr + 8:hdr + 16])
        print('\nop table @ %#x  counts @ %#x   nodes=%d  distinct ops=%d  extra=%s'
              % (op_off, hdr, nodes, nops, extra))
        print('ops: %s' % ', '.join(ops))
        assert nops == len(ops), 'op count mismatch: %d vs %d' % (nops, len(ops))
    tab = words[7] if len(words) > 7 else 0          # 0x2C: tensor name table offset
    if 0 < tab < len(d):
        nt = name_table(d, tab)
        print('\ntensor/name table @ %#x: %d names, first %r last %r'
              % (tab, len(nt), nt[0][1] if nt else None, nt[-1][1] if nt else None))
        states = [n for _, n in nt if 'state' in n or 'history' in n]
        print('stateful tensors: %d (e.g. %s)' % (len(states), states[:3]))
    if op_off and tab:
        print('\nbody (graph + int8 weights) 0x%x..0x%x = %d bytes' % (op_off, tab, tab - op_off))
    print('\nweight payload ends at/after 0x%x; trailing tables to 0x%x'
          % (tab, len(d)))


if __name__ == '__main__':
    sys.exit(main())
