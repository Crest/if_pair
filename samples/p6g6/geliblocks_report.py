#!/usr/bin/env python3
"""geliblocks_report.py FILE...: geliblocks.sh's rows, the median over the
repetitions of the worker's CPU time per block, stock GELI against
kwq-geli, writes (encryption) and reads (decryption)."""
import statistics, sys
from collections import defaultdict

for f in sys.argv[1:]:
    r = defaultdict(list); mb = defaultdict(list)
    for l in open(f):
        s = l.split()
        if len(s) != 9:
            continue
        k = (s[1], int(s[2]), int(s[3]), s[4])
        r[k].append(float(s[7])); mb[k].append(float(s[8]))
    print('\n### %s: CPU time per block, us (stock GELI / kwq-geli), and MB/s\n' % f)
    print('| | encrypt (write) | decrypt (read) | MB/s write | MB/s read |')
    print('|---|---|---|---|---|')
    for sec, b, name in ((4096, 4096, '4 KiB block, 4 KiB bios'), (4096, 1048576, '4 KiB block, 1 MiB bios'),
                         (32768, 32768, '32 KiB block, 32 KiB bios'), (32768, 1048576, '32 KiB block, 1 MiB bios')):
        def m(d, st, op):
            v = d.get((st, sec, b, op))
            return statistics.median(v) if v else float('nan')
        fmt = lambda x: ('%.2f' % x) if x < 10 else ('%.1f' % x)
        print('| %s | %s / %s | %s / %s | %.0f / %.0f | %.0f / %.0f |' % (name,
            fmt(m(r, 'eli', 'write')), fmt(m(r, 'kwq', 'write')), fmt(m(r, 'eli', 'read')), fmt(m(r, 'kwq', 'read')),
            m(mb, 'eli', 'write'), m(mb, 'kwq', 'write'), m(mb, 'eli', 'read'), m(mb, 'kwq', 'read')))
        if b == 1048576:
            print('| %s, per 1 MiB bio, ms | %.2f / %.2f | %.2f / %.2f | | |' % (name.split(',')[0],
                m(r, 'eli', 'write') * b / sec / 1000, m(r, 'kwq', 'write') * b / sec / 1000,
                m(r, 'eli', 'read') * b / sec / 1000, m(r, 'kwq', 'read') * b / sec / 1000))
