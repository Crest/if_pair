#!/usr/bin/env python3
"""steps_report.py DIR NCPU: medians over the repetitions of steps.sh's TSVs
in DIR (stock, p2, p55) as markdown tables, one column or row per setup:
stock GELI, kwq-geli as it is, budget_sectors 0, priority 55, both.  Cuts:
requeued elements per 1 MiB item handed over, requeued / (items - requeued).
Spread: (max - min) / median of the repetitions."""
import glob, os, statistics, sys
from collections import defaultdict

D, NCPU = sys.argv[1], int(sys.argv[2])
SET = [('stock', 'stock', 'eli'), ('base', 'p2', 'kwqshard'), ('bio', 'p2', 'kwqbio'),
       ('prio', 'p55', 'kwqshard'), ('both', 'p55', 'kwqbio')]

def load(name):
    v = defaultdict(lambda: defaultdict(list))
    for tag, grp, st in SET:
        for p in sorted(glob.glob(os.path.join(D, grp, name + '_r*.tsv'))):
            for l in open(p):
                s = l.rstrip('\n').split('\t')
                if len(s) != 5 or s[0] != st:
                    continue
                try:
                    v[tag][(s[2], s[3])].append(float(s[4]))
                except ValueError:
                    pass
    return v if v else None

def med(v, tag, t, m):
    x = v[tag].get((t, m))
    return statistics.median(x) if x else None

def spread(v, tag, t, m):
    x = v[tag].get((t, m))
    return (max(x) - min(x)) / statistics.median(x) * 100 if x and statistics.median(x) else None

def cut(v, tag, t, pfx):
    it, rq = med(v, tag, t, 'kwq_%sitems' % pfx), med(v, tag, t, 'kwq_%srequeued' % pfx)
    return rq / (it - rq) if it is not None and rq is not None and it - rq > 0 else None

def f(x, d=0):
    return '-' if x is None else ('%.' + str(d) + 'f') % x

def permib(v, tag, t, sysm, mbps):
    q, y = med(v, tag, t, sysm), med(v, tag, t, mbps)
    return q * NCPU * 1e3 / (y / 1.048576) if q and y else None

def scaled(k):
    return lambda v, tag, t, m: (lambda x: x / k if x is not None else None)(med(v, tag, t, m))

def table(title, cols, rows):
    print('\n### %s\n' % title)
    print('| | ' + ' | '.join(cols) + ' |')
    print('|---|' + '---|' * len(cols))
    for r in rows:
        print('| ' + ' | '.join(r) + ' |')

sc = load('scale')
if sc:
    ns = sorted({int(t[5:]) for tag in sc for (t, _) in sc[tag] if t.startswith('scale') and t != 'scale0'})
    def per(title, fn, d=0, tags=None):
        table(title, [str(n) for n in ns], [[tag] + [f(fn(tag, 'scale%d' % n), d) for n in ns]
              for tag in (tags or [s[0] for s in SET])])
    per('scale: 1 MiB writes, MB/s', lambda g, t: med(sc, g, t, 'bulk_mbps'))
    per('scale: spread of the writes, %', lambda g, t: spread(sc, g, t, 'bulk_mbps'))
    per('scale: CPU per MiB written, ms', lambda g, t: permib(sc, g, t, 'bulk_sys', 'bulk_mbps'), 2)
    per('scale: write p99, ms', lambda g, t: scaled(1e3)(sc, g, t, 'bulk_lat_p99_us'), 1)
    per('scale: cuts per 1 MiB write', lambda g, t: cut(sc, g, t, 'bulk_'), 2, ['base', 'bio', 'prio', 'both'])
    per('scale: passes over budget during the writes', lambda g, t: med(sc, g, t, 'kwq_bulk_overruns'), 0, ['base', 'bio', 'prio', 'both'])
    per('scale: 4 KiB reads/s, k', lambda g, t: scaled(1e3)(sc, g, t, 'small_iops'))
    per('scale: CPU per read, us', lambda g, t: (lambda q, y: q * NCPU * 1e6 / y if q and y else None)(med(sc, g, t, 'small_sys'), med(sc, g, t, 'small_iops')), 1)
    for ph, nm in (('bulk', 'writes'), ('small', 'reads')):
        per('beside the %s: spin share' % nm, lambda g, t, ph=ph: med(sc, g, t, ph + '_spin_share'), 2)
        per('beside the %s: timer p99, us' % nm, lambda g, t, ph=ph: med(sc, g, t, ph + '_timer_late_p99'))
        per('beside the %s: fork+exec p99, ms' % nm, lambda g, t, ph=ph: scaled(1e3)(sc, g, t, ph + '_exec_exec_p99'), 1)

