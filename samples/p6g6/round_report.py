#!/usr/bin/env python3
"""round_report.py DIR NCPU: medians over the repetitions of round.sh's
TSVs (scale, tput, tput32k, ssd) as markdown tables, stock GELI (eli)
against BULK's new defaults (kwqshard) and the 1 ms values (kwqshard1).
Cuts: requeued elements per 1 MiB item handed over, requeued / (items -
requeued), from geli_bench's kwq_*items and kwq_*requeued rows."""
import os, statistics, sys
from collections import defaultdict

D, NCPU = sys.argv[1], int(sys.argv[2])
ST = ['eli', 'kwqshard', 'kwqshard1']
NAME = {'eli': 'stock GELI', 'kwqshard': 'new (5/10 ms)', 'kwqshard1': 'old (1/1 ms)'}

def load(f):
    v = defaultdict(list)
    p = os.path.join(D, f)
    if not os.path.exists(p):
        return None
    for l in open(p):
        s = l.rstrip('\n').split('\t')
        if len(s) != 5:
            continue
        try:
            v[(s[0], s[2], s[3])].append(float(s[4]))
        except ValueError:
            pass
    return v

def med(v, st, t, m):
    x = v.get((st, t, m))
    return statistics.median(x) if x else None

def cut(v, st, t, pfx):
    it, rq = med(v, st, t, 'kwq_%sitems' % pfx), med(v, st, t, 'kwq_%srequeued' % pfx)
    if it is None or rq is None or it - rq <= 0:
        return None
    return rq / (it - rq)

def f(x, d=0):
    if x is None:
        return '-'
    return ('%.' + str(d) + 'f') % x

def row(label, vals):
    print('| %s | %s |' % (label, ' | '.join(vals)))

def triple(get, d=0):
    return ' / '.join(f(get(s), d) for s in ST)

hdr = ' / '.join(NAME[s] for s in ST)
sc = load('scale.tsv')
if sc:
    ns = sorted({int(t[5:]) for (_, t, _) in sc if t.startswith('scale') and t != 'scale0'})
    print('\n### scale (%s)\n' % hdr)
    print('| providers | ' + ' | '.join(str(n) for n in ns) + ' |')
    print('|---|' + '---|' * len(ns))
    def per(fn, d=0):
        return [triple(lambda s, n=n: fn(s, 'scale%d' % n), d) for n in ns]
    row('4 KiB reads/s, k', per(lambda s, t: (med(sc, s, t, 'small_iops') or 0) / 1e3 or None))
    row('CPU per read, us', per(lambda s, t: (lambda q, y: q * NCPU * 1e6 / y if q and y else None)(med(sc, s, t, 'small_sys'), med(sc, s, t, 'small_iops')), 1))
    row('1 MiB writes, MB/s', per(lambda s, t: med(sc, s, t, 'bulk_mbps')))
    row('CPU per MiB written, ms', per(lambda s, t: (lambda q, y: q * NCPU * 1e3 / (y / 1.048576) if q and y else None)(med(sc, s, t, 'bulk_sys'), med(sc, s, t, 'bulk_mbps')), 2))
    row('write p99, ms', per(lambda s, t: (med(sc, s, t, 'bulk_lat_p99_us') or 0) / 1e3 or None, 1))
    row('cuts per 1 MiB write (new / old)', [' / '.join(f(cut(sc, s, 'scale%d' % n, 'bulk_'), 2) for s in ST[1:]) for n in ns])
    for ph, nm in (('bulk', 'writes'), ('small', 'reads')):
        row('beside the %s: spin share' % nm, per(lambda s, t, ph=ph: med(sc, s, t, ph + '_spin_share'), 2))
        row('beside the %s: timer p99, us' % nm, per(lambda s, t, ph=ph: med(sc, s, t, ph + '_timer_late_p99')))
        row('beside the %s: fork+exec p99, ms' % nm, per(lambda s, t, ph=ph: (med(sc, s, t, ph + '_exec_exec_p99') or 0) / 1e3 or None, 1))

for fn, title in (('tput.tsv', 'tput, resp, onecpu, 4 KiB sectors'), ('tput32k.tsv', 'tput, 32 KiB sectors')):
    tp = load(fn)
    if not tp:
        continue
    print('\n### %s (%s)\n' % (title, hdr))
    print('| metric | value |\n|---|---|')
    row('write MB/s', [triple(lambda s: med(tp, s, 'tput', 'write_mbps'))])
    row('write CPU per MiB, ms', [triple(lambda s: (lambda q, y: q * NCPU * 1e3 / (y / 1.048576) if q and y else None)(med(tp, s, 'tput', 'write_sys'), med(tp, s, 'tput', 'write_mbps')), 2)])
    row('read MB/s', [triple(lambda s: med(tp, s, 'tput', 'read_mbps'))])
    row('read CPU per MiB, ms', [triple(lambda s: (lambda q, y: q * NCPU * 1e3 / (y / 1.048576) if q and y else None)(med(tp, s, 'tput', 'read_sys'), med(tp, s, 'tput', 'read_mbps')), 2)])
    row('cuts per 1 MiB write, new / old', [' / '.join(f(cut(tp, s, 'tput', ''), 2) for s in ST[1:])])
    row('cuts per 1 MiB read, new / old', [' / '.join(f(cut(tp, s, 'tput', 'read_'), 2) for s in ST[1:])])
    if fn != 'tput.tsv':
        continue
    for m, nm, d, k in (('load_mbps', 'resp: writers MB/s', 0, 1), ('timer_late_p99', 'resp: timer p99, us', 0, 1),
                        ('timer_pin_late_p99', 'resp: timer pinned p99, us', 0, 1), ('spin_share', 'resp: spin share', 2, 1),
                        ('spin_pin_share', 'resp: spin pinned share', 2, 1), ('exec_exec_p99', 'resp: fork+exec p99, ms', 1, 1e3)):
        row(nm, [triple(lambda s, m=m, k=k: (lambda x: x / k if x is not None else None)(med(tp, s, 'resp', m)), d)])
    for m, nm, d, k in (('timer_mbps', 'onecpu: MB/s beside the timer', 0, 1), ('timer_jain', 'onecpu: Jain beside the timer', 3, 1),
                        ('timer_pin_late_p99', 'onecpu: timer p99, us', 0, 1), ('timer_pin_late_max', 'onecpu: timer max, us', 0, 1),
                        ('spin_mbps', 'onecpu: MB/s beside the spin loop', 0, 1), ('spin_pin_share', 'onecpu: spin share', 2, 1),
                        ('spin_jain', 'onecpu: Jain beside the spin loop', 3, 1)):
        row(nm, [triple(lambda s, m=m, k=k: (lambda x: x / k if x is not None else None)(med(tp, s, 'onecpu', m)), d)])

ss = load('ssd.tsv')
if ss:
    print('\n### ssd model, 16 devices (nop / %s)\n' % hdr)
    print('| load | rate | sys | p99, us | cuts new / old |\n|---|---|---|---|---|')
    for l in ('seqread', 'seqwrite', 'randread'):
        key = l + ('_iops' if l == 'randread' else '_mbps')
        st4 = ['nop'] + ST
        print('| %s | %s | %s | %s | %s |' % (l,
            ' / '.join(f(med(ss, s, 'ssd', key)) for s in st4),
            ' / '.join(f(med(ss, s, 'ssd', l + '_sys'), 3) for s in st4),
            ' / '.join(f(med(ss, s, 'ssd', l + '_lat_p99_us')) for s in st4),
            ' / '.join(f(cut(ss, s, 'ssd', l + '_'), 2) for s in ST[1:])))
