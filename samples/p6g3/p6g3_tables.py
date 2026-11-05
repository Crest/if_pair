#!/usr/bin/env python3
"""p6g3_tables.py - P6g.3's geli_bench rows as tables: per machine, test and
stage the median over repetitions and its range.

  p6g3_tables.py NCPU SCALE_TSV [TPUT_TSV] [SSD_TSV]
"""
import collections
import statistics
import sys

ncpu = int(sys.argv[1])


def load(path):
    rows = collections.defaultdict(lambda: collections.defaultdict(dict))
    try:
        with open(path) as f:
            for l in f:
                c = l.rstrip("\n").split("\t")
                if len(c) != 5:
                    continue
                try:
                    rows[(c[2], c[0])][c[1]][c[3]] = float(c[4])
                except ValueError:
                    pass
    except FileNotFoundError:
        pass
    return rows


def med(reps, m, fmt="{:.0f}"):
    xs = [r[m] for r in reps.values() if m in r]
    if not xs:
        return "-"
    return fmt.format(statistics.median(xs)) + (" (" + fmt.format(min(xs)) + "-" + fmt.format(max(xs)) + ")" if len(xs) > 1 else "")


def cpu_per_op(reps, p):
    xs = [r[p + "sys"] * ncpu * 1e6 / r[p + "iops"] for r in reps.values()
          if r.get(p + "iops") and (p + "sys") in r]
    return "{:.1f}".format(statistics.median(xs)) if xs else "-"


ORDER = ["eli", "nop", "kwqnosteal", "kwq", "kwqgossip", "kwqshard"]
sc = load(sys.argv[2])
tests = sorted({t for t, s in sc}, key=lambda t: int(t[5:]) if t[5:].isdigit() else -1)
print("scale: per N and stage, medians (range) over the repetitions")
print(f"{'N':>4s} {'stage':11s} {'small IOPS':>22s} {'small p99 us':>18s} {'cpu us/op':>9s} {'bulk MB/s':>20s} {'bulk p99 us':>22s} {'timer p99':>9s} {'spin':>6s} {'exec p99':>8s}")
for t in tests:
    if t == "scale0":
        continue
    for s in ORDER:
        reps = sc.get((t, s))
        if not reps:
            continue
        print(f"{t[5:]:>4s} {s:11s} {med(reps, 'small_iops'):>22s} {med(reps, 'small_lat_p99_us'):>18s} {cpu_per_op(reps, 'small_'):>9s} "
              f"{med(reps, 'bulk_mbps'):>20s} {med(reps, 'bulk_lat_p99_us'):>22s} {med(reps, 'small_timer_late_p99', '{:.0f}'):>9s} "
              f"{med(reps, 'small_spin_share', '{:.2f}'):>6s} {med(reps, 'small_exec_exec_p99', '{:.0f}'):>8s}")
if len(sys.argv) > 3:
    tp = load(sys.argv[3])
    print("\ntput and resp: medians (range)")
    ms = sorted({m for (t, s), reps in tp.items() for r in reps.values() for m in r})
    for t in sorted({t for t, s in tp}):
        for s in ORDER:
            reps = tp.get((t, s))
            if not reps:
                continue
            pick = [m for m in ms if any(m in r for r in reps.values()) and
                    any(k in m for k in ("mbps", "late_p99", "spin_share", "exec_p99", "sys"))]
            print(f"{t:8s} {s:11s} " + "  ".join(f"{m}={med(reps, m, '{:.2f}' if 'share' in m or m.endswith('sys') else '{:.0f}')}" for m in pick[:9]))
if len(sys.argv) > 4:
    sd = load(sys.argv[4])
    print("\nssd: medians (range)")
    for t in sorted({t for t, s in sd}):
        for s in ORDER:
            reps = sd.get((t, s))
            if not reps:
                continue
            print(f"{t:12s} {s:11s} iops {med(reps, 'iops')} mbps {med(reps, 'mbps')} p99 {med(reps, 'lat_p99_us')} sys {med(reps, 'sys', '{:.2f}')}")
