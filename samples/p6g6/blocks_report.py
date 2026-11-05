#!/usr/bin/env python3
"""blocks_report.py FILE...: blocks.sh's rows as one table per machine,
the median over the repetitions of the time per GELI block (sector) and
per request on one busy CPU: window / items, per sector / (bytes/sector)."""
import statistics, sys
from collections import defaultdict

CFG = [(4096, 4096, '4 KiB block'), (32768, 32768, '32 KiB block'),
       (1048576, 4096, '1 MiB bio of 4 KiB blocks'), (1048576, 32768, '1 MiB bio of 32 KiB blocks'),
       (1048576, 1048576, '1 MiB as one block')]
for f in sys.argv[1:]:
    r = defaultdict(list); drv = set(); cpus = set(); err = 0
    for l in open(f):
        s = l.split()
        if len(s) < 11:
            continue
        rep, b, sec, rk, dec, items, win = map(int, s[:7])
        drv.add(s[8]); cpus.add(s[9]); err += int(s[10])
        if items > 0:
            r[(b, sec, rk, dec)].append(win * 1000.0 / items)
    print('\n### %s (driver %s, CPUs used %s, errors %d)\n' % (f, ','.join(sorted(drv)), ','.join(sorted(cpus)), err))
    print('| | encrypt, per block | decrypt, per block | per request (enc / dec) | MB/s (enc / dec) | key setup per block (enc / dec) |')
    print('|---|---|---|---|---|---|')
    def us(x):
        return '%.2f us' % x if x < 10 else ('%.1f us' % x if x < 100 else '%.0f us' % x)
    for b, sec, name in CFG:
        m = {}
        for rk in (1, 0):
            for dec in (0, 1):
                v = r.get((b, sec, rk, dec))
                m[(rk, dec)] = statistics.median(v) if v else None
        if m[(1, 0)] is None:
            continue
        n = b // sec
        enc, de = m[(1, 0)] / n, m[(1, 1)] / n
        ks = ' / '.join(us((m[(1, d)] - m[(0, d)]) / n) if m[(0, d)] else '-' for d in (0, 1))
        print('| %s | %s | %s | %s / %s | %.0f / %.0f | %s |' % (name, us(enc), us(de), us(m[(1, 0)]), us(m[(1, 1)]),
            b / m[(1, 0)], b / m[(1, 1)], ks))
