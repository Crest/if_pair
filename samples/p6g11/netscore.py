#!/usr/bin/env python3
"""netscore.py DIR: nettime.sh's r<rep>.tsv under DIR, medians per stage: NET
beside GELI writes, unpinned (net) and on one CPU (netcpu), and GELI beside it."""
import glob, statistics as st, sys
from collections import defaultdict

v = defaultdict(list)
for p in glob.glob(sys.argv[1] + '/r*.tsv'):
    for l in open(p):
        x = l.rstrip('\n').split('\t')
        if len(x) == 5:
            try:
                v[(x[0], x[2], x[3])].append(float(x[4]))
            except ValueError:
                pass
R = (('dense_gbps', 'NET Gbit/s'), ('dense_pair_min_gbps', 'worst pair'), ('load_mbps', 'GELI MB/s'),
     ('sparse_rtt_p50_us', 'ping p50'), ('sparse_rtt_p99_us', 'ping p99'), ('dense_rtt_p50_us', 'dense p50'),
     ('dense_rtt_p99_us', 'dense p99'), ('kwq_requeued', 'stops'))
stages = []
for k in v:
    if k[0] not in stages:
        stages.append(k[0])
for t in ('net', 'netcpu'):
    print('%s: %s' % (t, ' | '.join(n for _, n in R)))
    for s in stages:
        print('  %-9s %s  (n=%d)' % (s, ' | '.join('%.1f' % st.median(v[(s, t, k)]) if v.get((s, t, k)) else '-' for k, _ in R), len(v.get((s, t, 'dense_gbps'), []))))
