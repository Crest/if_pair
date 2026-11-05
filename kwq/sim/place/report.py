#!/usr/bin/env python3
"""report.py - the datasets of the G1 report (SPEC-p6g.md S3.7) from the
matrix's TSV files in out/, as JSON row arrays in out/report/.

    python3 report.py

Every number the report shows is in one of these files: verdicts per
criterion and policy, the scenario families, the worst cases over 100
seeds, the ablations and sweeps, the fuzz summary, the validation runs
(run here, from the simulator), P6g.0's lab results and the simulator's
findings.
"""

import collections
import json
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
REP = os.path.join(OUT, "report")
sys.path.insert(0, HERE)
import summarize  # noqa: E402

NAMES = summarize.NAMES


def load(name):
    p = os.path.join(OUT, name)
    return summarize.load(p) if os.path.exists(p) else {}


def write(name, rows):
    os.makedirs(REP, exist_ok=True)
    with open(os.path.join(REP, name + ".json"), "w") as f:
        json.dump(rows, f, separators=(",", ":"))
    print(f"{name}: {len(rows)} rows", file=sys.stderr)


def verdicts(runs, pols, setname):
    crit = summarize.criteria()
    rows = []
    for cid, text, applies, judge in crit:
        for p in pols:
            n = ok = 0
            worst = None
            for (v, topo, pp, s), m in runs.items():
                if pp != p or not applies(v, topo):
                    continue
                o = runs.get((v, topo, 0, s))
                if o is None or "wait_all_mean_us" not in o or "wait_all_mean_us" not in m:
                    continue
                t = runs.get((v, topo, 1, s))
                ns = runs.get((v, topo, 2, s))
                try:
                    r = judge(m, o, t, ns) if cid == "W6" else judge(m, o, t)
                except KeyError:
                    continue
                if r is None:
                    continue
                good, val, bound = r
                n += 1
                ok += bool(good)
                bad = (bound / val if val else 1e9) if cid in ("W1c", "A2", "W1d") else (
                    val / bound if bound else val)
                if not good and (worst is None or bad > worst[0]):
                    worst = (bad, v, topo, s, val, bound)
            if n == 0:
                continue
            rows.append({"set": setname, "criterion": cid, "text": text, "policy": NAMES[p],
                         "passed": ok, "runs": n, "share": ok / n,
                         "worst_case": f"{worst[1]} {worst[2]} seed {worst[3]}" if worst else "",
                         "worst_value": worst[4] if worst else None,
                         "worst_bound": worst[5] if worst else None})
    return rows


def families(runs, pols):
    cell = collections.defaultdict(lambda: collections.defaultdict(list))
    for (v, t, p, s), m in runs.items():
        if p not in pols or "wait_all_mean_us" not in m:
            continue
        fam = re.match(r"^(S\d+)-([a-z]+)", v)
        key = (fam.group(1) + " " + fam.group(2), t, p)
        for k in ("wait_all_mean_us", "wait_all_p999_us", "wait_all_max_us", "xfers_per_place",
                  "cpu_per_op_us", "good_frac", "idle_wait_l3_frac"):
            if k in m:
                cell[key][k].append(m[k])
    rows = []
    for (fam, t, p), d in sorted(cell.items(), key=lambda x: (int(x[0][0][1:].split()[0]), x[0][1], x[0][2])):
        rows.append({"family": fam, "topo": t, "policy": NAMES[p],
                     "runs": len(d["wait_all_mean_us"]),
                     "mean_wait_us": statistics.median(d["wait_all_mean_us"]),
                     "mean_wait_max_us": max(d["wait_all_mean_us"]),
                     "p999_wait_us": statistics.median(d["wait_all_p999_us"]),
                     "max_wait_us": max(d["wait_all_max_us"]),
                     "xfers_per_place": statistics.median(d["xfers_per_place"]),
                     "good_share": statistics.median(d["good_frac"]),
                     "idle_waiting_share": statistics.median(d["idle_wait_l3_frac"]),
                     "key": f"{fam}|{t}|{NAMES[p]}"})
    return rows


