#!/usr/bin/env python3
# sweepscore.py DIR: home_sweep.sh's rows, the median of the repetitions per
# stage and queue depth.  busy is write/900 + read/1040: the share of the two
# drives' time those rates take, at the rates measured alone.  r_ms is a read's
# latency at p50, and little the same from Little's law on the reads, RQD 2 of
# 128 KiB on each of 2 drives, as a check.  ahead is how many writes r_ms makes
# at 1 MiB and 450 MB/s each.
import glob, os, statistics, sys

def rows(path):
    d = {}
    for l in open(path):
        f = l.rstrip('\n').split('\t')
        if len(f) >= 5 and f[2] == 'dev':
            d[f[3]] = f[4]
    return d

base = sys.argv[1]
print('stage  qd  both_w  both_r  total   busy   r_ms  little  ahead   w_ms  holds')
for st in sorted(os.listdir(base)):
    qds = sorted({int(os.path.basename(p).split('_')[0][1:]) for p in glob.glob(f'{base}/{st}/q*_r*.tsv')})
    for q in qds:
        w, r, h, rl, wl = [], [], [], [], []
        for p in glob.glob(f'{base}/{st}/q{q}_r*.tsv'):
            d = rows(p)
            if 'both_w_mbps' in d and 'both_r_mbps' in d:
                w.append(float(d['both_w_mbps'])); r.append(float(d['both_r_mbps']))
                h.append(float(d.get('both_write_holds', 0)))
                rl.append(float(d['both_r_lat_p50_us']) / 1e3)
                wl.append(float(d['both_w_lat_p50_us']) / 1e3)
        if not w:
            continue
        mw, mr = statistics.median(w), statistics.median(r)
        little = 2 * 2 * 131072 / (mr * 1e6) * 1e3
        svc = 1048576 / 450e6 * 1e3
        rm, wm = statistics.median(rl), statistics.median(wl)
        print(f'{st:6} {q:2} {mw:7.0f} {mr:7.1f} {mw + mr:6.0f} {mw / 900 + mr / 1040:6.3f} {rm:6.2f} {little:7.2f} {rm / svc:6.2f} {wm:6.2f} {statistics.median(h):6.0f}  n={len(w)}')
