#!/usr/bin/env python3
"""pmc_report.py DIR: where3_lab.sh's pmcstat files (pmc_<tag>_<n>.txt,
per-CPU increments per second) against the MB/s of the same bulk phase
(scale_<tag>.tsv): per MiB written, cycles, instructions, IPC, and L1D
fills from DRAM (demand, hardware prefetch, any) and from L3, as MiB of
64-byte lines."""
import os, re, sys

D = sys.argv[1]
TAGS = [('stock', 'stock GELI'), ('p2_base', 'base'), ('p2_bio', 'bio'), ('p2_nocap', 'limit off'), ('p55_base', 'prio'), ('p55_bio', 'both'), ('p55_nocap', 'prio, limit off')]
EV = ['ls_not_halted_cyc', 'ex_ret_instr', 'ls_dmnd_fills_from_sys.mem_io_local',
      'ls_hw_pf_dc_fills.mem_io_local', 'ls_any_fills_from_sys.mem_io_local', 'ls_any_fills_from_sys.int_cache']
for n in (32, 128):
    rows = []
    for tag, name in TAGS:
        f = os.path.join(D, 'pmc_%s_%d.txt' % (tag, n))
        if not os.path.exists(f):
            continue
        lines = open(f).read().splitlines()
        hdr = lines[0].lstrip('#').split()
        tot = {e: 0 for e in EV}; secs = 0
        for l in lines[1:]:
            v = l.split()
            if len(v) != len(hdr):
                continue
            secs += 1
            for h, x in zip(hdr, v):
                tot[h.split('/', 2)[2]] += int(x)
        mbps = None
        for l in open(os.path.join(D, 'scale_%s.tsv' % tag)):
            s = l.rstrip('\n').split('\t')
            if len(s) == 5 and s[2] == 'scale%d' % n and s[3] == 'bulk_mbps':
                mbps = float(s[4])
        mib = mbps * 1e6 * secs / 1048576
        line = lambda e: tot[e] * 64 / 1048576 / mib
        rows.append((name, mbps, tot['ls_not_halted_cyc'] / mib / 1e6, tot['ex_ret_instr'] / mib / 1e6,
                     tot['ex_ret_instr'] / tot['ls_not_halted_cyc'], line(EV[2]), line(EV[3]), line(EV[4]), line(EV[5]), secs))
    print('\n### %d providers, 1 MiB writes, per MiB written (all CPUs)\n' % n)
    print('| | MB/s | cycles, M | instructions, M | IPC | DRAM demand fills, MiB | DRAM prefetch fills, MiB | DRAM fills, all, MiB | L3 fills, MiB |')
    print('|---|---|---|---|---|---|---|---|---|')
    for r in rows:
        print('| %s | %.0f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |' % r[:9])
