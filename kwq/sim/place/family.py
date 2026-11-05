#!/usr/bin/env python3
"""family.py - placesim's matrix by scenario family and topology: the
median over variants and seeds of each policy's mean wait, p99.9 and max
wait relative to the oracle's, and the CPU per item.

    python3 family.py OUT.tsv [--policies 0,1,2,5,7,8] [--by variant]
"""
import argparse, collections, math, re, statistics, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("--policies", default="0,1,2,5,7,8")
    ap.add_argument("--by", default="family")
    ap.add_argument("--metric", default="wait_all_mean_us")
    a = ap.parse_args()
    pols = [int(x) for x in a.policies.split(",")]
    runs = collections.defaultdict(dict)
    for line in open(a.tsv):
        c = line.rstrip("\n").split("\t")
        if len(c) == 7:
            runs[(c[0], c[2], int(c[3]), int(c[4]))][c[5]] = float(c[6])
    cell = collections.defaultdict(list)
    for (v, t, p, s), m in runs.items():
        if p not in pols or a.metric not in m:
            continue
        key = v if a.by == "variant" else re.match(r"^(S\d+-[a-z]+)", v).group(1)
        cell[(key, t, p)].append(m[a.metric])
    keys = sorted({(k, t) for k, t, p in cell}, key=lambda x: (int(re.match(r"S(\d+)", x[0]).group(1)), x[0], x[1]))
    print(f"{a.metric}: median (max) over variants and seeds")
    print(f"{'family':30s} {'topo':6s} " + " ".join(f"{p:>16d}" for p in pols))
    for k, t in keys:
        row = []
        for p in pols:
            x = cell.get((k, t, p))
            row.append(f"{statistics.median(x):8.3g} ({max(x):6.3g})" if x else f"{'-':>16s}")
        print(f"{k[:30]:30s} {t:6s} " + " ".join(f"{r:>16s}" for r in row))

main()
