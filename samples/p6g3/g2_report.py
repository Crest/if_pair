#!/usr/bin/env python3
"""g2_report.py - P6g.3's kernel measurements (samples/p6g3/) as the G2
report's datasets: JSON row arrays in OUT (default samples/p6g3/report).

  python3 g2_report.py [OUT]

Medians over the repetitions, with the range as <metric>_lo/_hi.
"""
import collections
import glob
import json
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "report")
NCPU = {"lab": 16, "a07": 128}
STAGE = {"kwqnosteal": "no stealing", "kwq": "today", "kwqgossip": "gossip",
         "kwqshard": "P8", "eli": "stock GELI", "nop": "no encryption"}


def rows5(path):
    reps = collections.defaultdict(lambda: collections.defaultdict(dict))
    if not os.path.exists(path):
        return reps
    for l in open(path):
        c = l.rstrip("\n").split("\t")
        if len(c) == 5:
            try:
                reps[(c[2], c[0])][c[1]][c[3]] = float(c[4])
            except ValueError:
                pass
    return reps


def agg(reps, m, scale=1.0):
    xs = [r[m] * scale for r in reps.values() if m in r]
    if not xs:
        return {}
    return {m: statistics.median(xs), m + "_lo": min(xs), m + "_hi": max(xs)}


def write(name, rows):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + ".json"), "w") as f:
        json.dump(rows, f, separators=(",", ":"))
    print(f"{name}: {len(rows)} rows", file=sys.stderr)


def scale():
    out = []
    for mach, sub, tag in (("lab", "lab/scale_lab.tsv", ""), ("a07", "a07/scale_a07.tsv", ""),
                           ("lab", "lab2/scale_spill500.tsv", "spill_idle=1 donor_us=500"),
                           ("lab", "lab2/scale_spill50.tsv", "spill_idle=1 donor_us=50"),
                           ("a07", "a07b/scale_spill500.tsv", "spill_idle=1 donor_us=500"),
                           ("a07", "a07b/scale_spill50.tsv", "spill_idle=1 donor_us=50"),
                           ("lab", "lab4/scale_chain.tsv", "directed=2"), ("a07", "a07d/scale_chain.tsv", "directed=2"),
                           ("lab", "lab6/scale_final.tsv", "G2 defaults"), ("a07", "a07f/scale_final.tsv", "G2 defaults")):
        f = os.path.join(HERE, sub)
        if not os.path.exists(f):
            continue
        for (t, s), reps in sorted(rows5(f).items()):
            if not t.startswith("scale") or t == "scale0":
                continue
            r = {"machine": mach, "knobs": tag or "defaults", "n": int(t[5:]), "stage": STAGE.get(s, s), "reps": len(reps)}
            for m in ("small_iops", "small_lat_p99_us", "bulk_mbps", "bulk_lat_p99_us", "small_timer_late_p99",
                      "small_spin_share", "small_exec_exec_p99"):
                r.update(agg(reps, m))
            xs = [rr["small_sys"] * NCPU[mach] * 1e6 / rr["small_iops"] for rr in reps.values()
                  if rr.get("small_iops") and "small_sys" in rr]
            if xs:
                r["cpu_us_per_read"] = statistics.median(xs)
            r["key"] = f"{mach}|{r['knobs']}|{r['n']}|{r['stage']}"
            out.append(r)
    return out