def worst_cases(runs):
    cell = collections.defaultdict(list)
    for (v, t, p, s), m in runs.items():
        if "wait_all_max_us" in m:
            cell[(v, t, p)].append((m["wait_all_max_us"], m.get("wait_all_p999_us", 0),
                                    m.get("wait_all_mean_us", 0), s, m.get("moved4p", 0)))
    rows = []
    for (v, t, p), xs in sorted(cell.items()):
        xs.sort()
        mx = xs[-1]
        p999s = sorted(x[1] for x in xs)
        rows.append({"variant": v, "topo": t, "policy": NAMES[p], "seeds": len(xs),
                     "max_wait_us": mx[0], "worst_seed": mx[3],
                     "p999_wait_p99_us": p999s[min(len(p999s) - 1, int(0.99 * len(p999s)))],
                     "mean_wait_med_us": statistics.median(x[2] for x in xs),
                     "runs_moving_4x": sum(1 for x in xs if x[4] > 0),
                     "key": f"{v}|{t}|{NAMES[p]}"})
    return rows


def ablations(abl, base, pol):
    cell = collections.defaultdict(lambda: collections.defaultdict(list))
    for (v, t, p, s), m in abl.items():
        if "+" not in v or "wait_all_mean_us" not in m:
            continue
        b, a = v.split("+", 1)
        bm = base.get((b, t, pol, s))
        if not bm or "wait_all_mean_us" not in bm:
            continue
        r = cell[(a, t)]
        r["mean"].append((m["wait_all_mean_us"] + 1) / (bm["wait_all_mean_us"] + 1))
        r["p999"].append((m["wait_all_p999_us"] + 1) / (bm["wait_all_p999_us"] + 1))
        r["xfers"].append(m.get("xfers_per_place", 0) - bm.get("xfers_per_place", 0))
        r["moved"].append(m.get("moved2p_frac", 0) - bm.get("moved2p_frac", 0))
        r["worse"].append(m["wait_all_p999_us"] > 1.2 * bm["wait_all_p999_us"] + 10)
    rows = []
    for (a, t), d in sorted(cell.items()):
        rows.append({"policy": NAMES[pol], "ablation": a, "topo": t, "runs": len(d["mean"]),
                     "mean_wait_ratio": statistics.median(d["mean"]),
                     "mean_wait_ratio_max": max(d["mean"]),
                     "p999_ratio": statistics.median(d["p999"]),
                     "p999_ratio_max": max(d["p999"]),
                     "runs_worse": sum(d["worse"]),
                     "xfers_change": statistics.median(d["xfers"]),
                     "moved_change": statistics.median(d["moved"]),
                     "key": f"{NAMES[pol]}|{a}|{t}"})
    return rows


def sweeps(sw, base, pol):
    cell = collections.defaultdict(lambda: collections.defaultdict(list))
    for (v, t, p, s), m in sw.items():
        g = re.match(r"^(.*)\+([a-z_]+)=(-?[0-9.]+)$", v)
        if not g or "wait_all_mean_us" not in m:
            continue
        b, k, val = g.group(1), g.group(2), float(g.group(3))
        bm = base.get((b, t, pol, s))
        r = cell[(b, t, k, val)]
        r["mean"].append(m["wait_all_mean_us"])
        r["p999"].append(m["wait_all_p999_us"])
        r["scheme"].append(m.get("scheme_cpu_frac", 0))
        if bm and "wait_all_p999_us" in bm:
            r["p999_ratio"].append((m["wait_all_p999_us"] + 1) / (bm["wait_all_p999_us"] + 1))
    rows = []
    for (b, t, k, val), d in sorted(cell.items()):
        rows.append({"policy": NAMES[pol], "scenario": b, "topo": t, "constant": k, "value": val,
                     "runs": len(d["mean"]), "mean_wait_us": statistics.median(d["mean"]),
                     "p999_wait_us": statistics.median(d["p999"]),
                     "p999_ratio": statistics.median(d["p999_ratio"]) if d["p999_ratio"] else None,
                     "scheme_cpu_share": statistics.median(d["scheme"]),
                     "key": f"{NAMES[pol]}|{b}|{t}|{k}|{val:g}"})
    return rows


