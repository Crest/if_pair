#!/usr/bin/env python3
"""summarize.py - placesim's matrix against SPEC-p6g.md section 1.

    python3 summarize.py OUT.tsv [--json REPORT.json] [--policies 1,2,5,8]

Reads matrix.py's rows (variant, scen, topo, policy, seed, metric, value)
and judges every criterion of section 1 that the simulator can judge,
per policy, against the oracle (policy 0) of the same variant, topology
and seed.  Prints a table per criterion: runs passed of runs judged, and
the worst case with its seed.
"""

import argparse
import collections
import json
import re
import statistics
import sys

NAMES = {0: "oracle", 10: "oracle-L3", 1: "today", 2: "nosteal", 3: "two-choice",
         4: "L3 arrays", 5: "gossip", 6: "random", 7: "donor", 8: "shard"}
STEADY = re.compile(r"^S(1|2|3|4|5|6|13|14|16)-")
STEPS = re.compile(r"^S(7|8|9|10|12)-")
OVER = re.compile(r"^S15-")


def load(path):
    runs = collections.defaultdict(dict)
    with open(path) as f:
        for line in f:
            c = line.rstrip("\n").split("\t")
            if len(c) != 7:
                continue
            try:
                runs[(c[0], c[2], int(c[3]), int(c[4]))][c[5]] = float(c[6])
            except ValueError:
                pass
    return runs


def big(topo):
    return topo in ("a07", "t4", "odd127")