def tput():
    out = []
    for mach, f, tag in (("lab", "lab/tput_lab.tsv", "defaults"), ("a07", "a07/tput_a07.tsv", "defaults"),
                         ("lab", "lab2/tput_spill500.tsv", "spill_idle=1 donor_us=500"),
                         ("lab", "lab2/tput_spill50.tsv", "spill_idle=1 donor_us=50"),
                         ("a07", "a07b/tput_spill500.tsv", "spill_idle=1 donor_us=500"),
                         ("a07", "a07b/tput_spill50.tsv", "spill_idle=1 donor_us=50"),
                         ("lab", "lab4/tput_chain.tsv", "directed=2"), ("a07", "a07d/tput_chain.tsv", "directed=2"),
                         ("lab", "lab6/tput_final.tsv", "G2 defaults"), ("a07", "a07f/tput_final.tsv", "G2 defaults")):
        if not os.path.exists(os.path.join(HERE, f)):
            continue
        rp = rows5(os.path.join(HERE, f))
        for s in sorted({s for t, s in rp}):
            r = {"machine": mach, "knobs": tag, "stage": STAGE.get(s, s)}
            r.update(agg(rp.get(("tput", s), {}), "read_mbps"))
            r.update(agg(rp.get(("tput", s), {}), "write_mbps"))
            for m in ("exec_exec_p99", "timer_late_p99", "spin_share", "load_mbps"):
                r.update({("resp_" + k): v for k, v in agg(rp.get(("resp", s), {}), m).items()})
            r["key"] = f"{mach}|{tag}|{r['stage']}"
            out.append(r)
    return out


def ssd():
    out = []
    for f, tag in (("a07/ssd_a07.tsv", "defaults"), ("a07d/ssd_chain.tsv", "directed=2"), ("a07f/ssd_final.tsv", "G2 defaults")):
        if not os.path.exists(os.path.join(HERE, f)):
            continue
        rp = rows5(os.path.join(HERE, f))
        loads = sorted({m.split("_")[0] for (t, s), reps in rp.items() for r in reps.values() for m in r if "_" in m} - {"model"})
        for ld in loads:
            for s in ("nop", "eli", "kwq", "kwqgossip", "kwqshard"):
                reps = rp.get(("ssd", s))
                if not reps:
                    continue
                r = {"machine": "a07", "knobs": tag, "load": ld, "stage": STAGE[s]}
                for m in ("iops", "mbps", "lat_p99_us", "sys", "timer_late_p99"):
                    r.update({k.replace(ld + "_", ""): v for k, v in agg(reps, ld + "_" + m).items()})
                r["key"] = f"a07|{tag}|{ld}|{r['stage']}"
                out.append(r)
    return out


def crypto():
    out = []
    pat = re.compile(r"([0-9.]+) MB/s p50 ([0-9]+) p99 ([0-9]+).*jain ([0-9]+) cpus ([0-9]+)")
    names = {"0": "today", "1": "gossip", "2": "p8"}	# round 2's names, default code in both
    for mach, files in (("lab", ["lab/crypto_lab.tsv", "lab2/k4_lab.tsv", "lab2/k4b_lab.tsv", "lab3/k4c_lab.tsv",
                                 "lab4/k4e_lab.tsv", "lab5/k4f_lab.tsv", "lab6/k4_final.tsv"]),
                        ("a07", ["a07/crypto_a07.tsv", "a07b/k4_a07.tsv", "a07b/k4b_a07.tsv", "a07c/k4c_a07.tsv",
                                 "a07c/k4d_a07.tsv", "a07d/k4e_a07.tsv", "a07e/k4f_a07.tsv", "a07f/k4_final.tsv"])):
        cell = collections.defaultdict(list)
        for f in files:
            p = os.path.join(HERE, f)
            if not os.path.exists(p):
                continue
            for l in open(p):
                c = l.rstrip("\n").split("\t")
                if len(c) < 4:		# k4e adds the steal counters
                    continue
                m = pat.search(c[3])
                if not m:
                    continue
                if f.startswith(mach + "/crypto"):	# nq, place, rep
                    cfg, nq = names[c[1]], c[0]
                else:				# config, nq, rep
                    cfg, nq = c[0], c[1]
                    if f.endswith("k4_final.tsv") and cfg == "p8":
                        cfg = "p8_g2"		# the G2 defaults, apart from P8 alone
                cell[(cfg, int(nq))].append(tuple(float(x) for x in m.groups()))
        today = {nq: statistics.median(x[0] for x in xs) for (cfg, nq), xs in cell.items() if cfg == "today"}
        for (cfg, nq), xs in sorted(cell.items()):
            mb = [x[0] for x in xs]
            r = {"machine": mach, "config": cfg, "queues": nq, "runs": len(xs), "mbps": statistics.median(mb),
                 "mbps_lo": min(mb), "mbps_hi": max(mb), "p99_us": statistics.median(x[2] for x in xs),
                 "jain": statistics.median(x[3] for x in xs) / 1000, "cpus": statistics.median(x[4] for x in xs)}
            if nq in today:
                r["of_today"] = r["mbps"] / today[nq]
            r["key"] = f"{mach}|{cfg}|{nq}"
            out.append(r)
    return out