def fuzz(fz):
    cell = collections.defaultdict(lambda: collections.Counter())
    mx = collections.defaultdict(float)
    for (v, t, p, s), m in fz.items():
        c = cell[NAMES[p]]
        c["runs"] += 1
        c["violations"] += m.get("violations", 0) > 0
        c["expired"] += m.get("expired_used", 0) > 0
        c["moved4"] += m.get("moved4p", 0) > 0
        mx[NAMES[p]] = max(mx[NAMES[p]], m.get("wait_all_max_us", 0))
    return [{"policy": p, "runs": c["runs"], "runs_with_violation": c["violations"],
             "runs_using_expired": c["expired"], "runs_moving_4x": c["moved4"],
             "max_wait_us": mx[p]} for p, c in sorted(cell.items())]


def validation():
    """V1-V4 (suite.sh's runs), the simulator run here; lab's figures as measured."""
    sim = os.path.join(HERE, "placesim")
    Z = ("c_hit=0 c_sib=0 c_l3=0 c_atomic=0 scan_entry=0 place_ns=0 merge_ns=0 wl_short_us=0 "
         "wl_long_us=0 wl_deep_us=0 smt_bulk=1 smt_other=1 expcost=1 small_us=10 csmall_us=1 "
         "topo=guest scen=uniform dur_ms=1500 warm_ms=20")
    G = "topo=lab scen=geli inflight=1 small_us=1.1 other_us=4.6 dur_ms=150 warm_ms=30"
    V = "topo=lab scen=crypto small_us=191 nprov=2 inflight=4 dur_ms=300 warm_ms=50"
    checks = [
        ("V1", "Oracle, FCFS CPUs, rho 0.8: mean wait (us)", "Erlang C", 7.455, f"{Z} policy=0 oneq=1 rho=0.8", "wait_all_mean_us", 1, "theory"),
        ("V1", "Oracle, FCFS CPUs, rho 0.9: mean wait (us)", "Erlang C", 19.69, f"{Z} policy=0 oneq=1 rho=0.9", "wait_all_mean_us", 1, "theory"),
        ("V1", "Random placement, rho 0.8: mean wait (us)", "M/M/1", 40.0, f"{Z} policy=6 rho=0.8", "wait_all_mean_us", 1, "theory"),
        ("V3", "128 providers, stealing off: reads/s", "lab", 1.58e6, f"{G} nprov=128 policy=2", "ops_per_s", 1, "calibrated"),
        ("V3", "128 providers, stealing on: reads/s", "lab", 0.55e6, f"{G} nprov=128 policy=1", "ops_per_s", 1, "scan cost calibrated"),
        ("V3", "128 providers, stealing on: steal attempts per read", "lab", 1.05, f"{G} nprov=128 policy=1", "steal_tries_per_op", 1, "predicted"),
        ("V3", "128 providers, stealing on: steals per read", "lab", 0.18, f"{G} nprov=128 policy=1", "steals_per_op", 1, "predicted"),
        ("V3", "128 providers, steal_scan 2: reads/s", "lab", 1.35e6, f"{G} nprov=128 policy=1 steal_scan=2", "ops_per_s", 1, "predicted"),
        ("V3", "16 busy of 128 queues, stealing on: reads/s", "lab", 0.561e6, f"{G} nprov=16 extraq=112 policy=1", "ops_per_s", 1, "predicted"),
        ("V3", "16 busy of 128 queues, stealing off: reads/s", "lab", 1.68e6, f"{G} nprov=16 extraq=112 policy=2", "ops_per_s", 1, "predicted"),
        ("V3", "128 providers, 1 MiB writes, share limit off: gain", "lab", 8260 / 5303,
         None, None, None, "predicted"),
        ("V4", "kwq_crypto 2 x 4 x 1 MiB, today's order: GB/s", "lab", 41.9, f"{V} policy=1", "ops_per_s", 1.048576e-3, "wake latency calibrated"),
        ("V4", "the same, sibling last: GB/s", "lab", 33.5, f"{V} policy=1 sib_last=1", "ops_per_s", 1.048576e-3, "predicted"),
        ("V4", "the same, every thread busy (4 x 4): GB/s", "lab", 51.3, "topo=lab scen=crypto small_us=191 nprov=4 inflight=4 dur_ms=300 warm_ms=50 policy=1", "ops_per_s", 1.048576e-3, "predicted"),
    ]
    rows = []
    cache = {}
    for vid, what, ref, meas, args, metric, scale, kind in checks:
        if args is None:
            B = "topo=lab scen=geli nprov=128 inflight=1 small_us=1230 other_us=660 dur_ms=1500 warm_ms=200 policy=1"
            a = run(sim, B, cache)
            b = run(sim, B + " cap=1", cache)
            simv = b["ops_per_s"] / a["ops_per_s"]
        else:
            simv = run(sim, args, cache)[metric] * scale
        rows.append({"check": vid, "what": what, "reference": ref, "measured": meas,
                     "simulated": simv, "ratio": simv / meas, "kind": kind, "key": what})
    return rows


