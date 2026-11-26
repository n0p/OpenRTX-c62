#!/usr/bin/env python3
"""Run dsd-neo over a directory of noisy captures and count what it decodes.

Companion of tests/platform/c62_nxdn_bench.c: that harness writes one WAV per
(Eb/N0, seed) with `--write-dir`, and this script feeds the very same files to
the reference decoder, so the two receivers are compared on identical input:

    ./nxdn_bench --write-dir /tmp/wavs --seeds 4 --snr 16,12,8,4,0 capture.wav
    tools/nxdn_ref_compare.py /tmp/wavs --dsd-neo /path/to/dsd-neo

Counts, from dsd-neo's own trace (-Z and --frame-log):

    syncs   "Sync: NXDN48" lines          frames it locked on
    lich    "LICH ERR" lines              frames whose LICH failed
    sacch   "SF Segment #" lines minus the "CRC ERR" ones it flags
                                         SACCH blocks whose CRC-6 passed
    ambe    "FRAME AMBE" lines            AMBE+2 frames it produced

The output is one CSV row per file plus the mean per Eb/N0, in the same shape
as the firmware bench's table.
"""

import argparse
import collections
import csv
import glob
import os
import re
import subprocess
import sys
import tempfile

SYNC = re.compile(r'Sync: NXDN48')
LICH = re.compile(r'LICH ERR')
SEG = re.compile(r'SF Segment #')
CRCERR = re.compile(r'CRC ERR')
AMBE = re.compile(r'FRAME AMBE')


def run_one(dsd, wav):
    with tempfile.TemporaryDirectory() as tmp:
        log = os.path.join(tmp, 'frames.log')
        out = subprocess.run([dsd, '-fi', '-i', wav, '-o', 'null', '-Z',
                              '--frame-log', log],
                             capture_output=True, text=True, timeout=120)
        trace = out.stderr + out.stdout
        try:
            with open(log) as f:
                trace += f.read()
        except OSError:
            pass

    return {
        'syncs': len(SYNC.findall(trace)),
        'lich': len(LICH.findall(trace)),
        'sacch': len(SEG.findall(trace)) - len(CRCERR.findall(trace)),
        'ambe': len(AMBE.findall(trace)),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wavdir')
    ap.add_argument('--dsd-neo', default='dsd-neo')
    ap.add_argument('--csv')
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.wavdir, 'nxdn_ebn0_*.wav')))
    if not files:
        sys.exit('no nxdn_ebn0_*.wav in %s' % args.wavdir)

    rows = []
    for path in files:
        m = re.search(r'ebn0_(-?\d+)_seed(\d+)', os.path.basename(path))
        ebn0 = int(m.group(1)) / 10.0
        seed = int(m.group(2))
        r = run_one(args.dsd_neo, path)
        rows.append(dict(ebn0=ebn0, seed=seed, **r))

    if args.csv:
        with open(args.csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=['ebn0', 'seed', 'syncs', 'lich',
                                              'sacch', 'ambe'])
            w.writeheader()
            w.writerows(rows)

    by = collections.defaultdict(list)
    for r in rows:
        by[r['ebn0']].append(r)

    print('  Eb/N0   syncs   LICHerr   SACCH   AMBE   (dsd-neo, mean)')
    for ebn0 in sorted(by, reverse=True):
        g = by[ebn0]
        n = len(g)
        print('%6.1f  %6.1f  %7.1f  %6.1f  %5.1f'
              % (ebn0, sum(r['syncs'] for r in g) / n,
                 sum(r['lich'] for r in g) / n,
                 sum(r['sacch'] for r in g) / n,
                 sum(r['ambe'] for r in g) / n))


if __name__ == '__main__':
    main()