def abtests():
    """The interleaved A/B rounds, kwq.ko preloaded: blocks of three runs of
    today's stage and three of P8's under one setting of the P8 knobs, the
    settings in turn (lab: scale at 128 providers; a07: ssd randread)."""
    out = []
    rounds = (("lab", "lab, 1", "lab4/k2ab/k2ab_*.tsv", "scale128", ("bulk_mbps", "small_iops"),
               {"d0": "P8", "d2": "P8 + chain"}),
              ("a07", "a07, 1", "a07d/ssdab/ssdab_*.tsv", "ssd", ("randread_iops", "randread_sys"),
               {"d0": "P8", "d2": "P8 + chain"}),
              ("a07", "a07, 2", "a07e/ssdab2_*.tsv", "ssd", ("randread_iops", "randread_sys"),
               {"d0_u0": "P8", "d2_u0": "P8 + chain", "d2_u10": "P8 + chain, directed_us 10"}),
              ("a07", "a07, 3", "a07f/ssdab_final_*.tsv", "ssd", ("randread_iops", "randread_sys"),
               {"final": "P8, G2 defaults"}))
    for mach, rnd, pat, test, metrics, cfgs in rounds:
        v = collections.defaultdict(list)
        for f in sorted(glob.glob(os.path.join(HERE, pat))):
            b = os.path.basename(f)[:-4].split("_")[1:-1]	# k2ab_d0_1 -> [d0]; ssdab2_d2_u10_3 -> [d2, u10]
            cfg = "_".join(b)
            for l in open(f):
                c = l.rstrip("\n").split("\t")
                if len(c) >= 5 and c[2] == test and c[3] in metrics:
                    v[(c[0], cfg, c[3])].append(float(c[4]))
        for m in metrics:
            t = sum((v[("kwq", c, m)] for c in cfgs), [])
            if not t:
                continue
            tm = statistics.median(t)
            out.append({"machine": mach, "round": rnd, "metric": m, "config": "today", "runs": len(t), "median": tm,
                        "lo": min(t), "hi": max(t), "key": f"{rnd}|{m}|today"})
            for c, nm in cfgs.items():
                xs = v[("kwqshard", c, m)]
                if xs:
                    out.append({"machine": mach, "round": rnd, "metric": m, "config": nm, "runs": len(xs),
                                "median": statistics.median(xs), "lo": min(xs), "hi": max(xs),
                                "of_today": statistics.median(xs) / tm, "key": f"{rnd}|{m}|{nm}"})
    return out


def kwqtest():
    out = []
    names = {"0": "today", "1": "gossip", "2": "P8"}
    for mach in ("lab", "a07"):
        p = os.path.join(HERE, mach, "kwqtest_%s.tsv" % mach)
        cell = collections.defaultdict(list)
        for l in open(p):
            c = l.rstrip("\n").split("\t")
            if len(c) == 8:
                cell[(c[0], names[c[1]])].append((int(c[4]) / 1e6, int(c[5]), int(c[6]), int(c[7])))
        for (sc, pl), xs in sorted(cell.items()):
            out.append({"machine": mach, "scenario": sc, "placement": pl, "runs": len(xs),
                        "ms": statistics.median(x[0] for x in xs), "ms_lo": min(x[0] for x in xs),
                        "ms_hi": max(x[0] for x in xs), "off_cpu": statistics.median(x[1] for x in xs),
                        "cpus": statistics.median(x[2] for x in xs), "dups": sum(x[3] for x in xs),
                        "key": f"{mach}|{sc}|{pl}"})
    return out


