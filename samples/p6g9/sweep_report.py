#!/usr/bin/env python3
"""sweep_report.py DIR NCPU [PRIORITY...]: prio_sweep.sh's medians, a row per priority."""
import glob, os, statistics, sys
from collections import defaultdict

D, NCPU = sys.argv[1], int(sys.argv[2])
PR = [int(x) for x in sys.argv[3:]] or [2, 41, 43, 44, 55]

def load(p, name):
    v = defaultdict(list)
    for fn in sorted(glob.glob(os.path.join(D, 'p%d' % p, name + '_r*.tsv'))):
        for l in open(fn):
            s = l.rstrip('\n').split('\t')
            if len(s) == 5 and s[0] == 'kwqbio':
                try:
                    v[(s[2], s[3])].append(float(s[4]))
                except ValueError:
                    pass
    return v

def med(v, t, m):
    return statistics.median(v[(t, m)]) if v.get((t, m)) else None

def f(x, d=0):
    return '-' if x is None else ('%.' + str(d) + 'f') % x

def spread(v, t, m):
    x = v.get((t, m))
    return (max(x) - min(x)) / statistics.median(x) * 100 if x else None

def permib(v, t, s, m):
    q, y = med(v, t, s), med(v, t, m)
    return q * NCPU * 1e3 / (y / 1.048576) if q and y else None

COL = [('32: writes MB/s', lambda s, t: f(med(s, 'scale32', 'bulk_mbps'))),
       ('128: writes MB/s', lambda s, t: f(med(s, 'scale128', 'bulk_mbps'))),
       ('128: spread %', lambda s, t: f(spread(s, 'scale128', 'bulk_mbps'))),
       ('128: CPU/MiB ms', lambda s, t: f(permib(s, 'scale128', 'bulk_sys', 'bulk_mbps'), 2)),
       ('128: 4K reads k/s', lambda s, t: f((med(s, 'scale128', 'small_iops') or 0) / 1e3)),
       ('128: timer p99 us', lambda s, t: f(med(s, 'scale128', 'bulk_timer_late_p99'))),
       ('128: fork+exec p99 ms', lambda s, t: f((med(s, 'scale128', 'bulk_exec_exec_p99') or 0) / 1e3, 1)),
       ('128: spin share', lambda s, t: f(med(s, 'scale128', 'bulk_spin_share'), 2)),
       ('tput writes MB/s', lambda s, t: f(med(t, 'tput', 'write_mbps'))),
       ('tput reads MB/s', lambda s, t: f(med(t, 'tput', 'read_mbps'))),
       ('reads spread %', lambda s, t: f(spread(t, 'tput', 'read_mbps'))),
       ('read CPU/MiB ms', lambda s, t: f(permib(t, 'tput', 'read_sys', 'read_mbps'), 2))]
print('| priority | ' + ' | '.join(c[0] for c in COL) + ' |')
print('|---|' + '---|' * len(COL))
for p in PR:
    s, t = load(p, 'scale'), load(p, 'tput')
    print('| %d | ' % p + ' | '.join(c[1](s, t) for c in COL) + ' |')