def run(sim, args, cache):
    if args not in cache:
        out = subprocess.run([sim] + args.split(), capture_output=True, text=True).stdout
        cache[args] = {l.split("\t")[4]: float(l.split("\t")[5]) for l in out.splitlines()}
    return cache[args]


def p6g0():
    path = os.path.join(HERE, "../../../samples/p6g0_lab_2026-11-05.txt")
    d = collections.defaultdict(list)
    for line in open(path):
        if not line.startswith("build="):
            continue
        kv = dict(x.split("=", 1) for x in line.split() if "=" in x)
        d[(kv["case"], kv["build"])].append(float(kv["mbps"]))
    label = {"yield_one_4096": "one thread, 4 KiB", "yield_sib_4096": "two SMT siblings, 4 KiB",
             "yield_core_4096": "two cores, 4 KiB", "yield_one_1048576": "one thread, 1 MiB",
             "yield_sib_1048576": "two SMT siblings, 1 MiB", "yield_core_1048576": "two cores, 1 MiB",
             "any_2": "2 queues x 4 in flight, any CPU", "any_4": "4 queues x 4", "any_8": "8 queues x 4",
             "one4": "4 queues fed on one CPU", "geli16_small": "16 kwq-geli providers, 4 KiB reads",
             "geli16_bulk": "16 kwq-geli providers, 1 MiB writes"}
    rows = []
    for (case, b), v in sorted(d.items()):
        rows.append({"case": label.get(case, case), "order": "today" if b == "A" else "sibling last",
                     "gb_per_s": statistics.median(v) / 1000, "reps": len(v), "key": f"{case}|{b}"})
    return rows