def criteria(sc, tp, cr, ab):
    """K1-K5 (SPEC-p6g.md S1) per machine and scheme, from the medians."""
    def cell(rows, **kw):
        for r in rows:
            if all(r.get(k) == v for k, v in kw.items()):
                return r
        return None
    schemes = [("gossip", "gossip", "defaults"), ("P8", "P8", "defaults"),
               ("P8 + spill_idle", "P8", "spill_idle=1 donor_us=500"), ("P8 + chain", "P8", "directed=2"),
               ("P8, G2 defaults", "P8", "G2 defaults")]
    out = []
    for mach, n3 in (("lab", 16), ("a07", 64)):
        for name, stage, knobs in schemes:
            def sc_(n, st, k=knobs):
                return cell(sc, machine=mach, n=n, stage=st, knobs=k)
            row = {"machine": mach, "scheme": name}
            # no stealing of the same round where the round ran it (the
            # chain's and the final one), else the first round's
            ns = sc_(128, "no stealing", knobs) or sc_(128, "no stealing", "defaults")
            s8, td = sc_(128, stage), sc_(128, "today", knobs)
            if ns and s8:
                k1 = s8["small_iops"] >= 0.97 * ns["small_iops"] and \
                    s8["cpu_us_per_read"] <= 1.05 * ns["cpu_us_per_read"]
                row["K1"] = "pass" if k1 else "fail"
                row["K1_v"] = f"{s8['small_iops'] / ns['small_iops']:.3f} of no stealing, CPU {s8['cpu_us_per_read'] / ns['cpu_us_per_read']:.3f}"
            if td and s8:
                row["K2"] = "pass" if s8["bulk_mbps"] >= 0.98 * td["bulk_mbps"] else "fail"
                row["K2_v"] = f"{s8['bulk_mbps'] / td['bulk_mbps']:.3f} of today"
            # K2 for the chain on lab from the interleaved A/B (6 runs against
            # 12 of today), which settles the round's edge value
            k2 = next((r for r in ab if r["machine"] == mach and r["metric"] == "bulk_mbps" and r["config"] == name), None)
            if k2 and name == "P8 + chain":
                if "K2_v" in row:
                    row["K2_v"] += f" in its round; the A/B: {k2['of_today']:.3f} ({k2['runs']} runs)"
                row["K2"] = "pass" if k2["of_today"] >= 0.98 else "fail"
            a, t3 = sc_(n3, stage), sc_(n3, "today", knobs)
            if a and t3:
                r1, r2 = a["small_iops"] / t3["small_iops"], a["bulk_mbps"] / t3["bulk_mbps"]
                row["K3"] = "pass" if r1 >= 0.98 and r2 >= 0.98 else "fail"
                row["K3_v"] = f"N={n3}: reads {r1:.3f}, writes {r2:.3f} of today"
            ccfg = {"gossip": "gossip", "P8": "p8", "P8 + spill_idle": "p8_spill", "P8 + chain": "p8_chain",
                    "P8, G2 defaults": "p8_g2"}[name]
            c1, c4 = cell(cr, machine=mach, config=ccfg, queues=1), cell(cr, machine=mach, config=ccfg, queues=4)
            if c1 and c4:
                row["K4"] = "pass" if c1["of_today"] >= 0.9 and c4["of_today"] >= 0.9 else "fail"
                row["K4_v"] = f"kwq_crypto 1 queue {c1['of_today']:.2f}, 4 queues {c4['of_today']:.2f} of today"
            rs, rt = cell(tp, machine=mach, stage=stage, knobs=knobs), cell(tp, machine=mach, stage="today", knobs=knobs)
            if rs and rt and "resp_exec_exec_p99" in rs:
                ok = rs["resp_exec_exec_p99"] <= rt["resp_exec_exec_p99_hi"] and \
                    rs["resp_timer_late_p99"] <= rt["resp_timer_late_p99_hi"] and \
                    rs["resp_spin_share"] >= rt["resp_spin_share_lo"]
                row["K5"] = "pass" if ok else "fail"
                row["K5_v"] = (f"exec p99 {rs['resp_exec_exec_p99'] / 1000:.1f} ms (today {rt['resp_exec_exec_p99'] / 1000:.1f}), "
                               f"timer p99 {rs['resp_timer_late_p99']:.0f} us ({rt['resp_timer_late_p99']:.0f}), "
                               f"spin {rs['resp_spin_share']:.2f} ({rt['resp_spin_share']:.2f})")
            row["key"] = f"{mach}|{name}"
            out.append(row)
    return out


