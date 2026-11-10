#!/usr/bin/env python3
"""matrix.py - placesim's scenario matrix (SPEC-p6g.md S3.5).

    python3 matrix.py [-j JOBS] [-s SEEDS] [--only REGEX] [--set NAME] [--list] OUT.tsv

Each run is one line of placesim key=value arguments; its TSV rows are
appended to OUT.tsv with a leading column naming the variant.  Runs that
are already in OUT.tsv (same variant, topology, policy, seed) are
skipped, so an interrupted matrix resumes.  Sets: "main" (every scenario
with every policy), "ablate" (P5's ablations on the scenarios that
exercise them), "sweep" (S18, the constants), "worst" (the worst-case
scenarios with 100 seeds), "fuzz" (S17).
"""

import argparse
import concurrent.futures as cf
import itertools
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "placesim")

TOPOS = ["guest", "lab", "a07", "t4"]
POLICIES = [0, 1, 2, 3, 4, 5, 7, 8]
# cores, threads per core; a thread beside a busy sibling runs at 0.61 (P6g.0)
SHAPE = {"guest": (4, 1), "lab": (8, 2), "a07": (128, 1), "t4": (64, 2),
         "odd6": (3, 2), "odd12": (6, 2), "odd127": (127, 1)}


def tcap(topo):
    """A thread's capacity when every thread is busy, in full-speed threads."""
    return 0.61 if SHAPE[topo][1] > 1 else 1.0


def capacity(topo):
    n, t = SHAPE[topo]
    return n * t * tcap(topo)


def scale(topo, args):
    """Values written @x are per-thread loads: x times the thread's capacity."""
    return re.sub(r"@([0-9.]+)", lambda m: f"{float(m.group(1)) * tcap(topo):.4g}", args)
SMALL = "small_us=3"
LARGE = "small_us=300"
MIX = "small_us=3 large_us=300 p_large=0.1"
PARETO = "small_us=30 pareto=1.5"
MIXES = {"small": SMALL, "large": LARGE, "mix": MIX, "pareto": PARETO}
GELI_SMALL = "small_us=1.1 other_us=4.6"
GELI_BULK = "small_us=1230 other_us=660"


def dur(topo, ms=None):
    """Durations: shorter on the 128-thread machines."""
    big = topo in ("a07", "t4", "odd127")
    if ms is not None:
        return f"dur_ms={ms} warm_ms=20"
    return "dur_ms=70 warm_ms=20" if big else "dur_ms=150 warm_ms=30"


