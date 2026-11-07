#!/usr/bin/env python3
"""report.py DIR: battery.sh's TSVs in DIR (t1, t2) as markdown tables, the
median over the repetitions; spread = (max - min) / median, in percent."""
import glob, os, statistics, sys
from collections import defaultdict

D = sys.argv[1]

def load(sub):
    v = defaultdict(list)
    for p in sorted(glob.glob(os.path.join(D, sub, '*.tsv'))):
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

def spread(v, st, t, m):
    x = v.get((st, t, m))
    return (max(x) - min(x)) / statistics.median(x) * 100 if x and statistics.median(x) else None

def f(x, d=0):
    return '-' if x is None else ('%.' + str(d) + 'f') % x

def table(head, rows):
    print('| ' + ' | '.join(head) + ' |')
    print('|' + '---|' * len(head))
    for r in rows:
        print('| ' + ' | '.join(r) + ' |')
    print()

t1 = load('t1')
if t1:
    print('## t1: budget_check_every, budget_sectors 0; kwqbio K=8, kwqbio1 K=1\n')
    K = [('K=8', 'kwqbio'), ('K=1', 'kwqbio1')]
    for t in ('deep', 'deepcpu'):
        rows = []
        for name, st in K:
            c, r = med(t1, st, t, 'kwq_small_budget_calls'), med(t1, st, t, 'kwq_small_budget_reads')
            it = med(t1, st, t, 'kwq_small_items')
            rows.append([name, f(med(t1, st, t, 'small_iops')), f(spread(t1, st, t, 'small_iops'), 1),
                         f(med(t1, st, t, 'small_lat_p50_us'), 1), f(med(t1, st, t, 'small_lat_p99_us'), 1),
                         f(med(t1, st, t, 'small_sys'), 4), f(c / it if c is not None and it else None, 2),
                         f(r / c if r is not None and c else None, 3)])
        print(t)
        table(['', 'reads/s', 'spread %', 'p50 us', 'p99 us', 'sys', 'checks/bio', 'clock reads/check'], rows)
        # paired by repetition: the run order alternates
        a, b = t1.get(('kwqbio', t, 'small_iops'), []), t1.get(('kwqbio1', t, 'small_iops'), [])
        if a and len(a) == len(b):
            d = [(y - x) / x * 100 for x, y in zip(a, b)]
            print('K=1 against K=8 per repetition, %: ' + ' '.join('%+.2f' % x for x in d) +
                  '; median %+.2f\n' % statistics.median(d))
    rows = []
    for name, st in K:
        c, r = med(t1, st, 'mixed', 'kwq_budget_calls'), med(t1, st, 'mixed', 'kwq_budget_reads')
        rows.append([name] + [f(med(t1, st, 'mixed', 'timer_pin_' + m), 0) for m in ('late_avg', 'late_p99', 'late_max')] +
                    [f(med(t1, st, 'mixed', 'bulk_mbps')), f(med(t1, st, 'mixed', 'small_iops')),
                     f(med(t1, st, 'mixed', 'kwq_overruns')), f(r / c if r is not None and c else None, 3)])
    print('mixed')
    table(['', 'timer avg us', 'timer p99 us', 'timer max us', 'write MB/s', 'reads/s', 'overruns', 'clock reads/check'], rows)

t2 = load('t2')
if t2:
    print('## t2: kpair traffic beside GELI writes\n')
    S = [('no GELI', 'none'), ('stock', 'eli'), ('sectors 16', 'kwqshard'), ('sectors 0', 'kwqbio')]
    for t in ('net', 'netcpu'):
        print(t, 'sparse')
        table(['', 'RTT p50 us', 'p99', 'p99.9', 'max', 'NET wait max us', 'GELI MB/s'],
              [[n, f(med(t2, s, t, 'sparse_rtt_p50_us')), f(med(t2, s, t, 'sparse_rtt_p99_us')),
                f(med(t2, s, t, 'sparse_rtt_p999_us')), f(med(t2, s, t, 'sparse_rtt_max_us')),
                f(med(t2, s, t, 'sparse_maxlat_us')), f(med(t2, s, t, 'load_mbps'))] for n, s in S])
        print(t, 'dense')
        table(['', 'Gbit/s', 'spread %', 'slowest pair', 'RTT p50 us', 'p99', 'p99.9', 'max', 'NET wait max us',
               'NET yields/s', 'BULK hand-backs/s', 'on PCPU %'],
              [[n, f(med(t2, s, t, 'dense_gbps'), 1), f(spread(t2, s, t, 'dense_gbps'), 1),
                f(med(t2, s, t, 'dense_pair_min_gbps'), 1), f(med(t2, s, t, 'dense_rtt_p50_us')),
                f(med(t2, s, t, 'dense_rtt_p99_us')), f(med(t2, s, t, 'dense_rtt_p999_us')),
                f(med(t2, s, t, 'dense_rtt_max_us')), f(med(t2, s, t, 'dense_maxlat_us')),
                f(med(t2, s, t, 'dense_net_yields_s')), f(med(t2, s, t, 'dense_bulk_handbacks_s')),
                f(med(t2, s, t, 'dense_items_pcpu_pct'), 1)] for n, s in S])
