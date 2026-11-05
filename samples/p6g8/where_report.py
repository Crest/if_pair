#!/usr/bin/env python3
"""where_report.py DIR: where.d's profiles (DIR/where_<tag>.out) and
geli_bench's rows (DIR/scale_<tag>.tsv) for the tags eli, kwqshard,
nocap: CPU time per MiB written in the bulk phases by thread kind, and
the kernel functions whose time per MiB differs most from stock GELI's."""
import os, sys
from collections import defaultdict

D = sys.argv[1]
TAGS = ['eli', 'kwqshard', 'nocap']
NAME = {'eli': 'stock GELI', 'kwqshard': 'kwq-geli', 'nocap': 'kwq-geli, no share limit'}
HZ = 997.0
P = {}
for t in TAGS:
    f = os.path.join(D, 'where_%s.out' % t)
    if not os.path.exists(f):
        continue
    d = {'dur': {}, 'cls': defaultdict(int), 'fn': defaultdict(int), 'kwq': defaultdict(int), 'iops': {}, 'mbps': {}}
    for l in open(f):
        s = l.rstrip('\n').split('\t')
        if s[0] == 'dur_ms':
            d['dur'][s[1]] = int(s[2]) / 1000.0
        elif s[0] == 'cls':
            d['cls'][(s[1], s[2])] += int(s[3])
        elif s[0] == 'fn':
            fn = s[3].split('`')[-1]
            d['fn'][(s[1], s[2], fn)] += int(s[4])
        elif s[0] in ('passes', 'items', 'requeued'):
            d['kwq'][(s[0], s[1])] = int(s[2])
    for l in open(os.path.join(D, 'scale_%s.tsv' % t)):
        s = l.rstrip('\n').split('\t')
        if len(s) == 5 and s[3] in ('bulk_iops', 'bulk_mbps'):
            d['iops' if s[3] == 'bulk_iops' else 'mbps'][s[2][5:] + ' bulk'] = float(s[4])
    P[t] = d

def mib(d, ph):
    return d['iops'].get(ph, 0) * d['dur'].get(ph, 0)

def ms_per_mib(n, d, ph):
    m = mib(d, ph)
    return n / HZ * 1000 / m if m else float('nan')

tags = [t for t in TAGS if t in P]
for ph in ('16 bulk', '32 bulk', '128 bulk'):
    print('\n### %s providers, 1 MiB writes: CPU ms per MiB written (%s)\n' % (ph.split()[0], ' / '.join(NAME[t] for t in tags)))
    print('| | ' + ' | '.join(NAME[t] for t in tags) + ' |')
    print('|---|' + '---|' * len(tags))
    print('| MB/s | ' + ' | '.join('%.0f' % P[t]['mbps'].get(ph, 0) for t in tags) + ' |')
    kinds = sorted({k for t in tags for (p, k) in P[t]['cls'] if p == ph},
                   key=lambda k: -max(P[t]['cls'].get((ph, k), 0) for t in tags))
    tot = {t: 0.0 for t in tags}
    for k in kinds:
        vals = [ms_per_mib(P[t]['cls'].get((ph, k), 0), P[t], ph) for t in tags]
        if max(vals) < 0.005:
            continue
        if k != 'idle':
            for t, v in zip(tags, vals):
                tot[t] += v
        print('| %s | %s |' % (k, ' | '.join('%.3f' % v for v in vals)))
    print('| **busy (all but idle)** | %s |' % ' | '.join('**%.3f**' % tot[t] for t in tags))
    for t in tags:
        if t != 'eli' and ('items', ph) in P[t]['kwq']:
            it, rq = P[t]['kwq'][('items', ph)], P[t]['kwq'][('requeued', ph)]
            print('| %s: passes, cuts per bio | %d, %.2f |' % (NAME[t], P[t]['kwq'][('passes', ph)], rq / max(it - rq, 1)))
for ph in ('32 bulk', '128 bulk'):
    for t in tags:
        if t == 'eli':
            continue
        fa = defaultdict(float); fb = defaultdict(float)
        for (p, k, fn), n in P['eli']['fn'].items():
            if p == ph:
                fa[fn] += ms_per_mib(n, P['eli'], ph)
        for (p, k, fn), n in P[t]['fn'].items():
            if p == ph:
                fb[fn] += ms_per_mib(n, P[t], ph)
        diff = sorted(set(fa) | set(fb), key=lambda f: -abs(fb[f] - fa[f]))[:18]
        print('\n### %s providers: kernel functions, CPU ms per MiB, stock GELI / %s (all threads)\n' % (ph.split()[0], NAME[t]))
        print('| function | stock GELI | %s | difference |\n|---|---|---|---|' % NAME[t])
        for f in diff:
            print('| `%s` | %.3f | %.3f | %+.3f |' % (f, fa[f], fb[f], fb[f] - fa[f]))
        print('| all kernel samples | %.3f | %.3f | %+.3f |' % (sum(fa.values()), sum(fb.values()), sum(fb.values()) - sum(fa.values())))