def variants_main():
    """(variant, topos, args) of the main set."""
    v = []
    for rho in (0.3, 0.7, 0.95):
        for m, a in MIXES.items():
            v.append((f"S1-uniform-rho{rho}-{m}", TOPOS, f"scen=uniform rho=@{rho} {a}"))
    for n in (16, 128):
        v.append((f"S2-geli-n{n}-small", ["lab"], f"scen=geli nprov={n} {GELI_SMALL}"))
        v.append((f"S2-geli-n{n}-bulk", ["lab"],
                  f"scen=geli nprov={n} {GELI_BULK} dur_ms=600 warm_ms=100"))
    v.append(("S2-geli-n128-q4-small", ["lab"], f"scen=geli nprov=128 inflight=4 {GELI_SMALL}"))
    for n in (1, 2, 4, 8):
        v.append((f"S2-crypto-q{n}x4", ["lab", "guest"],
                  f"scen=crypto nprov={n} inflight=4 small_us=191 dur_ms=300 warm_ms=50"))
    def fits(topo, k, f):
        """An island of k cores at f x their capacity within 80 % of the machine."""
        n, t = SHAPE[topo]
        return k < n and k * t * f * tcap(topo) <= 0.8 * capacity(topo)

    for k in (1, 2, 4, 9, 16, 32):
        for f in (2, 4, 8):
            topos = [t for t in TOPOS if fits(t, k, f)]
            if not topos:
                continue
            for m in ("small", "large"):
                v.append((f"S3-island-k{k}-x{f}-{m}", topos,
                          f"scen=island k={k} factor=@{f} layout=0 {MIXES[m]}"))
            if f == 4:
                # across the torus's wrap edge: from the last row and column
                v.append((f"S3-island-k{k}-x4-wrap", [t for t in topos if t != "guest"],
                          f"scen=island k={k} factor=@4 layout=-1 {SMALL}"))
    for k, topos in ((1, ["lab", "a07", "t4"]), (4, ["lab", "a07", "t4"]), (9, ["a07", "t4"])):
        for m in ("small", "large"):
            # the sea a little above its capacity, the whole at 90 %
            v.append((f"S4-idleisland-k{k}-{m}", topos, f"scen=idleisland k={k} factor=SEA{k} {MIXES[m]}"))
    for lay in (0, 1, 2):
        v.append((f"S5-stripes-l{lay}", ["lab", "a07", "t4"], f"scen=stripes layout={lay} factor=@1.5 {SMALL}"))
    for m in ("small", "large"):
        v.append((f"S6-deadband-{m}", ["lab", "a07", "t4"], f"scen=deadband factor=@1.5 {MIXES[m]}"))
    for gap in (10, 100, 1000, 10000):
        for f in (0.5, 0.9, 1.5):
            v.append((f"S7-return-gap{gap}-x{f}", ["lab", "a07"],
                      f"scen=return gap_ms={gap} factor=@{f} k=0 {SMALL} dur_ms={gap + 130}"))
        # the load returns in an island: lab's one core at 4x, a07's four
        v.append((f"S7-return-gap{gap}-island", ["lab", "a07"],
                  f"scen=return gap_ms={gap} factor=@4 k=ISLAND {SMALL} dur_ms={gap + 130}"))
    v.append(("S8-stop", ["lab", "a07", "t4"], f"scen=stop k=4 rho=@0.9 {SMALL}"))
    for per in (0.5, 1, 4, 16):
        v.append((f"S9-moving-p{per}", ["lab", "a07"], f"scen=moving period_ms={per} factor=@2 {SMALL}"))
    for per in (0.5, 2, 8, 20):
        for lay in (0, 1):
            v.append((f"S10-onoff-p{per}-l{lay}", ["lab", "a07"],
                      f"scen=onoff period_ms={per} layout={lay} rho=@0.4 {SMALL}"))
    for fx, name in ((0, "fixed"), (-1, "any")):
        v.append((f"S11-flash-{name}-100us", ["guest", "lab", "a07"],
                  f"scen=flash burst=2000 k=1 fixed={fx} small_us=100"))
        v.append((f"S11-flash-{name}-3us", ["lab", "a07"], f"scen=flash burst=2000 k=1 fixed={fx} small_us=3"))
    v.append(("S11-flash-many", ["lab", "a07"], f"scen=flash burst=50 k=128 small_us=30"))
    for drop in (0.5, 0.1, 0):
        for ms in (1, 10, 100):
            v.append((f"S12-collapse-s{drop}-{ms}ms", ["lab", "a07"],
                      f"scen=collapse share_drop={drop} drop_ms={ms} rho=@0.6 small_us=100 "
                      f"dur_ms={ms + 120}"))
    for lay in (0, 1):
        v.append((f"S13-hogs-l{lay}", ["lab", "a07", "t4"],
                  f"scen=hogs layout={lay} period_ms=10 rho=@0.3 {MIX}"))
    for f in (0.3, 0.55, 0.9):
        for m in ("small", "large"):
            v.append((f"S14-smt{f}-{m}", ["lab", "t4"],
                      f"scen=smt smt_bulk={f} rho={0.5 * f:.3f} {MIXES[m]}"))
    for rho in (1.0, 1.2, 2.0):
        v.append((f"S15-full-rho{rho}", ["lab", "a07", "t4"], f"scen=full rho=@{rho} small_us=100"))
        v.append((f"S15-steady-rho{rho}", ["lab", "a07", "t4"], f"scen=full layout=1 rho=@{rho} small_us=100"))
    for lay in (0, 1):
        v.append((f"S16-fixed-l{lay}", ["lab", "a07"], f"scen=fixed layout={lay} rho=@0.3 small_us=100"))
    return v


ABLATIONS = ["noself=1", "nosmt=1", "nocheck=1", "norand=1", "symhyst=1", "fixival=1",
             "nosplit=1", "norepair=1", "handoff=0", "spill=0", "table=2", "table=8",
             "shape=1", "shape=2", "checks=1", "noremerge=1", "remerge=1", "tomb=1",
             "busyterm=1", "handoff_n=1"]
ABLATIONS8 = ["spill=0", "donor_pull=0", "donor_cascade=0", "donor_wakes=0", "donor_avoid=0",
              "donor_bound=0", "probe_groups=0", "far_probe=0", "watch_us=0", "donor_lone=0"]
