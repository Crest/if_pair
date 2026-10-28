#!/usr/bin/env python3
"""kwq_rows_report.py OUT [OUT...]: medians over repetitions per stage of the
rows kwq_rows.sh wrote, with the lower bound's predictions beside the
measured values where the stage declared a bound (L=us in its stage line;
kwq/tests/SPEC-p4e.md S2.1, S2.3, S2.4): passes/s against lambda/(1+lambda L),
items per pass against 1+lambda L, where lambda is the measured item rate."""
import re
import sys
import statistics as st
from collections import defaultdict

rows = defaultdict(lambda: defaultdict(list))   # (stage, tag) -> metric -> values
stage_L = {}
stages = []
hdr = re.compile(r'=== rep (\d+) stage (\S+) \((\S+)\)(?: L=(\d+))? knobs:')
line_re = re.compile(r'\[(\S+) r(\d+)(?: (\S+))?\] (.*)')


def add(stage, tag, k, v):
    rows[(stage, tag)][k].append(float(v))


for path in sys.argv[1:]:
    for line in open(path):
        m = hdr.match(line)
        if m:
            stage = m.group(2)
            if stage not in stages:
                stages.append(stage)
            stage_L[stage] = float(m.group(4) or 0)
            continue
        m = line_re.match(line.rstrip())
        if not m:
            continue
        stage, rep, tag, body = m.groups()
        if stage not in stages:
            stages.append(stage)
        tag = tag or ''
        if body.startswith('counters'):
            kv = dict(x.split('=', 1) for x in body.split()[1:] if '=' in x)
            for k, v in kv.items():
                try:
                    add(stage, tag, k, v)
                except ValueError:
                    pass
            ep = float(kv.get('deferred', 0)) + float(kv.get('low', 0))
            ns = float(kv.get('defer_ns', 0)) + float(kv.get('low_ns', 0))
            add(stage, tag, 'ep_us', ns / ep / 1000 if ep else 0)
            continue
        # a run_p1b result line: "name state in=.. out=.. ... ns=.. msg"
        mm = re.match(r'(\w+)\s+(\w+)\s+in=(\d+) out=(\d+).*?ns/item=(\d+) prod=(\d+) ns=(\d+)', body)
        if mm:
            name, state, _in, out, nsi, prod, ns = mm.groups()
            if tag == '' and name in ('cost', 'pingpong'):
                tag = name
            add(stage, tag, 'out', out)
            add(stage, tag, 'ns_item', nsi)
            add(stage, tag, 'prod_ns', prod)
            add(stage, tag, 'run_ns', ns)
            add(stage, tag, 'items_s_M', float(out) / float(ns) * 1e3 if float(ns) else 0)
            add(stage, tag, 'fail', 0 if state == 'done' else 1)
            m2 = re.search(r'(\d+) pkt/s .*enqueue (\d+) ns = (\d+)% of sender CPU, (\d+) over 2 us; workers (\d+)% in packet work, (\d+) passes; queue latency mean (\d+) max (\d+) us', body)
            if m2:
                for k, v in zip(('pkt_s_M', 'enq_ns', 'enq_pct', 'enq_slow', 'work_pct', 'passes', 'lat_mean_us', 'lat_max_us'), m2.groups()):
                    add(stage, tag, k, int(v) / 1e6 if k == 'pkt_s_M' else int(v))
            m3 = re.search(r'batch 1: (\d+) items/s', body)
            if m3:
                add(stage, tag, 'items_s_M', int(m3.group(1)) / 1e6)
            continue
        if body.strip().startswith('burst:'):
            for x in body.split()[1:]:
                if '=' in x:
                    k, v = x.split('=', 1)
                    try:
                        add(stage, tag, k, v)
                    except ValueError:
                        pass
            continue
        m4 = re.search(r'consumer busy ns per item: (\d+).*items per pass (\d+), switched passes (\d+)', body)
        if m4:
            t = tag or 'cost'
            add(stage, t, 'busy_ns_item', m4.group(1))
            add(stage, t, 'items_per_pass_run', m4.group(2))
            add(stage, t, 'switched_run', m4.group(3))
            continue
        m5 = re.search(r'round trip (\d+) ns, producer in the enqueue (\d+) ns, enqueue-to-handler avg (\d+) ns max (\d+) us', body)
        if m5:
            for k, v in zip(('rtt_ns', 'enq_ns', 'e2h_ns', 'e2h_max_us'), m5.groups()):
                add(stage, 'pingpong', k, v)