FINDINGS = [
    ("Step 0 never spread a pile-up", "two requests that landed on one core co-ran at the SMT rate for good: 26.8 against today's 41.9 GB/s", "step 0 yields to an idle core when the sibling is busy", "621bead"),
    ("L_local 0.2 ms too loose", "requests waited in series on one thread beside idle cores", "L_local 50 us, M2's tolerance", "621bead"),
    ("Stamps wrapped after 2048 s looked fresh", "a table idle for half an hour would be used again; merges stopped; a fall stayed pending", "a stamp from the future counts as old", "621bead"),
    ("The hold timer fired early", "an idle core kept advertising its last load for good", "round up and re-arm", "621bead"),
    ("Step 0's own load included the item just done", "step 0 fired on half the resubmissions", "the load without it", "621bead"),
    ("The other-threads term in the load figure", "every CPU of a busy machine looked equally loaded: 1.01M against 1.65M reads/s", "dropped", "1b08c2b"),
    ("Re-merge at once after a failed check", "a merge storm: 33000 merges/s per core", "the interval shortened to Imin/4", "1b08c2b"),
    ("Gossip on a flat 128-CPU machine", "an island ringed by 124 idle cores waited 190 us on stale, sparse tables; an idle mask knows in a microsecond", "structural; P8 instead", "-"),
    ("The idle mask under contention", "every worker writes one line at each sleep and wake: 2-10 us of line wait per placement on a07, waits up to 0.5 ms", "P8: one line per 16 CPUs", "1b08c2b"),
    ("P8 without spill, pinned producers", "12.5 ms mean wait on a07", "spill at a donor, steal wakes, early pull, avoid donors", "4c16dd9"),
    ("P8 steals re-stolen", "an island moved 22000 items four times or more", "a steal stays below a donor's backlog", "b813fde"),
    ("P8's reach on a07 (worst-case set)", "an island at 72 cores' worth of load reached 48 CPUs: 3.7 ms mean wait, 3.2M refusals, 18.6M of 24M items/s while 80 CPUs slept", "without an idle worker near, one far group's line per placement", "f04df4f"),
    ("A starved CPU never flagged itself (worst-case set)", "a core whose BULK share fell to 0 for 100 ms held its items for 95 ms: no decision point, no donor flag", "a per-CPU starvation watchdog flags a starved donor, lone items stealable", "f04df4f"),
    ("A producer-raised donor flag", "fixed the p99.9 of the collapse but not its maximum (68 ms), and nothing once the watchdog was in", "dropped", "f04df4f"),
    ("P8 left lone items behind running work", "W2a met in 328 of 555 steady runs against today's 486: a 300 us item behind a running one waited while CPUs idled (a07: 14.8 us mean, 25 % idle-while-waiting)", "a lone item counts the work ahead of it, for idle thieves only: W2a 414, A1f 441 against 383", "01ec314"),
    ("Lower donor thresholds, the work ahead for every list", "donor_us 100 and 200: W2a unchanged, more items moved twice; the work ahead for every list: short lists of a saturated machine hopped (40 items moved 4 times), and pullers passed lone items on (144)", "dropped", "01ec314"),
    ("Herding against the oracle", "the oracle fails W4 in 39 % of the main runs: M6 counts an idle core taking items one after another", "W4o, judged against the oracle's herding; a pile-up count proposed for P6g.3", "a39e42a"),
]


def main():
    main_ = load("main.tsv")
    pols = [1, 2, 3, 4, 5, 7, 8]
    # the oracle's own W4: how often the reference itself "herds" (the page's decision 5)
    oracle_w4 = [r for r in verdicts(main_, [0], "main") if r["criterion"] == "W4"]
    write("verdicts", verdicts(main_, pols, "main") + oracle_w4 +
          verdicts(load("worst.tsv"), [1, 5, 8], "worst"))
    write("families", families(main_, [0, 1, 2, 5, 7, 8]))
    write("worst_cases", worst_cases(load("worst.tsv")))
    write("ablations", ablations(load("ablate.tsv"), main_, 5) + ablations(load("ablate8.tsv"), main_, 8))
    write("sweeps", sweeps(load("sweep.tsv"), main_, 5) + sweeps(load("sweep8.tsv"), main_, 8))
    write("fuzz", fuzz(load("fuzz.tsv")))
    write("validation", validation())
    write("p6g0", p6g0())
    write("findings", [{"finding": a, "effect": b, "fix": c, "commit": d, "key": a} for a, b, c, d in FINDINGS])


if __name__ == "__main__":
    main()