ABLATE_ON = r"^(S1-uniform-rho0\.7|S2-crypto-q2|S3-island-k(1|4|9)-x4|S4-idleisland-k4|S6-|S7-return-gap1000-x1\.5|S11-flash|S12-collapse-s0-10ms|S14-smt0\.55|S15-full-rho1\.2|S16-fixed-l0)"
# P8's far probe and starvation watchdog: the islands that overflow their neighbourhood, a long collapse
ABLATE8_ON = ABLATE_ON[:-1] + r"|S3-island-k9-x8-small|S3-island-k16-x4-small|S12-collapse-s0-100ms)"


def expand(topo, args):
    def sea(m):
        k = int(m.group(1))
        n, t = SHAPE[topo]
        return f"{0.9 * capacity(topo) / ((n - k) * t):.4g}"
    args = re.sub(r"SEA([0-9]+)", sea, args)
    args = args.replace("k=ISLAND", "k=1" if SHAPE[topo][0] <= 16 else "k=4")
    return scale(topo, args)


def runs(setname, seeds):
    out = []
    main = [(n, ts, a) for n, ts, a in variants_main()]
    if setname == "main":
        for name, topos, args in main:
            for t, p, s in itertools.product(topos, POLICIES, range(1, seeds + 1)):
                out.append((name, t, p, s, f"topo={t} policy={p} seed={s} {dur(t)} {expand(t, args)}"))
            if any(t == "t4" for t in topos):
                for s in range(1, seeds + 1):
                    out.append((name, "t4", 10, s, f"topo=t4 policy=10 seed={s} {dur('t4')} {expand('t4', args)}"))
    elif setname == "ablate":
        for name, topos, args in main:
            if not re.match(ABLATE_ON, name):
                continue
            for t, a, s in itertools.product(topos, ABLATIONS, range(1, seeds + 1)):
                out.append((f"{name}+{a}", t, 5, s, f"topo={t} policy=5 seed={s} {dur(t)} {expand(t, args)} {a}"))
    elif setname == "sweep":
        sweeps = {
            "imin_us": (250, 500, 2000, 4000), "imax_us": (2000, 4000, 16000, 32000),
            "texp_us": (8000, 16000, 64000, 128000), "hold_us": (0, 25, 50, 200, 400),
            "h_us": (1000, 1500, 3000, 4000), "l_us": (250, 1000), "ladv_us": (250, 1000),
            "llocal_us": (10, 25, 100, 200), "csmall_us": (5, 50, 100),
        }
        base = [m for m in main if re.match(r"^(S1-uniform-rho0\.7-mix|S3-island-k4-x4-small|"
                                             r"S4-idleisland-k4-small|S7-return-gap1000-x1\.5|"
                                             r"S9-moving-p1|S10-onoff-p2-l1|S12-collapse-s0-10ms|"
                                             r"S15-full-rho1\.2)$", m[0])]
        for name, topos, args in base:
            for k, vals in sweeps.items():
                for val, t, s in itertools.product(vals, [x for x in topos if x in ("lab", "a07")],
                                                  range(1, seeds + 1)):
                    out.append((f"{name}+{k}={val}", t, 5, s,
                                f"topo={t} policy=5 seed={s} {dur(t)} {expand(t, args)} {k}={val}"))
    elif setname == "worst":
        pat = r"^(S3-island-k(1|4|9|16)-x(4|8)-small|S3-island-k(1|4)-x4-wrap|S4-idleisland-k(1|4)-small|S6-deadband-small|S7-return-gap10000-(x1\.5|island)|S9-moving-p1|S10-onoff-p2-l0|S11-flash-(fixed|any)-100us|S11-flash-many|S12-collapse-s0-(10|100)ms|S15-(full|steady)-rho(1\.2|2\.0))$"
        for name, topos, args in main:
            if not re.match(pat, name):
                continue
            for t, p, s in itertools.product([x for x in topos if x in ("lab", "a07")], (0, 1, 5, 8),
                                             range(101, 101 + seeds)):
                out.append((name, t, p, s, f"topo={t} policy={p} seed={s} {dur(t)} {expand(t, args)}"))
    elif setname == "ablate8":
        # P8's parts, one at a time, on the scenarios that exercise them
        for name, topos, args in main:
            if not re.match(ABLATE8_ON, name):
                continue
            for t, a, s in itertools.product([x for x in topos if x in ("lab", "a07", "t4")],
                                             ABLATIONS8, range(1, seeds + 1)):
                out.append((f"{name}+{a}", t, 8, s, f"topo={t} policy=8 seed={s} {dur(t)} {expand(t, args)} {a}"))
    elif setname == "sweep8":
        sweeps = {"donor_us": (200, 1000, 2000), "group": (4, 8, 32, 64), "probe_groups": (0, 1, 4),
                  "steal_max": (16, 256), "steal_scan": (8, 32, 128),
                  "far_us": (-1, 20, 100), "far_probe": (2,), "watch_us": (250, 4000)}
        base = [m for m in main if re.match(r"^(S1-uniform-rho0\.7-(small|mix)|S3-island-k(4|9)-x4-small|"
                                             r"S4-idleisland-k4-small|S7-return-gap1000-x1\.5|"
                                             r"S11-flash-fixed-100us|S15-full-rho1\.2|S16-fixed-l0|"
                                             r"S3-island-k9-x8-small|S12-collapse-s0-100ms)$", m[0])]
        for name, topos, args in base:
            for k, vals in sweeps.items():
                for val, t, s in itertools.product(vals, [x for x in topos if x in ("lab", "a07", "t4")],
                                                  range(1, seeds + 1)):
                    out.append((f"{name}+{k}={val}", t, 8, s,
                                f"topo={t} policy=8 seed={s} {dur(t)} {expand(t, args)} {k}={val}"))
    elif setname == "fuzz":
        rng = random.Random(17)
        for s in range(1, seeds + 1):
            name, topos, args = rng.choice(main)
            t = rng.choice(topos + ["odd6", "odd12", "odd127"])
            extra = " ".join(rng.sample([
                f"imin_us={rng.choice((250, 1000, 4000))}", f"imax_us={rng.choice((2000, 8000, 32000))}",
                f"texp_us={rng.choice((8000, 32000, 128000))}", f"hold_us={rng.choice((0, 100, 400))}",
                f"h_us={rng.choice((1000, 2000, 4000))}", f"table={rng.choice((2, 4, 8))}",
                f"shape={rng.choice((0, 1, 2))}", f"smt_bulk={rng.choice((0.3, 0.61, 0.9))}",
                f"absent={rng.choice((0, 0, 5, 7))}", f"wl_long_us={rng.choice((2, 9, 40))}"], 4))
            # S3's k=32 does not fit the odd machines; an idle island must leave a sea
            m = re.search(r"^S4-idleisland-k([0-9]+)-", name)
            if t.startswith("odd") and ("S3-island-k32" in name or
                                        (m and int(m.group(1)) >= SHAPE[t][0])):
                continue
            for p in (5, 8):
                out.append((f"S17-fuzz-{s}:{name}", t, p, 1000 + s,
                            f"topo={t} policy={p} seed={1000 + s} {dur(t)} {expand(t, args)} {extra}"))
    return out


