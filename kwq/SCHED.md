# kwq worker scheduler: specification

Written 2026-09-28, before P1 is implemented; terminology per GLOSSARY.md.
This document is normative for the part of `kwq_worker.c` that decides
which (queue, CPU) list a worker serves next, for how long, and when the
worker gives the CPU away.  KWQ.md S5 states the goals and S15 the
literature; where S5 and this document differ, this document wins and
S5 is to be corrected.  P1 in PLAN.txt implements this specification and
its test plan (S10) is P1's exit criterion.

Contents: S1 prior art and what is taken from it; S2 definitions and
units; S3 state; S4 algorithm; S5 handler contract; S6 yielding;
S7 properties and bounds; S8 parameters; S9 observability; S10 test
plan; S11 alternatives rejected; S12 open questions; S13 numeric
robustness (every quantity, its width, its wrap behaviour); S14
pathological corner cases and what the algorithm does about each; S15
cost model: what the bookkeeping costs per item, pass and round, and
how that compares with scheduling kernel threads.

## 1. Prior art

Rows that give a source path or document were read there; the DRR and
BFQ papers, the Xen credit, CFS bandwidth and CFS-to-EEVDF history rows,
and the NAPI default values 300 and 2000 are from the literature and
memory and were not re-verified in this project.  The last column is
what this specification takes.

