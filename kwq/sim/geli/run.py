#!/usr/bin/env python3
"""run.py [OUT.jsonl] [JOBS]: gelisim's experiments, one JSON object per run.
E1-E9 are named in the report; every run is one (hardware, load, variant,
seed).  The hardware classes' costs and how they were fitted: README.md."""
import concurrent.futures as cf, json, subprocess, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else 'out/runs.jsonl'
JOBS = int(sys.argv[2]) if len(sys.argv) > 2 else 6
SEEDS = [1, 2, 3, 4, 5]

# per MiB unless named otherwise; lab at saturation, see README.md
LAB = dict(ncpu=16, crypto_ms=.5, copy_ms=.5, md_ms=.5, user_us=470, traffic=1,
           noise_hz=2000, cold_crypto=3)
LAB16 = dict(LAB, crypto_ms=.28, copy_ms=.28, md_ms=.35, user_us=440)
A07 = dict(ncpu=128, crypto_ms=.42, copy_ms=.42, md_ms=.6, user_us=600, traffic=1,
           noise_hz=2000, cold_crypto=0)
N1SW = dict(ncpu=8, crypto_ms=6.4, copy_ms=.3, md_ms=.5, user_us=500, traffic=1,
            noise_hz=2000, cold_crypto=.05, cold_md=.05)
SLOW = dict(ncpu=2, crypto_ms=56, copy_ms=4, md_ms=4, user_us=1000, traffic=1,
            noise_hz=1000, cold_crypto=.05, cold_md=.05)
PIN_LAB = dict(ncpu=16, pin=1, steal=0, nprov=4, crypto_ms=.14, copy_ms=.14, md_ms=.2,
               user_us=100, traffic=1, cold_crypto=0, noise_hz=0, netcpu=1)
PIN_A07 = dict(PIN_LAB, crypto_ms=.42, copy_ms=.42, md_ms=.6, user_us=300)

V = {
    'today': {},
    'whole': dict(budget_sectors=0),
    'keep': dict(keep=1),
    'netkeep': dict(keep=2),
    'time': dict(trule_us=1000),
    'keep+time': dict(keep=1, trule_us=1000),
    'netkeep+time': dict(keep=2, trule_us=1000),
    'netkeep+time+K1': dict(keep=2, trule_us=1000, k=1),
    'fit': dict(fit=1),
    'netkeep+time+fit': dict(keep=2, trule_us=1000, k=1, fit=1),
    'shared': dict(shared=1),
    'owner': dict(owner=1),
}
jobs = []

def add(exp, hw, base, var, n=None, **extra):
    o = dict(base)
    o.update(V[var] if isinstance(var, str) else var)
    o.update(extra)
    if n is not None:
        o['nprov'] = n
    for s in SEEDS:
        jobs.append(dict(exp=exp, hw=hw, var=var if isinstance(var, str) else extra.get('tag', ''),
                         n=o.get('nprov', 64), seed=s, opts=dict(o, seed=s)))

# E1, E2: lab's scale load: validation and the variants
for n in (16, 32, 64, 128):
    for v in V:
        add('scale', 'lab', LAB16 if n == 16 else LAB, v, n, timer=1, secs=3)
# E3: the time rule's T, with readers beside the writers
for n in (64, 128):
    for t in (0, 100, 250, 500, 750, 1000, 1250, 1500, 2000, 5000):
        o = dict(LAB, keep=2, trule_us=t, timer=1, secs=3, nprov=n, nsmall=16)
        for s in SEEDS:
            jobs.append(dict(exp='tsweep', hw='lab', var='T%d' % t, n=n, seed=s, opts=dict(o, seed=s)))
# E4: the hardware classes, four readers beside the writers
HV = ('today', 'whole', 'keep', 'keep+time', 'netkeep+time', 'netkeep+time+fit')
for v in HV:
    for n in (128, 256):
        add('hw', 'a07', A07, v, n, timer=1, secs=1.5, nsmall=8)
    for n in (8, 16, 32):
        add('hw', 'n1sw', N1SW, v, n, timer=1, secs=6, nsmall=2)
    for n in (1, 2, 4, 8):
        add('hw', 'slow', SLOW, v, n, timer=1, secs=30, warm=2, nsmall=2)