def med(s, t, k):
    v = rows.get((s, t), {}).get(k)
    return st.median(v) if v else None


def fmt(x, w=0):
    if x is None:
        return '-'.rjust(w)
    if isinstance(x, float) and abs(x - round(x)) > 1e-9:
        return (f'{x:.2f}' if abs(x) < 100 else f'{x:.0f}').rjust(w)
    return f'{int(round(x))}'.rjust(w)


def tags(prefix):
    seen = []
    for (s, t) in rows:
        if t.startswith(prefix) and t not in seen:
            seen.append(t)
    return seen


def table(title, tlist, cols, header):
    if not tlist:
        return
    print()
    print(title)
    print(f"{'stage':8}" + ''.join(f'| {t:^{len(header)}}' for t in tlist))
    print(f"{'':8}" + ''.join(f'| {header}' for _ in tlist))
    for s in stages:
        print(f'{s:8}' + ''.join('| ' + cols(s, t) for t in tlist))


# fan-in
table('fan-in: items/s M | items per pass | wakeups | ticks | idle ms',
      sorted(tags('fanin'), key=lambda t: int(t[5:])),
      lambda s, t: f"{fmt(med(s,t,'items_s_M'),6)} {fmt(med(s,t,'items_per_pass'),6)} {fmt(med(s,t,'wakeups'),8)} {fmt(med(s,t,'ticks'),7)} {fmt(med(s,t,'idle_ms'),5)}",
      'items/s  ipp   wakeups   ticks  idle')
# cost / pingpong / coloc
if any((s, t) in rows for s in stages for t in ('cost', 'pingpong', 'coloc')):
    print()
    print('cost (cross-CPU, then same-CPU half): busy ns/item | ipp | switched || pingpong: rtt ns | enqueue ns | e2h ns || coloc (producer prio 100 on W): wall ns/item | busy ns/item | ipp | wakeups | episodes(mean us) | self')
    for s in stages:
        print(f"{s:8}| {fmt(med(s,'cost','busy_ns_item'),5)} {fmt(med(s,'cost','items_per_pass_run'),5)} {fmt(med(s,'cost','switched_run'),5)} || {fmt(med(s,'pingpong','rtt_ns'),7)} {fmt(med(s,'pingpong','enq_ns'),6)} {fmt(med(s,'pingpong','e2h_ns'),7)} || {fmt(med(s,'coloc','ns_item'),5)} {fmt(med(s,'coloc','busy_ns_item'),5)} {fmt(med(s,'coloc','items_per_pass_run'),5)} {fmt(med(s,'coloc','wakeups'),8)} {fmt(med(s,'coloc','low'),7)}({fmt(med(s,'coloc','ep_us'),5)}) {fmt(med(s,'coloc','self'),6)}")
# ifpair shapes
table('ifpair shapes (N senders at 1000/1500 ns; S 3000/500; X 10000/500): Mpkt/s | enqueue ns (% sender CPU) | ipp | wakeups | ticks | queue latency mean/max us',
      [t for t in ('ifpair1', 'ifpair2', 'ifpairS1', 'ifpairS2', 'ifpairX1', 'ifpairX2') if any((s, t) in rows for s in stages)],
      lambda s, t: f"{fmt(med(s,t,'pkt_s_M'),5)} {fmt(med(s,t,'enq_ns'),5)}({fmt(med(s,t,'enq_pct'),2)}%) {fmt(med(s,t,'items_per_pass'),5)} {fmt(med(s,t,'wakeups'),7)} {fmt(med(s,t,'ticks'),6)} {fmt(med(s,t,'lat_mean_us'),4)}/{fmt(med(s,t,'lat_max_us'),5)}",
      'Mpkt  enq(pct)  ipp  wakeups  ticks  lat')
