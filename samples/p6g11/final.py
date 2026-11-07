#!/usr/bin/env python3
# final.py DIR: score.py's table read back as numbers: per row stock and the
# defaults against each other, and the gate with and without hold 1.
import re, subprocess, sys
out = subprocess.run(['python3', 'score.py', sys.argv[1]], capture_output=True, text=True).stdout
hdr = None; rows = []
for l in out.splitlines():
    if not l.startswith('|'):
        continue
    c = [x.strip() for x in l.strip('|').split('|')]
    if c[0] == 'test':
        hdr = c; continue
    if set(c[0]) <= set('-') or 'at the ceiling' in c[0]:
        continue
    best = float(c[1].split()[0])
    g = lambda n: float(c[hdr.index(n)])
    rows.append((c[0], best, c[1].split(None, 1)[1], g('stock'), g('defaults'), g('hold 0'), g('hold 1')))
print(f'{len(rows)} rows')
v = lambda b, p: b * (1 + p / 100)
beat = sum(1 for r in rows if v(r[1], r[4]) >= v(r[1], r[3]))
print('defaults >= stock:', beat, 'of', len(rows))
worst = min(rows, key=lambda r: (v(r[1], r[4]) / v(r[1], r[3])))
print('smallest defaults/stock: %s %.3f' % (worst[0], v(worst[1], worst[4]) / v(worst[1], worst[3])))
for r in sorted(rows, key=lambda r: v(r[1], r[4]) / v(r[1], r[3]))[:5]:
    print('   %-26s stock %8.0f defaults %8.0f  x%.3f' % (r[0], v(r[1], r[3]), v(r[1], r[4]), v(r[1], r[4]) / v(r[1], r[3])))
print('gate, all candidates: defaults within 5 %%: %d of %d' % (sum(1 for r in rows if r[4] >= -5), len(rows)), [r[0] for r in rows if r[4] < -5])
n = 0; miss = []
for r in rows:
    b = max(v(r[1], r[3]), v(r[1], r[4]), v(r[1], r[5]))
    if v(r[1], r[4]) >= 0.95 * b: n += 1
    else: miss.append(r[0])
print('gate, without hold 1: defaults within 5 %%: %d of %d' % (n, len(rows)), miss)
print('stock within 5 %% of best: %d' % sum(1 for r in rows if r[3] >= -5))
for name in ('scale scale16 bulk', 'scale scale16 small', 'tput tput write', 'tput tput read', 'storage dev write', 'storage dev read', 'storage dev both_w', 'storage dev both_r', 'home dev both_w', 'home dev both_r', 'compute dev write', 'compute dev both_w', 'compute dev both_r'):
    for r in rows:
        if r[0] == name:
            print('%-24s stock %8.0f defaults %8.0f hold0 %8.0f hold1 %8.0f' % (r[0], v(r[1], r[3]), v(r[1], r[4]), v(r[1], r[5]), v(r[1], r[6])))