def criteria():
    """(id, text, applies(variant, topo), judge(m, o, t) -> (ok, value, bound))."""
    def a1(m, o, t):
        b = 1.2 * o["wait_all_mean_us"]
        return m["wait_all_mean_us"] <= b + 1e-9, m["wait_all_mean_us"], b

    def a1f(m, o, t):
        b = max(1.2 * o["wait_all_mean_us"], o["wait_all_mean_us"] + 10)
        return m["wait_all_mean_us"] <= b + 1e-9, m["wait_all_mean_us"], b

    def a1t(m, o, t):
        if t is None:
            return None
        b = t["wait_all_mean_us"] * 1.05 + 1
        return m["wait_all_mean_us"] <= b, m["wait_all_mean_us"], b

    def a2(m, o, t):
        return m.get("good_frac", 1) >= 0.95, m.get("good_frac", 1), 0.95

    def a3(m, o, t):
        return m.get("xfers_per_place", 0) <= 2, m.get("xfers_per_place", 0), 2

    def w1p(m, o, t):
        b = 2 * o["wait_all_p999_us"] + 1000
        return m["wait_all_p999_us"] <= b, m["wait_all_p999_us"], b

    def w1m(m, o, t):
        b = 3 * o["wait_all_max_us"] + 2000
        return m["wait_all_max_us"] <= b, m["wait_all_max_us"], b

    def w1o(m, o, t):
        b = 0.97 * o["work_frac"]
        return m["work_frac"] >= b, m["work_frac"], b

    def w1j(m, o, t):
        return m.get("jain", 1) >= 0.95, m.get("jain", 1), 0.95

    def w2(m, o, t):
        return m.get("idle_wait_l3_frac", 0) <= 0.02, m.get("idle_wait_l3_frac", 0), 0.02

    def w2r(m, o, t, topo=None):
        vals = [v for k, v in m.items() if re.match(r"react\d+_ms", k)]
        if not vals:
            return None
        bad = [v for v in vals if v < 0 or v > 25]
        worst = max((v if v >= 0 else 1e9) for v in vals)
        return not bad, worst, 25

    def w3(m, o, t):
        if m.get("disc_n", 0) == 0:
            return None
        return m["disc_p99_us"] <= 2000, m["disc_p99_us"], 2000

    def w3h(m, o, t):
        worst, ok = 0, True
        for k, v in m.items():
            g = re.match(r"disc_hop(\d+)_max_us", k)
            if g and int(g.group(1)) > 0:
                b = int(g.group(1)) * 8000
                if v > b:
                    ok = False
                worst = max(worst, v / b)
        if worst == 0:
            return None
        return ok, worst, 1.0

    def w4(m, o, t):
        return m.get("herd_p999", 0) <= 8, m.get("herd_p999", 0), 8

    def w4o(m, o, t):
        b = max(8, o.get("herd_p999", 0))
        return m.get("herd_p999", 0) <= b, m.get("herd_p999", 0), b

    def w5(m, o, t):
        return m.get("moved2p_frac", 0) <= 0.01, m.get("moved2p_frac", 0), 0.01

    def w5x(m, o, t):
        return m.get("moved4p", 0) == 0, m.get("moved4p", 0), 0

    def w5h(m, o, t):
        return m.get("handoffs_per_s_core", 0) <= 1, m.get("handoffs_per_s_core", 0), 1

    def w6(m, o, t, ns=None):
        if ns is None:
            return None
        b = ns["scheme_cpu_frac"] + 0.005
        return m["scheme_cpu_frac"] <= b, m["scheme_cpu_frac"], b

    def w7(m, o, t):
        return m.get("expired_used", 0) == 0, m.get("expired_used", 0), 0

    def w8(m, o, t):
        return m.get("violations", 0) == 0, m.get("violations", 0), 0

    steady = lambda v, tp: bool(STEADY.match(v))
    every = lambda v, tp: True
    steps = lambda v, tp: bool(STEPS.match(v))
    over = lambda v, tp: bool(OVER.match(v))
    steady_over = lambda v, tp: v.startswith("S15-steady")
    return [
        ("A1", "mean wait <= 1.2 x oracle (steady)", steady, a1),
        ("A1f", "mean wait <= max(1.2 x oracle, oracle + 10 us) (steady)", steady, a1f),
        ("A1t", "mean wait no worse than today's (steady, +5 % +1 us)", steady, a1t),
        ("A2", ">= 95 % of placements good (steady)", steady, a2),
        ("A3", "<= 2 line transfers per placement (steady)", steady, a3),
        ("W1a", "wait p99.9 <= 2 x oracle + 1 ms", lambda v, tp: not OVER.match(v), w1p),
        ("W1b", "wait max <= 3 x oracle + 2 ms", lambda v, tp: not OVER.match(v), w1m),
        ("W1c", "overload: work >= 97 % of the oracle's", over, w1o),
        ("W1d", "overload: Jain >= 0.95", over, w1j),
        ("W2a", "idle-while-waiting <= 2 % (steady)", steady, w2),
        ("W2b", "after a step: good and M3 recovered within 25 ms", steps, w2r),
        ("W3a", "discovery p99 <= 2 ms", every, w3),
        ("W3b", "discovery max <= h x Imax", every, w3h),
        ("W4", "herding p99.9 <= 8", every, w4),
        ("W4o", "herding p99.9 <= max(8, the oracle's)", every, w4o),
        ("W5a", "<= 1 % of items moved twice or more", every, w5),
        ("W5b", "no item moved 4 times or more", every, w5x),
        ("W5c", "steady overload: <= 1 hand-off/s per core", steady_over, w5h),
        ("W6", "steady overload: scheme CPU <= no-stealing's + 0.5 %", steady_over, w6),
        ("W7", "no expired entry used", every, w7),
        ("W8", "no violation (conservation, asserts)", every, w8),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("--json")
    ap.add_argument("--policies", default="1,2,3,4,5,7,8")
    a = ap.parse_args()
    runs = load(a.tsv)
    pols = [int(x) for x in a.policies.split(",")]
    crit = criteria()
    res = {c[0]: {p: {"n": 0, "pass": 0, "worst": None} for p in pols} for c in crit}
    per = collections.defaultdict(lambda: collections.defaultdict(list))
    for (v, topo, p, s), m in runs.items():
        if "violations" not in m:
            continue
        per[(v, topo, p)]["_seeds"].append(s)
        for k, x in m.items():
            per[(v, topo, p)][k].append(x)
        if p not in pols:
            continue
        o = runs.get((v, topo, 0, s))
        if o is None or "wait_all_mean_us" not in o or "wait_all_mean_us" not in m:
            continue
        t = runs.get((v, topo, 1, s))
        ns = runs.get((v, topo, 2, s))
        for cid, text, applies, judge in crit:
            if not applies(v, topo):
                continue
            try:
                if cid == "A1t":
                    r = judge(m, o, t)
                elif cid == "W6":
                    r = judge(m, o, t, ns)
                else:
                    r = judge(m, o, t)
            except KeyError:
                continue
            if r is None:
                continue
            ok, val, bound = r
            e = res[cid][p]
            e["n"] += 1
            e["pass"] += bool(ok)
            badness = (val / bound if bound else val) if cid not in ("W1c", "A2", "W1d") else (
                bound / val if val else 1e9)
            if not ok and (e["worst"] is None or badness > e["worst"]["badness"]):
                e["worst"] = {"variant": v, "topo": topo, "seed": s, "value": val,
                              "bound": bound, "badness": badness}
    # print
    print(f"{'criterion':6s} {'':54s} " + " ".join(f"{NAMES[p]:>11s}" for p in pols))
    for cid, text, _, _ in crit:
        cells = []
        for p in pols:
            e = res[cid][p]
            cells.append(f"{e['pass']:>5d}/{e['n']:<5d}" if e["n"] else f"{'-':>11s}")
        print(f"{cid:6s} {text[:54]:54s} " + " ".join(cells))
    print()
    for cid, text, _, _ in crit:
        for p in pols:
            w = res[cid][p]["worst"]
            if w:
                print(f"{cid:5s} {NAMES[p]:10s} worst: {w['variant']} {w['topo']} seed {w['seed']}: "
                      f"{w['value']:.4g} against {w['bound']:.4g}")
    if a.json:
        summ = {}
        for (v, topo, p), d in per.items():
            summ.setdefault(v, {}).setdefault(topo, {})[str(p)] = {
                k: {"med": statistics.median(x), "max": max(x), "min": min(x)}
                for k, x in d.items() if k != "_seeds"}
        with open(a.json, "w") as f:
            json.dump({"criteria": [{"id": c[0], "text": c[1]} for c in crit],
                       "names": {str(k): v for k, v in NAMES.items()},
                       "verdicts": {cid: {str(p): e for p, e in d.items()} for cid, d in res.items()},
                       "summary": summ}, f)


if __name__ == "__main__":
    main()