# burst: predictions per stage L
bt = sorted(tags('burst_'), key=lambda t: (int(t.split('_')[1]), int(t.split('_')[2]), t.split('_')[3]))
if bt:
    print()
    print('burst (B items, gap G us, placement): first-item p50/p99/max us | last-item p50/p99/max us | wakes/burst | ticks/burst | empty/burst | passes/burst | idle ms')
    print('predictions with the stage L (SPEC 2.1): isolated items (G > L, B = 1): ticks 0, wakes 1; dense bursts: wakes 1, ticks ~ ceil(duration/L), empty <= 1, last-item <= L + pass')
    for t in bt:
        print(f'  {t}')
        for s in stages:
            nb = med(s, t, 'bursts') or 0
            per = lambda k: (med(s, t, k) / nb) if nb and med(s, t, k) is not None else None
            print(f"    {s:8}| {fmt(med(s,t,'first_p50_us'),5)}/{fmt(med(s,t,'first_p99_us'),5)}/{fmt(med(s,t,'first_max_us'),6)} | {fmt(med(s,t,'last_p50_us'),5)}/{fmt(med(s,t,'last_p99_us'),5)}/{fmt(med(s,t,'last_max_us'),6)} | {fmt(per('wakeups'),6)} {fmt(per('ticks'),6)} {fmt(per('ticks_empty'),6)} {fmt(per('passes'),7)} | {fmt(med(s,t,'idle_ms'),5)}  (L={stage_L.get(s,0):.0f})")
# sparse senders: predicted wakes+ticks = lambda/(1+lambda L), ipp = 1+lambda L
spt = sorted(tags('sparse_'), key=lambda t: (int(t.split('_')[1]), int(t.split('_')[2])))
if spt:
    print()
    print('sparse senders (fanin, gap us): Mpkt/s | enqueue ns (% sender CPU) | wakes+ticks/s (predicted) | ipp (predicted) | queue latency mean/max us')
    for t in spt:
        print(f'  {t}')
        for s in stages:
            L = stage_L.get(s, 0) * 1e-6
            lam = (med(s, t, 'pkt_s_M') or 0) * 1e6
            run_s = (med(s, t, 'run_ns') or 0) / 1e9
            wt = ((med(s, t, 'wakeups') or 0) + (med(s, t, 'ticks') or 0)) / run_s if run_s else None
            pred_w = lam / (1 + lam * L) if lam else None
            pred_ipp = 1 + lam * L
            print(f"    {s:8}| {fmt(med(s,t,'pkt_s_M'),5)} {fmt(med(s,t,'enq_ns'),5)}({fmt(med(s,t,'enq_pct'),2)}%) | {fmt(wt,8)} ({fmt(pred_w,8)}) | {fmt(med(s,t,'items_per_pass'),6)} ({fmt(pred_ipp,6)}) | {fmt(med(s,t,'lat_mean_us'),4)}/{fmt(med(s,t,'lat_max_us'),5)}")
# conformance
ct = sorted(tags('conf_'), key=lambda t: (int(t.split('_')[1]), int(t.split('_')[2])))
if ct:
    print()
    print('conformance (mode 1 constant, 2 exponential spacing; rate items/s): items/s M | passes/s (predicted lambda/(1+lambda L)) | ipp (predicted 1+lambda L) | last-item max us (bound L + pass) | wakes/s | ticks/s')
    for t in ct:
        print(f'  {t}')
        for s in stages:
            L = stage_L.get(s, 0) * 1e-6
            lam = (med(s, t, 'items_s_M') or 0) * 1e6
            run_s = (med(s, t, 'run_ns') or 0) / 1e9
            ps = (med(s, t, 'passes') or 0) / run_s if run_s else None
            ws = (med(s, t, 'wakeups') or 0) / run_s if run_s else None
            ts = (med(s, t, 'ticks') or 0) / run_s if run_s else None
            pred_p = lam / (1 + lam * L) if lam else None
            print(f"    {s:8}| {fmt(med(s,t,'items_s_M'),6)} | {fmt(ps,8)} ({fmt(pred_p,8)}) | {fmt(med(s,t,'items_per_pass'),6)} ({fmt(1 + lam * L,6)}) | {fmt(med(s,t,'last_max_us'),6)} (L={stage_L.get(s,0):.0f}) | {fmt(ws,7)} {fmt(ts,7)}")