for fn, title in (('tput', 'tput, resp, onecpu, 4 KiB sectors'), ('tput32k', 'tput, 32 KiB sectors')):
    tp = load(fn)
    if not tp:
        continue
    cols = [s[0] for s in SET]
    rows = []
    def add(nm, get, d=0):
        rows.append([nm] + [f(get(g), d) for g in cols])
    add('write MB/s', lambda g: med(tp, g, 'tput', 'write_mbps'))
    add('write spread, %', lambda g: spread(tp, g, 'tput', 'write_mbps'))
    add('write CPU per MiB, ms', lambda g: permib(tp, g, 'tput', 'write_sys', 'write_mbps'), 2)
    add('read MB/s', lambda g: med(tp, g, 'tput', 'read_mbps'))
    add('read spread, %', lambda g: spread(tp, g, 'tput', 'read_mbps'))
    add('read CPU per MiB, ms', lambda g: permib(tp, g, 'tput', 'read_sys', 'read_mbps'), 2)
    add('cuts per 1 MiB write', lambda g: cut(tp, g, 'tput', ''), 2)
    add('cuts per 1 MiB read', lambda g: cut(tp, g, 'tput', 'read_'), 2)
    if fn == 'tput':
        for m, nm, d, k in (('load_mbps', 'resp: writers MB/s', 0, 1), ('timer_late_p99', 'resp: timer p99, us', 0, 1),
                            ('timer_pin_late_p99', 'resp: timer pinned p99, us', 0, 1), ('spin_share', 'resp: spin share', 2, 1),
                            ('spin_pin_share', 'resp: spin pinned share', 2, 1), ('exec_exec_p99', 'resp: fork+exec p99, ms', 1, 1e3)):
            add(nm, lambda g, m=m, k=k: scaled(k)(tp, g, 'resp', m), d)
        for m, nm, d in (('timer_mbps', 'onecpu: MB/s beside the timer', 0), ('timer_jain', 'onecpu: Jain beside the timer', 3),
                         ('timer_pin_late_p99', 'onecpu: timer p99, us', 0), ('timer_pin_late_max', 'onecpu: timer max, us', 0),
                         ('spin_mbps', 'onecpu: MB/s beside the spin loop', 0), ('spin_pin_share', 'onecpu: spin share', 2),
                         ('spin_jain', 'onecpu: Jain beside the spin loop', 3)):
            add(nm, lambda g, m=m: med(tp, g, 'onecpu', m), d)
    table(title, cols, rows)

ss = load('ssd')
if ss:
    cols = [s[0] for s in SET]
    rows = []
    for l in ('seqread', 'seqwrite', 'randread'):
        key = l + ('_iops' if l == 'randread' else '_mbps')
        rows.append([l + ' rate'] + [f(med(ss, g, 'ssd', key)) for g in cols])
        rows.append([l + ' sys'] + [f(med(ss, g, 'ssd', l + '_sys'), 3) for g in cols])
        rows.append([l + ' p99, us'] + [f(med(ss, g, 'ssd', l + '_lat_p99_us')) for g in cols])
    table('ssd model, 16 devices', cols, rows)
