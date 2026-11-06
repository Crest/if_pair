#!/usr/bin/env python3
"""summarize.py [RUNS.jsonl]: medians over the seeds of run.py's runs, as text."""
import json, statistics as st, sys
from collections import defaultdict
P = sys.argv[1] if len(sys.argv) > 1 else 'out/runs.jsonl'
R = [json.loads(l) for l in open(P)]
err = [r for r in R if 'error' in r]
print(len(R), 'runs', len(err), 'errors')
for e in err[:5]: print(' ', e['exp'], e['hw'], e['var'], e['n'], e['error'][:120])
G = defaultdict(list)
for r in R:
    if 'm' in r: G[(r['exp'], r['hw'], r['var'], r['n'], json.dumps(r.get('sens')))].append(r['m'])
def med(k, m): return st.median(x[m] for x in G[k])
def rng(k, m):
    v = [x[m] for x in G[k]]; return min(v), max(v)
M = 'mbps cuts_bio partials_mean partials_max starts_over gap_p50_us gap_p99_us crypto_p50_us crypto_p99_us e2e_p50_us e2e_p99_us late_p99_us late_max_us jain bulk_cpu overruns small_p50_us small_p99_us rtt_p50_us rtt_p99_us rtt_max_us'.split()
def show(exp, hw=None):
    print('==', exp, hw or '')
    keys = sorted({k for k in G if k[0] == exp and (hw is None or k[1] == hw) and k[4] == 'null'}, key=lambda k: (k[1], k[3], k[2]))
    for k in keys:
        print(f'{k[1]:8s} n={k[3]:<4d} {k[2]:22s}', ' '.join(f"{m.replace('_us','').replace('partials','pt')}={med(k,m):.5g}" for m in M if med(k,m) != 0 or m in ('mbps',)))
for e in sys.argv[2:] or ['scale', 'tsweep', 'hw', 'sec32k', 'ping', 'reads', 'fair', 'mixed']:
    show(e)