| mechanism | unit of service | fairness | latency bound | anti-gaming | taken |
|---|---|---|---|---|---|
| DRR (Shreedhar & Varghese, SIGCOMM 1995 / ToN 1996) | bytes; quantum >= max packet for O(1) | deficit counter per queue, +quantum per round, serve while deficit >= packet, deficit reset to 0 when the queue empties | one round: N x quantum | none needed (packets are the unit) | the deficit counter, the per-round refill, the reset on empty, the term "round" |
| dummynet WRR, `sys/netpfil/ipfw/dn_sched_rr.c` (FreeBSD 15) | bytes | `credit += quantum` when the head packet does not fit, `credit -= len` when served; `quantum = q_bytes x weight` | one round | none | `quantum x weight` as the per-queue refill |
| fq_codel, Linux `net/sched/sch_fq_codel.c` (master, 2026) | bytes; `quantum = clamp(psched_mtu, 256, 64K)` | DRR over flows | new flows served first for one quantum | a **new** flow that empties is moved to the tail of `old_flows` if `old_flows` is non-empty, else deleted; deficit `<= 0` -> `+= quantum` and move to `old_flows` tail | the two lists (new, old); the empty-new-flow rule as the seed of S4's grace rule |
| CAKE DRR++, Linux `net/sched/sch_cake.c` (master, 2026) | bytes, `quantum` scaled per host (`quantum_div[host_load]`) | DRR with sparse (new), bulk (old) and **decaying** sets | sparse flows first, with a full quantum "sparse boost" | a bulk flow that empties becomes *decaying* and keeps its set membership; a packet arriving while decaying returns it to bulk **without** the boost (`CAKE_SET_SPARSE_WAIT`); only a flow that has fully decayed is deleted and can be sparse again | the decaying idea, as S4's *grace period*: a queue that leaves the ring is not eligible for the boost until it has been idle for a full round |
| Linux NAPI, `net/core/dev.c`, `Documentation/admin-guide/sysctl/net.rst` | packets **and** time: `dev_weight` 64 per poll, `netdev_budget` 300 and `netdev_budget_usecs` 2000 per softirq cycle, "whichever comes first"; leftovers go to `ksoftirqd` | round robin over NAPI instances | one softirq cycle <= 2 ms | none | the time bound as the primary limit (item counts stretch with item size: if_pair's batch=64 at 64 KB overran a tick); **not** taken: the ksoftirqd fallback (KWQ.md S5 "no punting") |
| illumos squeues, `uts/common/inet/squeue.c` | time: `squeue_drain_ms = 20` for non-worker drains; the worker thread drains without limit | none across squeues (one per CPU) | 20 ms for interrupt-context drains | none | confirmation that a time budget is the accepted unit for protocol-processing drains; 20 ms is far above what NET wants |
| FreeBSD netisr, `sys/net/netisr.c` | none: `netisr_process_workstream_proto()` swaps the whole per-protocol list and runs it to completion | none | none (queue limit only, `net.isr.maxqlimit`) | none | the list swap (kwq's pass); the gap it leaves is why kwq exists |
| FreeBSD iflib, `sys/net/iflib.c` | descriptors: `iflib_rxeof(rxq, budget)`, `budget = ifc_sysctl_rx_budget`, default 16 | none | one poll | none | nothing beyond confirming count budgets are the tree's habit |
| FreeBSD ULE, `sys/kern/sched_ule.c` | time: `sched_slice = stathz/10` (~94 ms), divided by load down to ~16 ms; timeshare class only; an ithread-class thread that runs a full slice is demoted by `RQ_PPQ` and preempted | interactivity score; not proportional share | slice | n/a | the fact that a kernel-priority worker is **never** time-sliced by ULE, so the quantum below is the only bound on a pass |
| FreeBSD taskqueue, `sys/kern/subr_taskqueue.c` | none | none | none | n/a | nothing |
| libdispatch (swift-corelibs 6.1.1, `src/queue.c`; ../DISPATCH.md S4) | none: a serial queue is pushed onto its root queue once when it becomes non-empty and the thread that takes it drains it to empty (exits only on empty, suspend, retarget, a QoS override, or Darwin's 50 ms pool "narrowing" check) | none within a QoS band; strict priority between the 12 root queues; the pool asks for **more threads** when queues wait, so a long drain costs a thread, not the other queues' turn | none | n/a | the negative lesson: GCD can omit fairness because its workers are preemptible timeshare threads under the kernel scheduler; a kernel pool at `PI_NET` has no scheduler above it doing that job, so kwq must carry the quantum, the round and the yield itself |
| if_pair worker, `../if_pair.c` `pair_task_deferred()` | packets (`net.link.pair.batch` = 64) plus a tick guard (`pair_ticked()`) | none (one queue per worker) | one tick + one packet for callouts | n/a | the tick guard and the restore-priority-after-yield rule; **not** the count budget |
| Linux EEVDF, `Documentation/scheduler/sched-eevdf.rst` | virtual time; per-task *slice* (request) separate from weight | lag: service owed relative to fair share | earliest virtual deadline; shorter slice = lower latency at equal share | lag is preserved across sleeps and decays, so brief sleeps cannot reset a negative lag | the observation that the slice and the weight are different knobs (S8: `quantum` per class is the slice, `weight` per queue the share) and that a negative balance must survive an idle period briefly (S4 grace rule) |
| BFQ (Valente & Checconi 2010; Linux `block/bfq-iosched.c`) | sectors: a *budget* per queue, adapted to what the queue used | WF2Q+ over budgets | one budget | budget shrinks for queues that do not use it | the vocabulary "budget" for the amount a pass may consume; adaptive budgets are S11 (rejected for now) |
| Xen credit scheduler (Xen 3.x `xen/common/sched_credit.c`) | CPU time: credits debited per 10 ms tick, 30 ms accounting period, weights and caps per domain | credits = a deficit counter for CPUs | a waking vCPU gets `BOOST` priority ahead of `UNDER` vCPUs | BOOST was abused by vCPUs that woke often; credit2 replaced it with a rate-limit and a "ratelimit_us" minimum run | confirmation that a "new arrival goes first" rule for CPU work needs the same anti-gaming as for packets (S4.6), and that a per-consumer **cap** is a legitimate non-work-conserving policy (S6.3) |
| Linux CFS bandwidth controller (`kernel/sched/fair.c`, `cpu.cfs_quota_us`/`cpu.cfs_period_us`, 2011) | CPU time: a quota per period per group; a group that exhausts it is *throttled* until the period ends | proportional share by weight, plus the cap | n/a | the quota is refilled per period, unused quota does not accumulate beyond the period ("burst" was added later, bounded) | the shape of S6.3's cap (busy fraction over a window, then sleep) and the carry cap in S4.2: unused service carries at most one refill |
| Linux CFS sleeper fairness -> EEVDF (`kernel/sched/fair.c`, 2007 -> 2023) | CPU time: a waking task was placed at `min_vruntime - sched_latency/2` (a boost) | vruntime | the boost gave wakers low latency | the boost was gamed by tasks sleeping briefly; EEVDF keeps *lag* across sleeps and decays it, so a brief sleep no longer resets a negative balance | S4.6's grace rule is the same repair applied to queues: an idle period shorter than a round does not erase the queue's ring membership |

Two facts from this survey shape the rest: every deployed round-robin
scheduler that serves *flows* found it necessary to add a "new" list
with a boost and then a rule against abusing the boost (fq_codel's
move-to-old, CAKE's decaying set); and every system that drains
protocol work in kernel threads bounds a drain by *time* when it cares
about latency (NAPI, squeues), by *count* when it does not (netisr,
iflib, taskqueue).

### 1a. Where CPU work differs from packets on a link

Most of the table schedules bytes onto a link.  kwq schedules CPU time
among kernel clients; packets are one kind of client work.  The
differences below are the ones that change the algorithm, each with the
place in this document that accounts for it.  The packet-fairness that
falls out for pseudo-interfaces is a side effect, not the design goal.

1. **The cost of a unit of work is unknown until it has run.**  A packet
   has a length; DRR serves a packet only when the deficit covers it and
   never overruns.  A pass costs what the handler makes it cost.  kwq can
   only charge after the fact (S4.3), so the quantum is cooperative
   (S5), an overrun is a first-class, counted event, and the *park*
   penalty replaces DRR's "wait until the deficit covers the packet".
   DRR's precondition "quantum >= largest packet" becomes the
   recommendation `Q >= c_max` (S8): correctness (B3, B8) holds without
   it, latency of other queues (B1, B5) needs it.
2. **The resource is shared with threads kwq does not schedule.**  A link
   scheduler owns its link.  A kwq worker shares its CPU with ithreads,
   softclock, other kwq classes, and user threads, arbitrated by ULE
   priority, and ULE never time-slices a kernel-priority thread (S1,
   ULE row).  Hence the end-of-round and tick-guard yields (S6.1), the
   fixed yield priority (S6.2), and the cap (S6.3): all *non-work-
   conserving* by design, which no link scheduler is.
3. **Between classes there is strict priority, not fairness.**  NET
   (`PI_NET`), BULK (`PI_SOFT`) and BLOCKING (`PUSER`) are three workers
   per CPU at fixed priorities; a saturated NET worker starves BULK on
   that CPU except at yields.  This is deliberate (KWQ.md S1) and is why
   the DRR of this document is *intra-class*: fairness among queues of
   one class on one CPU.
4. **Charge CPU time, not wall time.**  A pass measured with two
   `cpu_ticks()` reads includes time during which the worker was
   preempted by an ithread, and for BLOCKING, time asleep.  A CPU
   scheduler charges what the thread ran: the kernel keeps
   `td_runtime` (updated in `mi_switch()`) and `pc_switchtime` (the
   `cpu_ticks()` at the last switch-in), so the worker's own CPU time at
   any instant is `td_runtime + (cpu_ticks() - PCPU_GET(switchtime))`,
   readable by the thread itself without a lock.  S4.3 uses that for the
   deficit and `kwq_budget_left()`; `kc_cycles` keeps the wall-time
   view for latency diagnosis.  This also answers S12's BLOCKING
   question.
5. **The producer chooses the "link".**  A link scheduler has one link;
   kwq has one scheduler per (class, CPU) and the client picks the CPU per
   item (KWQ.md S3).  There is no cross-CPU fairness or balancing in the
   scheduler; imbalance is the client's steering to fix, and BULK
   stealing (P6) is the only cross-CPU mechanism.  Cache affinity, which
   no link scheduler has, is why the ring is per CPU and the worker is
   bound.
6. **Queues are not flows.**  A kwq queue is one client's per-CPU list of
   heterogeneous work (packets of many flows, notifiers, crypto jobs).
   Fairness is between clients (queues), weighted by `w`, and never
   between flows inside a client; a client wanting per-flow fairness
   builds it on top (or uses one queue per flow-set, S8).  This is why
   `weight` is a client declaration and there is no host-isolation-like
   scaling of quanta.
7. **Signals are not packets.**  Notifiers carry no payload and their
   handler cost is whatever the client's state demands; they are charged
   like items (S4.3) and are the case where "cost unknown until run" is
   most acute.
8. **No preemption, no interrupt-driven budget.**  Dataplane schedulers
   preempt at microsecond granularity with interrupts; CPU schedulers
   preempt on ticks.  A kwq worker cannot be interrupted by kwq, so the
   only tools are the cooperative budget, the parking penalty and the
   yield.  The `overruns` counter names the client that needs to check
   its budget.

## 2. Definitions and units

- **Pass**: one swap-out of a (queue, CPU) list followed by one handler
  invocation per notifier and one for the item list (kwq_worker.c
  `kwq_pass()`).  A pass is the unit of scheduling; the scheduler never
  interrupts a handler.
- **Round**: one traversal of the worker's ring with new-list passes
  interleaved (S4.2).  Rounds are the unit of refill and of yielding.
- **Quantum `Q`**: nanoseconds, per class, `kern.kwq.<class>.quantum_us`
  x 1000.  Defaults: NET 200 us, BULK 1 ms, BLOCKING 5 ms (S8).
- **Weight `w`**: per queue, 1..8, default 1, from `kwq_params.weight`.
  A queue's refill is `Qw = Q x w`.
- **Deficit `D`**: signed nanoseconds per (queue, CPU); the service the
  queue may still consume.  Consumer-private.
- **Budget**: the value of `D` at the start of a pass; what
  `kwq_budget_left()` counts down from.
- **Service `S`**: CPU time the worker itself consumed in passes:
  `td_runtime + (cpu_ticks() - PCPU_GET(switchtime))` sampled at pass
  start and end (S1a.4), converted to ns.  Excludes time the worker was
  preempted or asleep.
- **Boost**: a fresh `D = Qw` given to a queue when it enters the new
  list.
- **Grace period**: the interval after a queue leaves the ring during
  which a re-arrival puts it back on the ring instead of the new list
  (S4.6).
- **`c_max`**: the longest single item a client's handler runs without
  checking `kwq_budget_left()`; a client property, not a kwq parameter.

## 3. State

Per worker (`struct kwq_worker`, one per (class, CPU)):

| field | writer | meaning |
|---|---|---|
| `kw_new` | producers (doorbell) and the worker, under `kw_mtx` | queues that became non-empty; served first |
| `kw_active` | producers (grace rule, S4.1) and the worker, under `kw_mtx` | the ring, in round-robin order |
| `kw_nactive` | producers and the worker, under `kw_mtx` | ring length, fixes the number of ring entries a round visits |
| `kw_round` | the worker, plain store; read by producers under `kw_mtx` (one possibly stale read per doorbell, harmless: at worst one boost given or withheld) | round counter, for the grace rule; `uint64_t` (S13) |
| `kwq_waiting[class]` (per CPU, outside the worker struct) | each class's worker, plain volatile store; read by lower classes on the same CPU | hand-back flags (S6.4) |
| `kw_cur`, `kw_pass_start`, `kw_pass_budget` | the worker | the pass in progress, for `kwq_budget_left()` and `kwq_requeue()`; `kw_cur` and `kc_state = RUNNING` are set the moment `ks_next()` hands a queue over (I2 holds between the pop and the pass) |
| `kw_served` | the worker | passes served in the current round; the hand-back (S6.4) applies only once it is non-zero |
| `kw_win_start`, `kw_win_busy` | the worker | CPU-share cap window |
| counters (S9) | the worker | |

Per (queue, CPU) (`struct kwq_cpu`), in addition to the P0 fields:

| field | block | writer | meaning |
|---|---|---|---|
| `kc_onlist` | shared | producers and worker under `kw_mtx` | NONE, NEW or ACTIVE |
| `kc_idle_round` | shared | the worker, read by producers under `kw_mtx` | low 32 bits of `kw_round` when the queue last went idle from the ring (`u_int`, to fit block 0: only the difference to `kw_round` is used, and a false "warm" after exactly 2^32 rounds costs one boost; S13); valid only while `kc_warm` is set |
| `kc_warm` | shared | the worker, read by producers | set when the queue went idle from the ring, cleared when it went idle from the new list; replaces a sentinel value (S13) |
| `kc_empty_since` | shared | producers under `kc_mtx` | `sbinuptime()` at the empty -> non-empty transition; a global monotonic clock, because the producer and the consumer are on different CPUs (S13) |
| `kc_deficit` | private | the worker | `D`, `int64_t` ns, clamped to `[-penalty_rounds x Qw, +2Qw]`, `penalty_rounds` 32 (S4.3, S13) |
| `kc_state` | shared | as in P0 | IDLE, WAKING, RUNNING, PARKED |

## 4. Algorithm

### 4.1 Doorbell (producer, holds `kc_mtx`, then `kw_mtx`)

Triggered by the first enqueue or notify into an IDLE list:

    if kc_warm and (u_int)kw_round - kc_idle_round <= GRACE_ROUNDS:  # unsigned
        append kc to kw_active tail; kc_onlist = ACTIVE; kw_nactive++
        kc_deficit is left as it was            (no boost: S4.6)
    else:
        append kc to kw_new tail; kc_onlist = NEW
    kc_state = WAKING
    wake the worker if it sleeps

`GRACE_ROUNDS` is 1: a queue that went idle during the current or the
previous round is still "warm".  (A producer reading `kw_round`, a
worker-private counter, is a deliberate cross-CPU read once per burst;
it is not on the per-item path.)

### 4.2 Round (worker)

    kw_round++
    serve_new(1)                                  # one boosted pass, if any
    n = kw_nactive                                # ring entries present now;
    repeat n times:                               # queues appended during the
        if higher class waiting and >= 1 pass served this round (S6.4): break
        kc = pop head of kw_active; kw_nactive--
        kc_deficit = min(kc_deficit + Qw(kc), 2 Qw(kc))   # refill, carry cap
        # (the lower clamp -2 Qw was applied when the pass was charged, S4.3)
        if kc_deficit <= 0:
            kc_state = PARKED; kc_parks++
            append kc to kw_active tail; kw_nactive++
        else:
            pass(kc)
            tick_guard()
        if higher class waiting and >= 1 pass served: break
        serve_new(1)                              # alternate: new, ring, new, ...
    m = length(kw_new)                            # ring done or empty: drain the
    repeat m times:                               # new entries present NOW; later
        if higher class waiting and >= 1 pass served: break   # arrivals wait
        serve_new(1)                              # round (which starts at once)
    round_end()                                   # S6

    serve_new(k): up to k times, if kw_new non-empty:
        kc = pop head of kw_new; kc_deficit = Qw(kc); kc_boosts++   # the boost
        pass(kc); tick_guard()

New-list entries and ring entries alternate strictly whenever both lists
are non-empty.  Pure new-first (fq_codel, CAKE) would let a sustained
stream of newly active queues starve the ring (S14 case 2), and serving
the new list only at round start would make a new queue wait for the
rest of the round (S14 case 1); alternation bounds both: a new queue
waits at most 2k passes where k is the number of new entries ahead of
it, and the ring receives at least every other pass.

A round with an empty new list and an empty ring does not happen: the
worker sleeps on `kw_active` while both are empty (P0's idle loop).  A
round in which every ring entry is PARKED performs no pass; it still
refills every entry, and because `D >= -2Qw` after any charge (S4.3) at
most two such rounds occur in a row, each costing a few hundred
nanoseconds plus the end-of-round yield.

### 4.3 Pass (worker, `kwq_pass()` as in P0 plus accounting)

    lock kc; swap out items and notifiers; kc_state = RUNNING
    t0 = cpu_ticks(); sample maxlat_ns from kc_empty_since; unlock
    r0 = cputime_self()                 # td_runtime + (cpu_ticks() - switchtime)
    kw_pass_start = r0; kw_pass_budget = kc_deficit
    run notifiers one by one, then the item list (P0 rules)
    dt = ns(cputime_self() - r0)        # CPU time the worker ran, S1a.4
    kc_cycles += cpu_ticks() - t0       # wall time, for latency diagnosis
    kw_win_busy += dt; kw_busy_ns += dt
    if dt > kw_pass_budget + Qw(kc): kc_overruns++  # more than one item over
    kc_deficit = max(kc_deficit - dt, -penalty_rounds x Qw(kc))   # S13
    lock kc
    if lists non-empty:
        kc_state = (kc_deficit > 0) ? WAKING : PARKED
        append kc to kw_active tail; kc_onlist = ACTIVE; kw_nactive++
    else:
        kc_state = IDLE
        kc_warm = (kc came from the ring); kc_idle_round = kw_round
        kc_deficit = 0                              # DRR: reset on empty
    wake drain/cancel waiters; unlock

The lower clamp is a deliberate departure from textbook DRR, whose
deficit cannot go below minus one packet.  Here a pass can overrun by any
amount (a handler that ignores its budget, a BLOCKING handler that ran a
long request), and an unbounded debt would park the queue for
`ceil(-D/Qw)` rounds: a single one-hour pass at Q = 200 us would silence
the queue for eighteen million rounds.  The debt is therefore capped at
`penalty_rounds` quanta (a per-class knob, default 32): an overrun costs
the queue at most 32 rounds of parking, however large it was, and a
repeat offender's share is bounded by `pass / (32 Qw)` relative to a
cooperative queue.  The first draft capped the debt at two quanta, the
mirror of the carry cap; the simulator (P1a) showed a budget-ignoring
handler with 8 ms passes taking 13.4 times a cooperative queue's share
under that cap, and 1.22 times under 32.  Fairness in the DRR sense is
thus bounded per overrun rather than repaid in full (B3 holds for
cooperative handlers), which is what CFS bandwidth control (throttled
only until the period ends) and EEVDF (lag bounded and decaying) also
settled on for CPU time.

An **overrun**, for the counter, is a pass that exceeded its budget by
more than one `Qw`.  A cooperative handler that stops after the item that
exhausts the budget overshoots by one item, which is the design working
as intended, not an overrun; the first draft counted it and every
cooperative pass showed as one.

Notifiers are charged like items: the time their handler calls take is
part of `dt`.  A queue that was served from the new list and still has
work joins the ring tail: it has had its boost, it is now bulk
(fq_codel's move-to-old, CAKE's sparse->bulk).

### 4.4 `kwq_budget_left(q)` (handler only)

    return max(0, kw_pass_budget - ns(cputime_self() - kw_pass_start))

Monotone within a pass, 0 once the budget is spent, never negative.
Measured in the worker's own CPU time, so a handler preempted by an
ithread is not charged for the interruption.  A
handler that returns when it reads 0 and requeues its remainder keeps
its pass within `budget + c_max`.

### 4.5 `kwq_requeue(q, head, tail, n)` (handler only)

Prepends to the current (queue, CPU) list under `kc_mtx`, as in P0.  The
items return to the same list, so the end-of-pass check (S4.3) sees a
non-empty list and appends the queue to the ring tail with whatever
deficit remains: a cooperative handler leaves `D` slightly positive or
slightly negative and is served again next round.

### 4.6 Grace rule (anti-gaming)

Without it, a bulk queue whose producer pauses briefly between bursts
(any TCP sender does) would leave the ring, return through the new list
with a fresh `Qw`, and so obtain up to one extra quantum per burst at
the expense of continuously backlogged queues; fq_codel's move-to-old
and CAKE's decaying set exist for exactly this.  kwq's version: a queue
that goes idle *from the ring* records the round number; a re-arrival
within `GRACE_ROUNDS` rounds puts it back on the ring tail with its
deficit as it was (reset to 0 at idle, so it waits for the next refill
like any ring member); a re-arrival later goes to the new list with a
boost.  A queue that goes idle *from the new list* (it never needed the
ring) clears `kc_warm` and is eligible for the boost again immediately:
that is the light queue the new list exists for.  One round is chosen over a
time constant because the harm of the boost is measured in rounds
(one extra `Qw` per round of the others' service).

*Rounds as the clock.*  The round counter advances only while the
worker has work; when every list is empty the worker sleeps and
`kw_round` stands still, so "one round" is not convertible to wall or
CPU time and is not meant to be.  What the grace rule protects is the
service of *competing* queues, and competitors are served only during
rounds; while the worker sleeps nobody is being served, so a boost given
or withheld then has nothing to take from.  The three cases:

- The worker is busy with other queues when the idle queue returns.
  Rounds have advanced by the others' service; if more than
  `GRACE_ROUNDS` of it happened, the queue is a sparse arrival and gets
  the boost; if not, it is a burst-and-pause producer and joins the ring.
  This is the case the rule exists for, and here rounds measure exactly
  the right thing: how much service the others received since the queue
  left.
- The worker slept since the queue left (the ring emptied, however long
  ago).  `kw_round` has not moved, so the queue reads as warm and joins
  the ring rather than the new list.  It is the only queue present, so
  the difference is nil: on the ring it is refilled with `Qw` at the start
  of its pass (its deficit was reset to 0 at idle), which is what the
  boost would have given it.  A second queue arriving in that same first
  round may also read as warm and wait for one pass of the first, at most
  `2Qw + c`: acceptable, and the only price of not consulting a clock.
- The worker slept, then served other queues, then the idle queue
  returns.  Rounds advanced during the others' service, so the queue is
  boosted iff that service exceeded `GRACE_ROUNDS`: the same criterion as
  the first case, with the sleep correctly counting for nothing.

The same holds for the two other places rounds appear.  Refills and
parking happen per round: a parked queue is unparked by the refills of
the following rounds, and if the worker instead runs out of work and
sleeps, the queue was idle and its deficit was reset, so no debt survives
an idle period (I3).  The end-of-round yield happens per round: while the
worker sleeps it is not holding the CPU and there is nothing to yield.
Only the CPU-share cap (S6.3) is defined in wall time, with
`sbinuptime()` windows, because the fraction it bounds is the worker's
share of the CPU over time, sleep included; a window that spans a sleep
correctly reads as mostly idle.

### 4.7 Invariants

- I1 A (queue, CPU) is on at most one of `kw_new`, `kw_active`, and
  `kc_onlist` says which; only under `kw_mtx`.
- I2 `kc_state` is IDLE iff both lists are empty and the queue is on no
  worker list; WAKING or PARKED iff on a worker list; RUNNING iff
  `kw_cur == kc`.
- I3 `-penalty_rounds x Qw <= kc_deficit <= 2 Qw` at all times (penalty
  cap and carry cap), and `kc_deficit == 0` whenever `kc_state == IDLE`.
- I4 The worker holds no service lock while a handler runs.
- I5 Between two consecutive `round_end()` calls, every queue on the
  ring at the first call has been refilled exactly once and has had
  either one pass or one park, unless a hand-back (S6.4) ended the round
  early; the entries not reached keep their place at the ring's head.
- I6 A queue is boosted at most once per idle period, and an idle period
  ending within `GRACE_ROUNDS` of leaving the ring is not boosted.

## 5. Handler contract under the scheduler

Unchanged from KWQ.md S3/S6, restated with the scheduler's terms:

- The handler owns the batch and is never interrupted by kwq; the
  quantum is cooperative.  A handler processes items in order, checks
  `kwq_budget_left()` **after every item** (a `cpu_ticks()` read, cheaper
  than any item; KWQ.md S16), and when it reads 0 with items left,
  finishes any per-batch flush (LRO) and `kwq_requeue()`s the untouched
  remainder.
- **Progress rule**: a handler processes at least one item per
  invocation before honouring a zero budget.  The budget at pass start
  is positive but may be tiny (a carried remainder), and a notifier's
  handler running earlier in the same pass may have consumed it; without
  this rule an item list could be requeued untouched pass after pass
  (S14 case 4).  DRR's equivalent is serving a packet whenever the
  deficit covers it.
- A handler that does not check the budget is legal; its passes may run
  to the end of the batch, the overrun is charged, and the queue is
  parked for the following round(s).  The penalty is at most
  `penalty_rounds` (32) rounds (the deficit clamp, S4.3), so a
  persistent offender is not repaid in full by fairness but held to at
  most `pass / (32 Qw)` of a cooperative queue's share and named by
  `overruns`; latency of *other* queues suffers by the overrun once per
  pass of it.  `overruns` per queue is the symptom to look at.
- Notifier handlers are the same: a notifier that drains a client list
  (GELI) checks the budget and re-notifies itself when work remains.
- `kwq_requeue()` and `kwq_budget_left()` are valid only inside a
  handler of that queue on that CPU (asserted).

## 6. Yielding

### 6.1 When

- **End of round** (`round_end()`), whenever anything else is runnable
  on the CPU (below), even for a round that only parked.
- **Tick guard**, between passes: if `ticks - td_swvoltick >= 1` (a
  hardclock tick has passed since the worker last switched out
  voluntarily), yield before the next pass.  This bounds callout lateness
  to one tick plus one pass regardless of pass cost (if_pair's finding,
  ../NOTES.md 2026-08-20), and covers hz=100 guests where a round can be
  many ticks.

Both yields are skipped when `sched_runnable()` is false: nothing else
is runnable on the CPU, so the switch would return immediately, and at
one round per item under light load the saved `mi_switch()` is a few
percent of throughput.  `td_swvoltick` is then not refreshed, so the tick
guard keeps asking, which costs one `sched_runnable()` read per pass.

### 6.2 How

    kern_yield(kwq_yield_prio[class])      # a fixed priority, never PRI_USER
    thread_lock(curthread); sched_prio(curthread, class priority); thread_unlock

`yield_prio` defaults to `PUSER` (56): the yield lets every ithread,
softclock, the kernel range (40-55) and user threads at priority 56 (the
best interactive score) run; it lets no CPU-bound user thread run.  `PRI_USER` is
never used: it resolves to `td_user_pri`, which ULE recomputes every
tick from the worker's own history and which drifts into the batch range
under saturation, letting user threads run whole slices (KWQ.md S11
Missing 1).  An operator who wants user throughput over NET latency sets
`yield_prio` to `PRI_MAX_TIMESHARE` (223) and gets that deterministically.

A caveat on `PUSER`: a user thread that ULE scores interactive but that
has just turned CPU-bound sits at priority 56..119 for the seconds its
score takes to decay, and if it is at 56 a yield hands it the CPU for a
full ULE slice (16-94 ms).  Real interactive threads sleep within
microseconds, so this is rare, but it is a hole of a slice per round
while it lasts (S14 case 6).  `yield_prio = PRI_MAX_KERN` (55) closes it
by yielding to kernel threads only, at the price that user threads on a
saturated CPU get CPU only through the cap; that is the "hard NET
latency" setting and should be measured in P1's `yield` scenario.

### 6.3 CPU-share cap (off by default)

Mogul's "limit on CPU usage" feedback, for the case the yield cannot
serve (CPU-bound user threads on a saturated CPU):

    at round_end, if cap_pct < 100:
        if now - win_start >= cap_window:
            busy_fraction = win_busy / (now - win_start)
            if busy_fraction > cap_pct/100 and sched_runnable():
                pause_sbt(cap_sleep_us, C_PREL(1)); cap_sleeps++
                (a pause replaces the yield for this round)
            win_start = now; win_busy = 0

`sched_runnable()` is the exported ULE query "is anything else runnable
on this CPU"; without it the cap would sleep on an otherwise idle CPU.
Defaults: `cap_pct` 100 (off), `cap_window_us` 10000, `cap_sleep_us`
100.  The jitter the cap adds is `cap_sleep_us` per cap event; PLAN.txt
lists measuring it as a P1 exit item.

### 6.4 Hand-back between classes on one CPU

A NET yield at `PUSER` makes the NET worker the lowest runnable kernel
thread on its CPU until it runs again and re-asserts `PI_NET`.  If the
BULK worker (`PI_SOFT`) is runnable, it takes the CPU and keeps it until
*it* yields or sleeps: without further rules a NET yield costs up to a
whole BULK round (S14 case 3), thousands of times NET's quantum.  ULE
does not preempt BULK for a NET worker sitting at `PUSER`, and the
scheduler primitive that would (KWQ.md S11 Missing 1) does not exist.
kwq owns both workers, so it hands back itself:

    per CPU: kwq_waiting[class] flags, written by the class's worker
    NET worker, before a yield:   kwq_waiting[NET] = true
    NET worker, after the yield:  kwq_waiting[NET] = false
    BULK worker: kwq_budget_left() returns 0 while kwq_waiting[NET];
                 after every pass, if kwq_waiting[NET] and this round has
                 served at least one pass: end the round now (round_end(),
                 whose yield at PUSER ties with NET, and NET was queued
                 first).  The "at least one pass" matters: the flag is
                 already set when BULK starts running at a NET yield, and
                 without it BULK handed back before doing anything - the
                 simulator showed a saturated NET giving BULK nothing at
                 all (S14 case 3).  With it a NET yield buys BULK exactly
                 one pass of one item.
    BLOCKING worker: the same for kwq_waiting[NET] or kwq_waiting[BULK]

The hole a NET yield opens is then at most one BULK item (`c_bulk`) plus
two yields, instead of a BULK round.  The flags are per-CPU, written by
one thread each and read by the others on the same CPU, so a plain
volatile store and load suffice.  This is the strict priority between
classes of S1a.3 made to hold across yields.

## 7. Properties and bounds

Notation: `n` queues active on one (class, CPU); `Q`; `w_i`; `c_i` the
client's `c_max`; `Y` the time the yielded-to threads run at a yield
(zero when nothing else is runnable: the yield is then skipped, S6.1).

- **B1 Pass length.**  A cooperative pass takes at most `budget + c_i`
  <= `2 Q w_i + c_i` (carry cap).  A non-cooperative pass is unbounded
  by kwq; the overrun is charged.
- **B2 Round length.**  `R <= sum over active i of (2 Q w_i + c_i) + sum
  over new-list entries of (Q w_j + c_j) + tick-guard yields + Y`.  With
  one active queue of weight 1 and a cooperative handler, `R <= 2Q + c +
  Y`, i.e. today's if_pair behaviour with a time instead of a count.
- **B3 Fairness (DRR).**  For two queues continuously backlogged over
  `k` rounds, `|S_i/w_i - S_j/w_j| <= 2Q + c_i/w_i + c_j/w_j` at every
  round boundary, independent of `k` (Shreedhar-Varghese's bound with
  the carry cap as the quantum term).  Weighted shares therefore
  converge as `1/k`.
- **B4 Latency, newly active queue.**  From doorbell to its first pass:
  at most the pass in progress (B1), plus, for each of the `k` new-list
  entries ahead of it, one new pass (`Q w_j + c_j`) and one interleaved
  ring pass (`2 Q w_i + c_i`), plus at most `k + 1` yields.  Independent
  of ring length `n`: this is what the new list buys (KWQ.md S15,
  S4.2 alternation).
- **B5 Latency, backlogged queue.**  One round, B2.  With `n` cooperative
  weight-1 queues: `<= n(2Q + c) + Y`.
- **B6 Anti-gaming.**  A queue obtains at most one boost per idle period
  of more than `GRACE_ROUNDS` rounds; a producer alternating burst and
  pause within a round gains nothing over a continuously backlogged one.
- **B7 Timer lateness.**  A callout on the worker's CPU is late by at
  most one tick plus one pass (B1) plus the yield, because the tick guard
  yields before the next pass once a tick has elapsed and the yield
  drops below `PI_SOFT`.
- **B8 Work conservation.**  The worker never sleeps while a list is
  non-empty (P0 protocol), and never performs more than
  `penalty_rounds + 1` consecutive rounds without a pass (the most
  indebted queue needs that many refills, S4.3).
- **B9 Cost.**  Per enqueue: unchanged from P0 (one lock, one append,
  one state check).  Per item on the consumer side: the budget check,
  about 20 ns.  Per doorbell: one more read (`kw_round`) and one list
  append.  Per pass: about 200-300 ns of clock reads, arithmetic and
  list moves.  Per round: one yield if anything else is runnable.
  Details and the measurement plan: S15.

## 8. Parameters

| knob | scope | default | effect |
|---|---|---|---|
| `kern.kwq.<class>.quantum_us` | class, RWTUN | 200 / 1000 / 5000 | `Q`; the throughput/latency dial: larger = fewer yields and lock hand-offs per item, longer B4/B5.  Validated to `[10, 1000000]` us by the sysctl handler: 0 would park every queue forever (S13) |
| `kwq_params.weight` | queue, create time | 1 | share within the class on a CPU, 1..8, read as "this queue gets `w` quanta per round".  Clients are reviewed kernel code, not tenants, so a weight above 1 is a declaration reviewed with the client, not a privilege kwq enforces (KWQ.md S1 corrected 2026-09-28) |
| `kern.kwq.<class>.yield_prio` | class, RW | `PUSER` | who may run at a yield (S6.2) |
| `kern.kwq.<class>.cap_pct` | class, RW | 100 (off) | CPU-share cap threshold; values above 100 read as off |
| `kern.kwq.<class>.cap_window_us` | class, RW | 10000 | cap measurement window |
| `kern.kwq.<class>.cap_sleep_us` | class, RW | 100 | cap sleep, the jitter it adds |
| `GRACE_ROUNDS` | compile-time | 1 | anti-gaming window; the P1a sweep found 1 and 2 indistinguishable (S12) |
| `kern.kwq.<class>.penalty_rounds` | class, RW | 32 | debt clamp in quanta: how many rounds an overrunning queue can be parked at most (S4.3) |

Choosing `Q`: one to two orders of magnitude above the heaviest common
item (a TSO chain through `ip_input` ~10 us -> 200 us is ~20 chains), at
least `c_max` of every client in the class (S1a.1: below that a client's
single item overruns every pass and is parked most rounds - correct but
poor for everyone's latency), and well below a tick (1 ms at hz=1000) so
the tick guard rarely fires.
PLAN.txt P1 sweeps 50/100/200/500 us on the Ampere before the default is
final.

## 9. Observability

Per (queue, CPU), added to P0's counters: `overruns` (passes that
exceeded their budget by more than one `Qw`, S4.3), `parks` (rounds skipped), `boosts` (passes from the new
list), `grace` (doorbells that went to the ring because of S4.6),
`maxlat_ns` (largest oldest-item age at pass start; the B4/B5
measurement).  Per worker: `rounds`, `yields`, `tick_yields`,
`cap_sleeps`, `handbacks` (rounds ended early for a higher class, S6.4),
`busy_ns`, `idle_ns`, `wakeups`, `passes`.  KWQ.md S10.7 lists them with
types (rows marked P1).  DTrace probes
`kwq:::round-end`, `kwq:::park`, `kwq:::overrun`, `kwq:::yield` are P2
(KWQ.md S10.1); the counters above are P1 so that the test plan can be
checked without DTrace.

## 10. Test plan (P1 exit criteria)

The scheduler proper is a separate file, `kwq_sched.c`, with no kernel
calls of its own, and is tested first in a userspace discrete-event
simulator (`kwq/sim`, PLAN.txt P1a) that links that file against mock
clocks and threads.  The simulator checks the invariants of S4.7 after
every step and the bounds of S7 and the corner cases of S14 over seeded
random workloads, and it runs what a kernel cannot run safely: every
counter and clock started just below its wrap point (S13), the tick rate
changing or reading zero, hour-long passes, thousands of queues.  It
also settles the S12 questions by measurement.  The kernel scenarios
below (P1b) then check the glue and the real scheduler interactions;
each property has one scenario; the correctness rows run on the guest
under WITNESS and INVARIANTS, the cost rows on a GENERIC kernel without
them (S15.6); one saturated CPU, the test thread bound elsewhere.

| property | scenario | method | acceptance |
|---|---|---|---|
| B3 fairness | `fairness` | two NET queues, handlers burning 50 us and 500 us per item (`DELAY`), each kept backlogged by a producer refilling a 256-item pool, 3 s; handlers check the budget after every item and requeue | `cycles` within 5 % at equal weight; 2:1 within 10 % at weights 2:1 |
| B4 latency (new) | `latency` | queue A as above (heavy); queue B receives one time-stamped item every 1 ms from a callout on another CPU and goes idle between items | B's `maxlat_ns` <= 2 Q + c_A over the run |
| B5 latency (backlogged) | `latency` | A's items time-stamped at enqueue; the longest round `R_max` derived from `rounds` and `busy_ns` | A's `maxlat_ns` <= 2 R_max |
| B6 anti-gaming | `gaming` | queue C bursts 32 items then pauses one round, repeatedly, against backlogged A | C's `boosts` <= idle periods longer than one round (expected ~0); C's share equals A's within 10 % |
| B1/B5 overrun | `overrun` | handler ignores the budget and runs 2 ms items | `overruns` == passes; `parks` > 0; A's share still within 10 % over 3 s |
| B7 timers | `yield` | a callout at 1 kHz pinned to the saturated CPU for 3 s | fires within 1 % of 3000; with `quantum_us` = 5000 (above a tick) the tick guard keeps it there (`tick_yields` > 0) |
| S6.2 yield priority | `yield` (manual half) | a CPU-bound user process bound to the saturated CPU (`cpuset -l`), `ps -o cputime` | default: ~0 %; `yield_prio=223`: whole slices; cap on: >= (100 - cap) % |
| B8 conservation | `lifecycle` (P0) | unchanged | in == out |
| B9 cost | `cost`, `tq_baseline`, `switch_baseline` (S15.6) | items/s and ns per item with an empty and a 1 us handler; the same into a taskqueue; thread switch cost | consumer bookkeeping <= 30 ns per item and <= 300 ns per pass; within 5 % of `tq_baseline` items/s; numbers recorded in ../NOTES.md |

### 10.1 Simulator results (P1a, 2026-09-28)

`kwq/sim/kwqsim suite` on the host, 13 scenarios, 100 random seeds, in
about a second; all invariants hold, no bound violated.  Numbers to
compare the guest against in P1b:

| scenario | result |
|---|---|
| fairness, equal weight, 50 vs 500 us items, 3 s | service ratio 0.999; the 500 us queue shows `overruns == passes` and `parks` 4500 (case 5, `Q < c_max`) |
| fairness, weights 2:1 | ratio 1.998 |
| latency, light 1 kHz Poisson queue vs two floods | doorbell-to-pass 399 us worst against a B4 bound of 826 us; 2604 boosts, 17 grace hits |
| gaming, 32 x 20 us bursts with 10-200 us pauses vs a flood | 1 boost, 2081 grace hits, share 0.80 of the flood's (a pausing producer cannot equal a flood; it gains nothing over it) |
| overrun, budget-ignoring 8 ms passes vs a cooperative flood | `overruns == passes`, parks 6560, share 1.22 with `penalty_rounds` 32 (13.4 with the first draft's 2) |
| hand-back, NET flood and BULK flood on one CPU | BULK receives one item per NET round, 0.32 of NET's service; NET worst latency 1.7 ms with a 64-deep flood (0 before the "at least one pass" rule) |
| storm, 1000 light queues waking within 100 us against a flood | flood keeps 4750 passes in 2 s; light queues' worst latency 200 us |
| manyq, 2000 Poisson queues at 40 % load, weights 1..8 | worst item latency 5.5 ms; invariants sampled |
| wrap, wall clock, CPU time, round counter and `ticks` started just below their wraps | 27726 items through all four wraps, no violation; `ks_ticks2ns` exact against 128-bit arithmetic for 5 rates x 8 values including 2^64 - 1; `ks_ns_scale(0)` = 1 GHz |
| quantum 10 us with 20 us items, weights 8:1 | ratio 8.2, the light queue's `overruns == passes` |
| quantum 1 s, depth-32 floods | equal shares: the budget never binds, so weights cannot act (documented, not a defect) |
| badhandler, requeues everything | signature `passes` 9987, `requeued` 419476, items out 0; the cooperative queue keeps its throughput |
| random, 100 seeds x 3 s, 1-12 queues of mixed producers and handlers, external load and cap on or off | no violation |

Three findings changed the specification during P1a: the penalty cap
(S4.3), the hand-back rule (S6.4) and the overrun definition (S4.3); a
fourth, that a queue must be marked RUNNING when `ks_next()` hands it
over, is I2's wording made precise (S3).  Two simulator defects were
found by the invariants themselves (a snapshot taken after parks, a
streak counter that skipped hand-back rounds) and are recorded in
`sim/sim.c`.

## 11. Alternatives considered and rejected for the first version

- **FIFO among non-empty queues, whole batch per turn (P0's policy).**
  The worker keeps one list of queues that became non-empty and serves
  the head: one pass over everything that accumulated, then back to the
  tail if work remains.  It is O(1), needs no clock, and is what P0 runs
  today.  It fits two of kwq's situations well: one queue per worker
  (today's if_pair) and light load, where batches are one or two items
  and FIFO over queues approximates fine-grained processor sharing with
  Mogul/LRP's natural batching.  It fails the situations kwq exists for.
  (1) *Overload isolation*: a turn is as long as the backlog that arrived
  since the queue's last turn, so a flooding queue's turn grows until its
  list hits the limit; with `limit` 4096 and 10 us items every other
  queue on the CPU waits 40 ms per round, the callout-starvation
  scenario of ../NOTES.md 2026-08-20 made permanent for neighbours.  (2)
  *Heterogeneous cost*: the unit is the item, so a queue of 64 KB TSO
  chains takes hundreds of times the CPU of a queue of ACKs per turn.
  (3) *Notifiers*: a GELI or iflib notifier's pass drains a client list
  of unbounded length; without a budget the client must invent its own
  batch limit, which is the per-driver ad-hoc pool kwq replaces.  (4)
  *No weights and no penalty*: a handler that ignores a cooperative
  budget gets an unbounded share, so budgets cannot even be added
  meaningfully without the deficit's park.  (5) *New-queue latency*: a
  light queue waits behind every backlog ahead of it.  The step from
  FIFO to this specification is small in code (a time budget per turn,
  a refill, two clamps and a second list, about sixty lines) and is
  exactly what turns the cooperative budget into something a client
  cannot ignore for free, so the intermediate "FIFO plus budget plus
  tick guard" (if_pair's current worker with a time instead of a count)
  is not adopted either; it is, however, the natural first checkpoint
  when implementing P1, and the P0 lifecycle numbers are its baseline.

- **Timestamp schedulers (WF2Q+, QFQ, EEVDF over CPU time).**  Tighter
  B5 (one quantum instead of a round) at the cost of a sorted structure
  and virtual-time bookkeeping per pass.  With `n` a handful and `Q`
  200 us, B5 is one to two milliseconds, the same order as a tick;
  revisit if `maxlat_ns` measurements contradict this (KWQ.md S15).
- **Per-queue quantum (EEVDF's slice).**  A second knob for clients to
  get wrong; the class quantum plus the weight covers the cases seen so
  far.  Left for after P4/P6 measurements.
- **Adaptive budgets (BFQ).**  Useful when queues under-use their budget
  systematically; kwq queues that under-use go idle and reset instead.
- **Count budgets (NAPI weight, if_pair batch).**  Stretch with item
  size; rejected on if_pair's own evidence.
- **Preemption (interrupt-driven, dataplane style).**  Not available to
  a kernel thread without scheduler changes; the cooperative budget plus
  the parking penalty is the substitute, and `overruns` names the
  offender.
- **A ksoftirqd-style fallback thread for overrun work.**  Rejected in
  KWQ.md S5: it moves the problem to a lower-priority thread and makes
  tail latency depend on unrelated load.
- **Time-based grace instead of round-based.**  The harm of a boost is
  one `Qw` per round of the others' service, so rounds are the natural
  unit; a time constant would need tuning per quantum.

## 12. Open questions

- `GRACE_ROUNDS` 1 or 2?  **Answered by the P1a simulator (`sweep_grace`)**:
  a 32 x 20 us burst-and-pause producer against a flood obtains no boost
  and a share of 0.80 of the flood's for pauses of 10, 50 and 200 us
  under either setting, and is boosted only when its pause exceeds a
  round (500 us and up), identically for 1 and 2.  `GRACE_ROUNDS` stays
  1.  A time-based grace was rejected in S4.6 ("Rounds as the clock").
- Boost `Qw` or `Q`?  **Answered by `sweep_boost`**: a weight-8 newcomer
  bursting 100 x 20 us every 5 ms against a flood and a light Poisson
  queue gives the light queue the same worst latency (1.80 ms) and the
  newcomer the same share (0.40) either way; the boost is spent in one
  pass and the ring refill dominates.  `Qw` stays, for consistency with
  the ring.
- BLOCKING (P7): resolved by S1a.4 - passes are charged in the worker's
  own CPU time, so time asleep is not service.  What remains open for P7
  is whether a BLOCKING worker that sleeps mid-pass should release its
  place in the round (worker replacement, KWQ.md S1) or hold it.
- Should the survey's CPU-side rows (Xen credit, CFS bandwidth, EEVDF
  lag) be given more weight than the link schedulers when the two
  disagree?  So far they agree on every point this document relies on:
  new-arrival boost plus anti-gaming, per-consumer cap, carry limited to
  one refill.

## 13. Numeric robustness

Requirement: no quantity the scheduler keeps may overflow, wrap
incorrectly, divide by zero or accumulate error in a way that changes
behaviour with uptime.  Every stored number is listed; "wrap-safe" means
only differences of the value are used, in unsigned arithmetic, and the
true difference is far below half the type's range.

| quantity | type | range or rate | hazard | rule |
|---|---|---|---|---|
| `cpu_ticks()`, `td_runtime`, `pc_switchtime` | `uint64_t` ticks | up to ~5 GHz: 2^64 lasts 117 years; the TSC counts from CPU reset | wrap; cross-CPU skew | wrap-safe; only same-CPU differences are taken (the worker is bound), never a difference between two CPUs' ticks |
| `cpu_tickrate()` | `uint64_t` Hz | recalibrated once a second when the ticker is variable (`cpu_tick_variable`, non-invariant TSC) | division by zero before calibration; rate change inside a pass | `kwq_ticks2ns()` treats a zero rate as 1 GHz; a rate change mis-scales one pass by at most the P-state ratio, which only the deficit sees and the clamps bound.  Workers do not run before `smp_started`, by which time the ticker is calibrated |
| `kwq_ticks2ns(t)` | `uint64_t` ns | `t x 1e9` overflows above 18 s of ticks at 1 GHz | multiplication overflow; a 64-bit division per call costs 20-40 cycles | a 32.32 fixed-point scale `ns_per_tick = (1e9 << 32) / rate`, computed at load and re-derived at each round end when `cpu_tickrate()` differs from the cached rate (there is no change notification), so the hot path is one multiply and shift (`(t x scale) >> 32`); exact to 1 ns for `t < 2^32` ticks (about 1.4 s at 3 GHz, far above any pass), and the S13 split-division form is used for longer intervals (the idle time counter) |
| `kc_deficit`, `kw_pass_budget` | `int64_t` ns | clamped to `[-penalty_rounds x Qw, +2Qw]`, `Qw <= 8 s` | unbounded debt (S4.3), overflow | the clamp; with the largest legal `Q` (1 s), `w = 8` and `penalty_rounds` 32, `32 Qw = 2.6e11`, nowhere near 2^63 |
| `kwq_budget_left()` | `uint64_t` ns | `budget - elapsed` | negative result | computed in `int64_t`, clamped at 0 |
| `Q`, `Qw`, `2Qw` | `uint64_t` ns from `quantum_us` (`u_int`) | `quantum_us` validated to `[10, 1000000]`, `w` to `[1, 8]` | `Q = 0` parks every queue forever; an unvalidated `UINT_MAX` us x 8 x 1000 = 3.4e16 would still fit, but is nonsense | the sysctl handler rejects values outside the range; weight 0 is read as 1 at create |
| `kw_round` | `uint64_t` | ~1e6 rounds/s worst case: 2^64 lasts 5.8e5 years | a 32-bit counter would wrap in 71 minutes at that rate | 64-bit |
| `kc_idle_round` | `u_int` (block 0 has 4 bytes left, S17) | the low 32 bits of `kw_round` | wraps every 71 minutes at the worst-case round rate | only the unsigned difference to `kw_round` is used; validity is the `kc_warm` flag, not a sentinel; a wrapped difference can only make a queue idle for exactly a multiple of 2^32 rounds read as warm, which costs it one boost |
| `ticks`, `td_swvoltick` | `int` | wraps every 24.8 days at hz = 1000 | signed wrap | the tick guard uses `(u_int)ticks - (u_int)td_swvoltick >= 1`, the kernel's own idiom, correct across the wrap |
| `kc_empty_since`, `kw_win_start` | `sbintime_t` (`int64_t`, 32.32 fixed point) | uptime wraps after 68 years | cross-CPU stamp; wrap | `sbinuptime()` is one global monotonic clock, so a producer on CPU A stamping and the worker on CPU B reading do not compare two TSCs (the P0 code stamps with `cpu_ticks()`, unused so far; P1 switches it); differences only; 68 years is the kernel-wide limit and not kwq's to fix |
| `kw_win_busy`, `kw_busy_ns`, `kw_idle_ns`, `kc_cycles`, all counters | `uint64_t` | at 1e9/s, 584 years | wrap | monotonic counters; readers difference them |
| `kc_depth` | `u_int` | bounded by `limit` | `KWQ_LIMIT_NONE` = `UINT_MAX` would let `depth++` wrap at 2^32 queued items (~100 GB of queued objects, but not impossible on a large machine) | the limit check runs for every queue including unbounded ones, in the overflow-safe form `n > limit - depth`; `limit` is capped at `INT_MAX` in `kwq_create()`, so the 2^31st item is refused instead of wrapping anything, and that refusal is the fault signal the design wants |
| `kc_nnotify`, `kc_waiters`, `kw_nactive` | `u_int` | bounded by objects that exist | none | |
| `n` in `kwq_pass()` / handler `int n` | `int` | `<= limit <= INT_MAX` | `sign x n` would overflow `int` above 2^31 items in one batch | excluded by the `INT_MAX` cap on `limit` |
| cap arithmetic | `uint64_t` | window <= 1 s, busy <= window | `busy x 100` and `window_ns x pct` <= 1e11 | no overflow |
| `hz`, `mp_maxid`, CPU ids | `int` | fixed at boot | none | |

What this adds to the algorithm: the lower deficit clamp (S4.3), the
`kc_warm` flag (S3, S4.1), `sbinuptime()` for cross-CPU stamps (S3), the
zero-rate guard in `kwq_ticks2ns()`, sysctl validation of `quantum_us`,
`limit` capped at `INT_MAX`, and the overflow-safe depth check (the last
two are already in the P0 code).  None of them changes a bound in S7;
the lower clamp weakens B3 for non-cooperative handlers to "bounded per
overrun", stated in S4.3.

Uptime-dependent degradation is excluded by construction: every stored
value is either a bounded quantity (deficits, depths, list lengths), a
wrap-safe monotonic counter used only by difference, or a flag.  There
is no floating point, no averaging that accumulates rounding (the cap's
busy fraction is recomputed per window from two differences), and no
quantity whose *absolute* value is compared against a threshold except
the deficit, which is clamped.

## 14. Pathological corner cases

Each case: the trigger, what the algorithm as specified before this
section would have done, the signature in the counters, and the rule
that now covers it.  Cases 1-3 changed the algorithm (S4.2, S6.4);
case 4 added the progress rule (S5); the rest are documented limits.

| # | trigger | effect without the rule | signature | rule |
|---|---|---|---|---|
| 1 | a queue becomes active while the worker is in the ring part of a round | it waited for the rest of the round: B4 depended on ring length after all | `maxlat_ns` of light queues ~ round length | new list checked after every ring pass (S4.2 alternation) |
| 2 | a sustained stream of newly active queues (a broadcast waking thousands of idle pairs, or a client creating queues in a loop) | pure new-first service starves the ring for `sum(Qw)` of the new entries; fq_codel and CAKE have this property | ring queues' `maxlat_ns` grows with the number of new arrivals per round | alternation: the ring gets at least every other pass |
| 3 | NET yields while the BULK worker is runnable | BULK runs a whole round (up to `n_bulk x 2 Q_bulk`) before NET re-asserts `PI_NET`: 2 ms holes per 200 us NET round; and the first version of the hand-back flag starved BULK completely (it handed back before its first pass) | NET `maxlat_ns` ~ BULK round length; BULK `handbacks` counts the rounds it ended early | class hand-back flags after at least one pass (S6.4); simulated: BULK gets one item per NET round, 0.32 of NET's service in the `handback` scenario |
| 4 | a notifier's handler consumes the whole budget every pass, or the carried budget is tiny | the item list of the same queue is requeued untouched pass after pass: zero progress while the notifier keeps re-arming | `requeued` grows, `items` does not | progress rule (S5); also: do not mix notifiers and items on one queue |
| 5 | `Qw < c_max` (a client's single item exceeds the quantum) | every pass overruns and the queue is parked for `ceil(c_max/Qw) - 1` rounds after each; every pass of it stalls the others by `c_max` | `overruns == passes`, `parks` ~ `passes x (c_max/Qw - 1)` (simulated: 500 us items at Q = 200 us: overruns 2999 = passes, parks 4500) | not preventable without preemption; S8 recommends `Q >= c_max` and the counters name the client; fairness by weight still held (ratio 0.999 and 1.998 in the `fairness` scenario) |
| 6 | yield at `PUSER` hands the CPU to a user thread scored interactive that turned CPU-bound | a hole of one ULE slice (16-94 ms) per round for the seconds the score takes to decay | NET `maxlat_ns` spikes with no kwq counter explaining them; `yields` normal | documented (S6.2); `yield_prio = PRI_MAX_KERN` closes it |
| 7 | thousands of lightly active queues on one CPU | the per-pass fixed cost (S15.2, 200-300 ns) plus each handler's per-call cost dominates; batching efficiency falls; a round of 1000 one-item queues is 0.3-1 ms before the items' own work | `passes ~ items`, `busy_ns / items` high | a property of per-client queues (S1a.6); the cost is per active queue, idle queues cost nothing; per-side pair queues accept this (KWQ.md S7) |
| 8 | a handler requeues the same head item forever (it cannot make progress) | one pass per round is spent on it; the rest of the queue starves behind it | `requeued` grows, `items` does not, `passes` grows | a client bug kwq cannot fix; the signature is unambiguous |
| 9 | producers alternate burst and pause timed to the round | with the grace rule, no boost; without it one `Qw` per burst | `boosts` per queue vs its idle periods | S4.6 |
| 10 | a heavy-weight queue arrives new | boost of `Qw` with `w = 8` jumps eight quanta ahead of the ring | light queues' `maxlat_ns` | S12 open question: cap the boost at `Q` if seen |
| 11 | BULK stealing (P6) runs a batch of CPU A's queue on CPU B | whose deficit is charged is undefined | | P6 must define it (charge the victim's `kc_deficit` on A; the stealer's own accounting for the cap) |
| 12 | a BLOCKING handler sleeps mid-pass | the worker holds its place in the round while asleep; other BLOCKING queues on the CPU wait for worker replacement | | P7; CPU-time charging (S1a.4) already excludes the sleep from the deficit |
| 13 | an ithread preempts the worker mid-pass | wall-clock pass length grows; the deficit (CPU time) does not | `cycles` (wall) vs `busy_ns` (CPU) diverge | by design; B1-B5 are CPU-time bounds, wall-clock latency includes interrupt load |
| 14 | `quantum_us` changed at runtime | deficits refilled and clamped with the new `Qw` from the next round | | fine; the clamps re-bound old values immediately |
| 15 | worker sleeps with parked queues | cannot happen: a parked queue has work, and the worker sleeps only when both lists are empty | | I2 |
| 16 | `kwq_drain()` during a round | producers refused, the worker finishes queued passes, the drain waits for IDLE | | P0 protocol |
| 17 | a producer at lower priority holds `kc_mtx` when the worker wants it | turnstile lends the worker's priority | | mutex(9) |
| 18 | a NIC ithread at `PI_NET` competes with the NET worker at `PI_NET` | round-robin between them; the ithread's own run is bounded by iflib's budget and ULE's ithread slice demotion | | not kwq's; noted so that a saturated NET worker is not blamed for iflib's 94 ms |

## 15. Cost model

Operation counts are from the specified algorithm; the nanosecond
figures are order-of-magnitude estimates for a current x86-64 or arm64
server core (3 GHz class), to be replaced by measurements in P1 (S15.4).
The measured P0 figure from the debug guest (1e6 items in 6.7 s under
WITNESS and INVARIANTS, producer and consumer on 4 vCPUs) is not usable
for this: WITNESS alone multiplies every lock operation several times
over.

### 15.1 Per item

| who | operation | cost |
|---|---|---|
| producer, `kwq_enqueue()` | CPU choice (`hash % n` or a bounds check), one uncontended mutex lock/unlock, `STAILQ_INSERT_TAIL`, `depth++`, a state compare | 30-60 ns; the same lock and insert if_pair's `pair_output()` pays today, so the scheduler adds nothing here |
| producer, cross-CPU | the (queue, CPU) block moves to the producer's cache and back to the consumer's | 100-200 ns per line transfer, once per pass, not per item (a burst of items from one producer keeps the line) |
| consumer, handler loop | `KWQ_ITEM_NEXT` (one dependent load, usually a miss on the item), the call | the item's own cost dominates; the load is the price of an intrusive list, paid by every design |
| consumer, `kwq_budget_left()` after each item | `cpu_ticks()` (rdtsc ~8 ns, arm64 `CNTVCT` ~5-20 ns), two per-CPU loads, one multiply-shift, a compare | 15-30 ns; 1-3 % of a 1 us `ip_input()` for a small packet, negligible against a TSO chain |

Everything else the scheduler does is per pass or per round, so the
marginal cost per item is the budget check: **about 20 ns**.

### 15.2 Per pass (one queue's turn)

| operation | cost |
|---|---|
| lock, swap two list heads, read depth, state store, unlock | 40-60 ns |
| two `cpu_ticks()` reads, CPU-time arithmetic, deficit update, clamps | 20-30 ns |
| `sbinuptime()` for the latency sample (timecounter read) | 20-40 ns |
| `THREAD_NO_SLEEPING()`, `NET_EPOCH_ENTER/EXIT` (per-CPU record, critical section) | 30-50 ns |
| end-of-pass lock, emptiness check, unlock; ring append under the worker's spin mutex (interrupt disable/enable) | 60-100 ns |
| counters (five increments in the consumer's private line) | 5 ns |

**About 200-300 ns per pass**, independent of batch size; with an
average batch of 10 items that is 20-30 ns per item, with 1 item it is
the whole cost.  Passes with one item are the light-load regime where
the CPU has time to spare; under load batches grow and the per-pass cost
amortises (S11's FIFO analysis, Mogul's natural batching).

### 15.3 Per round and per burst

| operation | cost | frequency |
|---|---|---|
| round bookkeeping (counter, `sched_runnable()`, cap window arithmetic) | 10-20 ns | per round |
| yield when something is runnable: `kern_yield()` = `sched_prio` + `mi_switch()` + return + `sched_prio` back | 0.5-1.5 us when it actually switches (x86-64 and arm64 alike: register save/restore, run-queue update, no address-space switch for kernel threads) plus whatever the other thread runs | per round, only if runnable |
| doorbell: worker spin lock, list append, `wakeup_one()` (sleepqueue chain hash, lock, `setrunnable`, `sched_add`) | 200-500 ns on the producer; if the worker's CPU is idle, an IPI (~1-2 us to deliver) and the target's exit from its idle state (10 us and more from a deep C-state) before the worker runs | once per burst (IDLE -> WAKING), never per item |
| tick guard check | one `ticks` load and a compare | per pass |

### 15.4 Comparison with scheduling kernel threads

The alternative to scheduling queues inside one worker is to give each
client its own thread and let ULE schedule between them (today's
per-driver pools).  Per turn between two clients:

| | kwq pass switch | thread switch |
|---|---|---|
| mechanism | pop a list head, swap two list heads, refill a counter | `wakeup`/`setrunnable`, `sched_add`, `mi_switch`, `cpu_switch` (save/restore ~30 registers, stack switch), possibly an IPI |
| cost, same CPU | 0.2-0.3 us | 1-2 us (lmbench-class `lat_ctx` numbers for FreeBSD and Linux on x86-64 and arm64; ULE's `sched_switch` alone is a few hundred ns) |
| cost, target CPU idle | n/a (the worker is already running) | plus IPI delivery and idle exit, 2-20 us |
| cache footprint | the (queue, CPU) block, 1-2 lines | the other thread's stack and working set, tens of lines |
| who provides fairness and the time bound | kwq's deficit and quantum, at fixed priority | ULE: none for `PI_NET` threads (S1, ULE row); time slices only in the timeshare class |
| latency knob | `quantum_us` (200 us) | none below `sched_slice` (16-94 ms) |

So switching between queues is five to ten times cheaper than switching
between threads on the same CPU, and orders of magnitude cheaper than
waking a thread on an idle CPU; and it is the only one of the two that
carries a fairness and latency policy at interrupt-class priority.  The
comparison is not the whole story in kwq's favour: a thread per client
gets true preemption (ULE would slice timeshare threads; it does not
slice `PI_NET` ones), and kwq's budget is cooperative (S5).  For the
taskqueue shape used by epair and if_pair today - one `taskqueue_enqueue`
per burst plus one mutex per packet - kwq's per-item and per-burst costs
are the same operations; the scheduler's additions are the ~20 ns budget
check per item and ~200 ns per pass.

### 15.5 Memory

Per (queue, CPU): two `KWQ_LINE` blocks (shared and private), 256 bytes;
per queue on a 128-CPU machine 32 KB plus the header; a thousand
per-side pair queues, 32 MB.  Per worker: two blocks, 256 bytes.  No
per-item state.

### 15.6 Measuring it (P1 exit, added to S10)

| scenario | method | reports |
|---|---|---|
| `cost` | one queue, handler does nothing but count, producer on another CPU enqueues 1e7 items as fast as it can, then the same with a producer on the consumer's CPU; on a **non-WITNESS** GENERIC kernel in the guest (`KERNCONF=GENERIC INSTKERNNAME=kernel.generic tests/kwqvm.sh kernel install` puts it beside `kernel.debug`; select it at the loader prompt or with `kernel=` in loader.conf) | items/s, `passes`, `busy_ns / items` = consumer cost per item, producer ns per enqueue from the producer's own `cpu_ticks()` |
| `cost` with `cost_us=1` | the same with a 1 us handler | fraction of CPU in bookkeeping = 1 - (items x 1 us) / busy_ns |
| `tq_baseline` | the same producer pattern into a `taskqueue(9)` with one task per burst and an mbufq-style list (epair's shape) | items/s and ns per item for the design kwq replaces |
| `switch_baseline` | two kernel threads on one CPU handing a token back and forth with `wakeup`/`msleep` 1e6 times | ns per thread switch on this machine, the number the pass cost is compared against |

The Ampere runs the same four scenarios for the arm64 column, with
`pmcstat` for the cache-line traffic baseline of KWQ.md S17.
