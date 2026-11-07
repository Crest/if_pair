#!/usr/bin/env python3
"""score.py DIR: battery2.sh's TSVs under DIR (stock, p2, p55, nop) scored
against the best median per test; the gate is within 5 % of it.  A row is left
out of the score, and shown as such, when nothing can fail it: every setup
within 1 % of the others and of the drives' own ceiling (nop).  Spread: the
largest (max - min) / median over the setups' repetitions, in percent; a row
whose spread exceeds the gate says less than its numbers.

The probes run one after another beside a load of 2 * PROBE_S + 5 s.  A timer
probe held up past PROBE_LATE_MS leaves the spin and exec probes running after
the load has ended, on an idle machine: their rows and the CPU rows, whose
window then has an idle tail, are shown as "late" and not as results."""
import glob, os, statistics as st, sys
from collections import defaultdict

D = sys.argv[1]
SET = [('stock', 'stock', 'eli'), ('today', 'p2', 'kwqshard'), ('whole', 'p2', 'kwqbio'),
       ('time', 'p2', 'kwqtime'), ('time+rq', 'p2', 'kwqrq'), ('time+rq+buf', 'p2', 'kwqbuf'),
       ('+buf p55', 'p55', 'kwqbuf'), ('+buf+out', 'p2', 'kwqout'), ('+nobucket', 'p2', 'kwqnb'),
       ('defaults', 'p2', 'kwqdef'), ('hold 0', 'p2', 'kwqhold0'), ('hold 1', 'p2', 'kwqhold1')]
PROBE_LATE_MS = 7500	# PROBE_S 3: 0.5 + elapsed + 3 s within the 11 s load
METRICS = ('bulk_mbps', 'small_mbps', 'write_mbps', 'read_mbps', 'load_mbps', 'both_w_mbps', 'both_r_mbps')

def load(grp, stage):
    v = defaultdict(list)
    for p in glob.glob(os.path.join(D, grp, '*.tsv')):
        f = os.path.basename(p).split('_r')[0]
        for l in open(p):
            x = l.rstrip('\n').split('\t')
            if len(x) == 5 and x[0] == stage:
                try:
                    v[(f, x[2], x[3])].append(float(x[4]))
                except ValueError:
                    pass
    return v

C = {t: load(g, s) for t, g, s in SET}
NOP = load('nop', 'nop')
tests = sorted({k for v in C.values() for k in v if k[2] in METRICS})
tested = defaultdict(int); ok = defaultdict(int); worst = defaultdict(float); fails = defaultdict(list)
print('| test | best (MB/s) | ' + ' | '.join(t for t, _, _ in SET) + ' | spread % | nop |')
print('|---|---|' + '---|' * len(SET) + '---|---|')
skipped = []
for k in tests:
    m = {t: st.median(C[t][k]) for t, _, _ in SET if C[t].get(k)}
    if len(m) < 2:
        continue
    bt = max(m, key=m.get); b = m[bt]
    if b <= 0:
        continue
    n = st.median(NOP[k]) if NOP.get(k) else None
    spread = max((max(C[t][k]) - min(C[t][k])) / st.median(C[t][k]) * 100 for t in m if st.median(C[t][k]) > 0)
    flat = min(m.values()) >= b * 0.99 and n is not None and b >= n * 0.99
    print('| %s %s %s%s | %.0f %s | ' % (k[0], k[1], k[2].replace('_mbps', ''), ' (at the ceiling)' if flat else '', b, bt) +
          ' | '.join('%+.1f' % ((m[t] / b - 1) * 100) if t in m else '-' for t, _, _ in SET) +
          ' | %.1f | %s |' % (spread, '%.0f' % n if n is not None else '-'))
    if flat:
        skipped.append(k)
        continue
    for t, v in m.items():
        d = (1 - v / b) * 100
        tested[t] += 1; ok[t] += d <= 5; worst[t] = max(worst[t], d)
        if d > 5:
            fails[t].append((round(d, 1), k[0], k[1], k[2].replace('_mbps', '')))
print()
print('%d rows at the drives\' ceiling left out of the score' % len(skipped))
for t, _, _ in SET:
    print('%-12s within 5 %%: %d of %d, worst %.1f %%; %s' % (t, ok[t], tested[t], worst[t], sorted(fails[t], reverse=True)[:6]))
print()
print('drives: reads beside writes (p50 / p99 us), MB/s, timer p99 us, under writes: timer probe ms, spin share, exec us, busiest / idlest CPU')
for t, _, _ in SET:
    for f in sorted({k[0] for k in C[t] if k[1] == 'dev'}):
        g = lambda m: st.median(C[t][(f, 'dev', m)]) if C[t].get((f, 'dev', m)) else float('nan')
        late = g('write_timer_elapsed_ms') > PROBE_LATE_MS
        probes = 'late' if late else 'spin %.2f  exec %6.0f  cpu %.2f/%.2f' % (g('write_spin_share'), g('write_exec_exec_avg'), g('write_cpu_max'), g('write_cpu_min'))
        print('%-12s %-10s read %7.0f /%8.0f  w %7.0f r %7.0f  timer %7.0f  probe %6.0f  %s  alone %6.0f/%7.0f' % (t, f, g('both_r_lat_p50_us'), g('both_r_lat_p99_us'), g('both_w_mbps'), g('both_r_mbps'), g('both_timer_late_p99'), g('write_timer_elapsed_ms'), probes, g('read_lat_p50_us'), g('read_lat_p99_us')))