# the same on a 1 GHz CPU at 32 KiB sectors: a check every 16 sectors is 29 ms
for v in ('today', 'netkeep+time'):
    add('sec32k', 'slow', SLOW, v, 2, timer=1, secs=30, warm=2, nsmall=2, secsize=32768)
for s in SEEDS:
    o = dict(SLOW, keep=2, trule_us=1000, budget_sectors=1, nprov=2, timer=1, secs=30, warm=2, nsmall=2, secsize=32768)
    jobs.append(dict(exp='sec32k', hw='slow', var='netkeep+time, check every sector', n=2, seed=s, opts=dict(o, seed=s)))
# E5: a sparse ping on the CPU of the bios, the pinned half of test 2
for hw, base in (('lab', PIN_LAB), ('a07', PIN_A07)):
    for v in ('today', 'whole', 'keep', 'time', 'keep+time', 'netkeep+time', 'netkeep+time+K1', 'netkeep+time+fit'):
        add('ping', hw, base, v, None, net=1, secs=3)
# E6: 4 KiB reads beside 1 MiB writes, separate providers
for v in ('today', 'whole', 'keep', 'time', 'keep+time', 'netkeep+time', 'netkeep+time+fit', 'shared'):
    add('reads', 'lab', LAB, v, 64, nsmall=16, smallq=1, secs=3, timer=1)
# E7: equal queues on one CPU beside a spinning thread
for nqs in (2, 4, 8, 16):
    for cost in ((.14, .14), (.42, .42), (.5, .5)):
        for wq in (1, 4):
            for v in ('today', 'keep', 'keep+time', 'netkeep+time', 'whole'):
                add('fair', 'bio%.2f q%d' % (cost[0] + cost[1], wq), dict(PIN_LAB, crypto_ms=cost[0], copy_ms=cost[1]),
                    v, nqs, wq=wq, spin=1, noise_hz=2000, secs=5)
# E8: bios of two sizes on one queue, budget_check_every
for hw, base in (('lab', PIN_LAB), ('a07', PIN_A07)):
    for v, k in (('whole', 8), ('whole', 1), ('netkeep+time', 8), ('netkeep+time', 1), ('today', 8), ('today', 1)):
        o = dict(base); o.update(V[v]); o.update(nprov=1, wq=8, nsmall=1, smallq=8, mixq=1, timer=1, secs=5, k=k)
        for s in SEEDS:
            jobs.append(dict(exp='mixed', hw=hw, var='%s K%d' % (v, k), n=1, seed=s, opts=dict(o, seed=s)))
# E9: how much the answers hang on the fitted cache model
for n in (64, 128):
    for cold in (.9, 1.5, 2, 3):
        for noise in (500, 2000, 8000):
            for traffic in (.5, 1, 2):
                for v in ('today', 'whole', 'keep', 'netkeep+time'):
                    o = dict(LAB); o.update(V[v]); o.update(cold_crypto=cold, noise_hz=noise, traffic=traffic, nprov=n, secs=2)
                    for s in SEEDS[:2]:
                        jobs.append(dict(exp='sens', hw='lab', var=v, n=n, seed=s,
                                         sens=dict(cold=cold, noise=noise, traffic=traffic), opts=dict(o, seed=s)))

def run(j):
    a = ['./gelisim'] + ['%s=%s' % kv for kv in j['opts'].items()]
    try:
        p = subprocess.run(a, capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        return dict(j, error='timeout')
    if p.returncode != 0:
        return dict(j, error=p.stderr[-300:])
    r = dict(j)
    r['m'] = {k: float(v) for k, v in (x.split('=') for x in p.stdout.split())}
    return r

print(len(jobs), 'runs', flush=True)
done = bad = 0
with open(OUT, 'w') as f, cf.ThreadPoolExecutor(JOBS) as ex:
    for r in ex.map(run, jobs):
        done += 1
        bad += 'error' in r
        f.write(json.dumps(r) + '\n')
        if done % 200 == 0:
            print(done, 'done', bad, 'failed', flush=True)
            f.flush()
print('finished', done, 'failed', bad, flush=True)