def done_keys(path):
    keys = set()
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                c = line.rstrip("\n").split("\t")
                if len(c) >= 7 and c[5] == "violations":
                    keys.add((c[0], c[3], c[2], c[4]))
    return keys


def one(job):
    name, t, p, s, args = job
    try:
        r = subprocess.run(["nice", "-n", "10", SIM] + args.split(), capture_output=True,
                           text=True, timeout=3600)
    except subprocess.TimeoutExpired:
        return name, t, p, s, "", "timeout"
    rows = "".join(f"{name}\t{line}\n" for line in r.stdout.splitlines() if line)
    return name, t, p, s, rows, r.stderr.strip() if r.returncode == 0 else f"exit {r.returncode}: {r.stderr[-300:]}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("-j", type=int, default=6)
    ap.add_argument("-s", type=int, default=5)
    ap.add_argument("--set", default="main")
    ap.add_argument("--only", default=None)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    jobs = runs(a.set, a.s)
    if a.only:
        jobs = [j for j in jobs if re.search(a.only, j[0]) or re.search(a.only, j[4])]
    have = done_keys(a.out)
    todo = [j for j in jobs if (j[0], str(j[2]), str(j[1]), str(j[3])) not in have]
    if a.list:
        for j in todo:
            print(j[4])
        return
    print(f"{len(jobs)} runs, {len(jobs) - len(todo)} done, {len(todo)} to do", file=sys.stderr)
    errs = 0
    with open(a.out, "a") as f, cf.ThreadPoolExecutor(a.j) as ex:
        for i, (name, t, p, s, rows, err) in enumerate(ex.map(one, todo), 1):
            f.write(rows)
            f.flush()
            if err:
                errs += 1
                print(f"{name} {t} p{p} s{s}: {err[:300]}", file=sys.stderr)
            if i % 200 == 0:
                print(f"{i}/{len(todo)}", file=sys.stderr)
    print(f"finished, {errs} runs with messages", file=sys.stderr)


if __name__ == "__main__":
    main()