def hybrid():
    """The main set's first three seeds: today, gossip, P8 and H1 (P5 with lazy=1)."""
    sys.path.insert(0, os.path.join(HERE, "..", "..", "kwq", "sim", "place"))
    import summarize as S
    path = os.path.join(HERE, "..", "..", "kwq", "sim", "place", "out", "hybrid_main3.tsv")
    if not os.path.exists(path):
        return []
    runs = S.load(path)
    names = {1: "today", 5: "gossip", 8: "P8", 91: "H1, lazy_us 0", 92: "H1, lazy_us 10"}
    out = []
    for cid, text, applies, judge in S.criteria():
        row = {"criterion": cid, "text": text}
        for p, nm in names.items():
            n = ok = 0
            for (v, topo, pp, sd), m in runs.items():
                if pp != p or not applies(v, topo):
                    continue
                o = runs.get((v, topo, 0, sd))
                if o is None or "wait_all_mean_us" not in o or "wait_all_mean_us" not in m:
                    continue
                try:
                    r = judge(m, o, runs.get((v, topo, 1, sd)), runs.get((v, topo, 2, sd))) if cid == "W6" else \
                        judge(m, o, runs.get((v, topo, 1, sd)))
                except KeyError:
                    continue
                if r is None:
                    continue
                n += 1
                ok += bool(r[0])
            if n:
                row[nm] = ok
                row[nm + "_runs"] = n
        row["key"] = cid
        out.append(row)
    return out


def t4risk():
    sys.path.insert(0, os.path.join(HERE, "..", "..", "kwq", "sim", "place"))
    import summarize as S
    path = os.path.join(HERE, "..", "..", "kwq", "sim", "place", "out", "main.tsv")
    if not os.path.exists(path):
        return []
    runs = S.load(path)
    out = []
    for v in ("S1-uniform-rho0.95-small", "S4-idleisland-k4-small", "S5-stripes-l0", "S8-stop", "S15-full-rho1.2"):
        for t in ("lab", "a07", "t4"):
            row = {"scenario": v, "topology": t}
            for p, nm in ((0, "oracle"), (1, "today"), (5, "gossip"), (8, "P8")):
                xs = [m for (vv, tt, pp, sd), m in runs.items() if vv == v and tt == t and pp == p and "wait_all_mean_us" in m]
                if xs:
                    row[nm + "_wait_us"] = statistics.median(m["wait_all_mean_us"] for m in xs)
                    row[nm + "_mops"] = statistics.median(m["ops_per_s"] for m in xs) / 1e6
            row["key"] = f"{v}|{t}"
            out.append(row)
    return out


FINDINGS = [
    ("P8's steal wake looked in the donor's own group only", "the guest with groups of one CPU: three workers slept beside a donor, one CPU ran 6 of 3000 items", "the wake and the pass start's helper test read the group lines covering the steal reach", "fe1c50f", "guest"),
    ("kwq_scatter() read the old idle mask", "under place=2 no CPU looked idle: no call reached a second CPU", "kwq_bulk_idle_hint(), the mode's own idle view", "fe1c50f", "guest"),
    ("Gossip aged a worker's status word", "after 32 ms asleep a worker looked unknown, not idle (scatter again)", "read as kp_core_view() reads it, without an age", "fe1c50f", "guest"),
    ("kwq-geli's steal case counted stolen bios only", "under place 1 and 2 bios spill at the enqueue: 1 and 3 of 470 stolen", "the case asks for what ran off the CPU", "9c51819", "guest"),
    ("island asked an eighth of the items from every CPU", "fair only up to eight CPUs: lab's CPU 0 ran 311 of 3000", "a quarter of the fair share", "b838b8b", "lab"),
    ("A closed-loop queue on a fixed CPU did not spread", "16 requests in flight hold less than a donor's 500 us or gossip's H of 2 ms: lab 9 and 10 % of today", "shard.spill_idle (P8): lab 91 %, later superseded by the chain; gossip has no equivalent", "0994902", "lab"),
    ("spill_idle near the producer drifted on a07", "the producer is the CPU that completed the last request: 16 requests over 128 CPUs, 44 % of today", "spill_idle=2 searches near the fixed CPU", "b3a5ffa", "a07"),
    ("The starved tick's threat model", "BULK runs at PI_SOFT, above every user thread: only interrupt threads and NET work starve it", "a direct callout in the clock interrupt; the G1 page's wording, corrected", "246daef, df4e03d", "review"),
    ("The mailbox alone left a07's one queue at 56 %", "with directed=1 every item waited one service time (p50 79 us, today 39), at donor_us 500, 2000 and 5000 alike: not the steal bound", "the lone item behind running work never makes a donor, so a thief slept after each batch; see the chain", "e5fd01f", "a07"),
    ("A P8 thief slept after every batch of a shallow list", "one wake per item: lab 1.99 M wakes in a run where today needed 142 k", "shard.directed=2, the chain: an idle thief retries its last victim; lab 100 %, a07 98 % of today for one queue, 99 % and 170 % for four, Jain 1.00", "777b372", "lab, a07"),
    ("P8's randread lead on a07 seemed lost with the chain", "1.067 of today without it, 1.003 with it (6 runs each, U test p 0.24); a second A/B: 0.817 and 0.667; every configuration's runs fall at about 0.6 M or 1.3 M IOPS", "not the chain: it took 0 to 2 batches per block of randread (the mailbox 229 to 641) against about a million steals of today's stage; randread items cost 7.5 to 8.3 us; the test cannot resolve 10 % with 6 runs", "3c5c2cd", "a07"),
    ("A cost gate for the mailbox and the chain", "a list of cheap items would move as today's scan moves it", "shard.directed_us, default 0 (off): neither test needs it, K4 is the same at 10 us (lab 0.99 and 0.98, a07 0.98 and 1.70)", "3c5c2cd", "lab, a07"),
    ("The pass cost average took glitched CPU-time deltas", "a07's ticker glitches drove a list's average below zero: 2^63 ns or more in half the cost samples, the donor rule's depth x cost overflowed and the steal bound fell to one item", "the average skips what ks_pass_end() counts as a glitch; a07 after the fix: at most 14 us over 13 samples", "946967d", "a07"),
    ("The steal wake read the queue after the list unlock", "with directed the mailbox word came from kc->kc_q after the reach walk: a drain and destroy completing meanwhile would be read freed (a caller bug only, as today's flag read, but a longer window)", "the wake's test and word happen under the list lock; the thief's side was safe already (slot and id under kwq_steal_rm)", "b40b3a6", "review"),
    ("P8 on the simulator's two-socket model", "at rho 0.95 of 3 us items, P8 placed a third of the items per second with 15 us of CPU each (lab and a07: none of it)", "open: no two-socket machine to measure; P8 for one package only until one is", "-", "sim"),
    ("H1, gossip with lazy idle lines", "converges on P8 only when its laziness is off; below P8 on every criterion of the main set", "dropped as the hybrid", "533c351", "sim"),
]


def findings():
    return [{"finding": a, "effect": b, "fix": c, "commit": d, "where": e, "key": a} for a, b, c, d, e in FINDINGS]


sc, tp, cr, ab = scale(), tput(), crypto(), abtests()
write("kscale", sc)
write("ktput", tp)
write("kssd", ssd())
write("kcrypto", cr)
write("ktest", kwqtest())
write("kab", ab)
write("kcriteria", criteria(sc, tp, cr, ab))
write("hybrid", hybrid())
write("t4risk", t4risk())
write("kfindings", findings())
