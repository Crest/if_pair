# kwq: tunables and introspection

How to steer a running kwq and how to see what it is doing, with the
commands to type.  Everything here exists in the module as of
2026-09-29 (P2); the knobs and counters of the two optional wake
regimes, the urgent exemption and the priority tripwire, specified from
2026-10-28 on (KWQ.md S3, S16, SCHED.md S4.6, S6.3), are in the merged
module as well.  KWQ.md S10 is the design behind it and the reference
for anything not listed.  Names are exact.  ASCII only.

Contents: 1 the sysctl tree; 2 tunables; 3 service-wide read-only nodes;
4 per-worker counters; 5 per-queue nodes and counters; 6 DTrace; 7 DDB;
8 lockstat, fbt and profile; 9 the test module and its runner;
10 building for the translator.

## 1. The sysctl tree

    kern.kwq                              service-wide
    kern.kwq.<class>                      class knobs and read-only class data
    kern.kwq.<class>.cpu.<N>              one worker thread (class, CPU)
    kern.kwq.<class>.queue.<queue>        one queue
    kern.kwq.<class>.queue.<queue>.cpu.<N>  that queue's list on one CPU

`<class>` is `net`, `bulk` or `blocking`; `<N>` is a CPU id; `<queue>` is
the name given to `kwq_create()`, verbatim.  A name is 1 to 31
characters from `[A-Za-z0-9_-]`, so `netisr_ip` and `wg-crypto` are
names and `netisr/ip` is refused at create with a line in the kernel
log, as is a name already used in the class.  Every level holds one kind
of thing: knobs, workers or queues, never mixed.  Counters are 64-bit
and never reset except `maxlat_ns` and `maxdepth` through `reset`.  The
whole tree at a glance:

    sysctl kern.kwq
    sysctl kern.kwq.net.queue.pair0a      # one queue with all its CPUs
    sysctl kern.kwq.net.cpu.1             # one worker

## 2. Tunables

All per class.  RWTUN nodes are also read from `loader.conf` when the
module is preloaded; RW nodes take effect at the next round.  Every
handler rejects values outside its range with EINVAL.

| node | range | default (net / bulk / blocking) | what it does |
|---|---|---|---|
| `quantum_us` (RWTUN) | 10 .. 1000000 | 200 / 1000 / 5000 | CPU time a queue of weight 1 may use per round before its turn ends.  Larger: fewer yields and lock hand-offs per item, longer worst-case wait for the other queues on the CPU |
| `limit` (RWTUN) | 1..UINT_MAX | 4096 | per-CPU item limit for queues created with `limit = 0`; applies to queues created afterwards, clamped to INT_MAX there.  A queue at its limit refuses enqueues with ENOBUFS and counts them in `rejected`.  0 is refused with EINVAL, since every queue created afterwards would refuse every enqueue; as a loader tunable 0 leaves 4096 and the kernel prints `Setting sysctl kern.kwq.<class>.limit failed: 22` |
| `penalty_rounds` (RWTUN) | 1 .. 1024 | 32 | how many rounds a queue can be parked for overrunning its budget, in quanta of debt.  Smaller forgives a misbehaving handler sooner |
| `budget_check_every` (RWTUN) | 1 .. 64 | 8 | `kwq_budget_left()` reads the clock at least every this many calls and whenever the budget is nearly spent; between reads it estimates.  1 = a clock read per call (exact, expensive with cheap items) |
| `yield_prio` (RW) | class priority .. 223 | 56 (PUSER) | the priority the worker yields at after a round when something else is runnable.  223 lets every user thread run a full time slice per round |
| `cap_permille` (RWTUN) | 1 .. 1000 | 500 | share limit: the share of its CPU the worker may spend in passes per `cap_period_us` while other threads are runnable there, in 1/1000; for the rest of the period it waits between passes as an idle-priority thread, which runs only when the CPU would otherwise idle.  The default is an equal share; 1000 turns it off, and the worker then obeys ULE's slice end as a process would (SCHED.md S6.3) |
| `cap_period_us` (RWTUN) | 100 .. 1000000 | 1000 | the period of the share limit, and the longest the CPU is given away at a time |
| `cap_prio` (RWTUN) | class priority + 1 .. 255 | 224 (PRI_MIN_IDLE) | the priority the worker waits at between passes while over its allowance, and the priority it waits at under the idle-class wait of `wake_delay_us`; never the priority of a pass.  224 runs only when the CPU would otherwise idle and is preempted by anything; 223 (PRI_MAX_TIMESHARE) is the other useful value: the last batch slot, no idle-affinity shortcut in `sched_pickcpu()`, preempted only by priorities <= `kern.sched.preempt_thresh`.  Other values are for experiments; the default covers both waits with one decision (SCHED.md S6.3) |
| `tick_us` (RWTUN) | 0 .. 100000 | 0 (off) | the tick regime's bound L, an opt-in (KWQ.md S16): while the worker's producer is dense (three consecutive sleeps shorter than L) its sleep has a timeout of L and precision L/8, a producer that places a list counts `doorbells_skipped` instead of waking the worker, and the timeout serves what accumulated; it leaves on a tick that served one element or none.  An item that arrives while the regime is on waits at most L plus the pass in progress, so every co-scheduled small flow pays L; 20 to 50 us where it is worth having, off everywhere else |
| `tick_urgent_us` (RWTUN) | 0 .. 100000 | 0 (at once) | inside the tick regime, an urgent item (`kwq_enqueue_urgent()`) that finds the worker asleep wakes it this much later instead of at once, so the urgent items within the bound share one wake (`tick_urgent_deferred`, `tick_urgent_wakes`); a bound at or above `tick_us` is the tick's own and arms nothing.  Outside the regime, and for a notifier, a drain or a cancel, an urgent ring wakes at once at any setting (KWQ.md S16) |
| `steal` (RWTUN) | 0, 1 | bulk: 1 | BULK only: a worker with nothing of its own takes batches of `KWQ_F_STEALABLE` queues from other CPUs before it sleeps, and producers wake sleeping workers to do so; 0 turns stealing off and a stealable pass takes its whole list again (SCHED.md S16) |
| `steal_scope` (RWTUN) | 0 .. 8 | bulk: 8 | rings of the cache topology that stealing, the steal wake, `kwq_scatter()` and `KWQ_CPU_ANY` look at, nearest first; 0 keeps every item on its CPU |
| `steal_scan` (RWTUN) | 1 .. 4096 | bulk: 16 | other CPUs probed at most per attempt, wake, scatter or pick |
| `steal_max` (RWTUN) | 1 .. 4096 | bulk: 64 | items one steal takes at most, otherwise half the victim's live list |
| `steal_batch` (RWTUN) | 1 .. 4096 | bulk: 16 | items a pass of a stealable queue takes at most; one while a BULK worker near it sleeps, which the pass wakes for the rest |
| `steal_wake` (RWTUN) | 0 .. 4096 | bulk: 2 | an enqueue that leaves a stealable list this deep, or at a doubling of it, wakes the nearest sleeping BULK worker; 0: never |
| `place` (RWTUN) | 0, 1, 2 | bulk: 0 | BULK placement (SPEC-p6g.md S2.8), flippable under load: 0 `kwq_pick_any()`, the one idle mask and the idle-path steal scan (SCHED.md S16); 1 the gossip scheme, tables of nearly idle cores on a torus and the hand-off at a pass end in place of stealing (`kern.kwq.bulk.gossip.*`); 2 P8, the idle mask in lines of 16 CPUs, the donor line and the starved tick (`kern.kwq.bulk.shard.*`).  Under 1 and 2 an item for a fixed CPU of a stealable queue spills to where `KWQ_CPU_ANY` would put it when that CPU is behind.  `steal=0` turns the steals, hand-offs and spills of every mode off.  A flip wakes the sleeping BULK workers so each registers as idle where the new mode reads it |
| `shard.probe` (RW) | 0 .. 4 | 2 | place=2: groups a producer reads besides its own before the far probe |
| `shard.donor_us` (RW) | 0 .. 1000000 | 500 | place=2: the stealable backlog, us, that makes a CPU a donor (a lone item counts the work running ahead of it) |
| `shard.pull_us` (RW) | 0 .. 1000000 | 500 | place=2: a worker whose backlog is below this at a pass end pulls a batch from a donor; 0: never |
| `shard.watch_us` (RW) | 0 .. 1000000 | 1000 | place=2: the starved tick's period: a CPU holding stealable work whose worker reached no decision point for this long is flagged a starved donor; 0: off |
| `gossip.h_us`, `gossip.l_us`, `gossip.llocal_us` (RW) | | 2000, 500, 50 | place=1: H, the backlog at which a worker hands off and a fixed CPU's items spill; L, the most a hand-off's target may hold; L_local, the producer's own thread is good below it (SPEC-p6g.md S2.9) |
| `gossip.handoff_n` (RW) | | 4 | place=1: hand-offs at most per pass end |
| `wake_delay_us` (RWTUN) | 0 .. 1000000 | 0 (off) | the idle-class wait's deadline, an opt-in (KWQ.md S16): an enqueue from the worker's own CPU makes the worker runnable in the idle class at `cap_prio` instead of preempting the producer, and the worker runs when the producer sleeps or at this deadline, whichever is first.  An item from the worker's CPU waits at most the deadline; an enqueue from another CPU rings the doorbell at either setting |

Examples:

    # give NET handlers longer turns on a machine with big batches
    sysctl kern.kwq.net.quantum_us=500

    # a client that legitimately queues deep: raise the default before it is created
    sysctl kern.kwq.bulk.limit=16384

    # a dedicated router: NET workers keep the whole CPU against anything
    sysctl kern.kwq.net.cap_permille=1000
    # a shared box where the network is a background duty: a tenth
    sysctl kern.kwq.net.cap_permille=100

    # preloaded module: the same in /boot/loader.conf
    kern.kwq.net.quantum_us="500"
    kern.kwq.net.limit="8192"

    # exact per-item budget accounting while diagnosing a handler
    sysctl kern.kwq.net.budget_check_every=1

    # a dense cross-CPU producer on a busy worker: trade 50 us of
    # latency for the wakes (every queue of the class pays it)
    sysctl kern.kwq.net.tick_us=50

    # a receiver pinned to its worker's CPU: the doorbell stops
    # preempting the producer, items wait at most the deadline
    sysctl kern.kwq.net.wake_delay_us=200

The grace window of the anti-gaming rule is not a knob and no longer a
round count: it is `GRACE_SERVICE`, one ring's worth of service,
`quantum_us` times the ring length read at the doorbell, counted on a
per-worker service counter that stands still while the worker sleeps,
because the share limit's budget cut ends passes early and a round then
no longer stands for one quantum of service per competitor (SCHED.md
S4.6; the compile-time `GRACE_ROUNDS` of one round is gone as of
2026-10-28).  The counter carries each pass's sub-microsecond rest, so
passes shorter than a microsecond count too.  The two regimes above are
specified in KWQ.md S16 and are in the merged module, off by default.

## 3. Service-wide read-only nodes

    kern.kwq.version        KPI version (1)
    kern.kwq.ncpu           CPUs with workers
    kern.kwq.nqueues        queues currently created, active or not
    kern.kwq.<class>.priority   scheduler priority of the class's workers; a loader
                            tunable (net PI_NET = 1, bulk PI_SOFT = 2, blocking PUSER
                            = 56 by default), net and bulk in [0, 55], blocking in
                            [0, 223], and net <= bulk <= blocking, or all three fall
                            back to the defaults with a console message.  PI_SOFT for net puts the workers level
                            with netisr and iflib's queue threads, one below the
                            NIC interrupt threads
    kern.kwq.clock          the scheduler's tick source and rate: "cntvct 25000000 Hz",
                            "tsc 2194914621 Hz", "timebase ...", or "cpu_ticks ..." (the
                            kernel's ticker, the fallback); SCHED.md S15.2
    kern.kwq.clock_fast     loader tunable, default 1: 0 keeps cpu_ticks() (A/B runs)
    kern.kwq.topo           each CPU's search order for KWQ_CPU_ANY and stealing, one
                            line per CPU, rings separated by "|": "0: 1 | 15 2 14 3 ..."
                            on lab (SMT sibling first), "0: 1 127 2 126 ..." on a07
    kern.kwq.bulk.shard.group, .ngroups
                            place=2: CPUs per idle line (loader tunable
                            kern.kwq.bulk.group, 1-64, default 16) and the lines
    kern.kwq.bulk.shard.idle
                            each line's idle bits in hex, then the donor CPUs
    kern.kwq.bulk.shard.pick_near, .pick_far, .pick_self, .pick_nondonor,
    .pick_probe, .spills    where place=2 put KWQ_CPU_ANY items: an idle CPU of the
                            near groups (of those, by the far probe), the producer's
                            CPU, the nearest non-donor, the shallowest of four probes;
                            fixed-CPU items spilled at a donor
    kern.kwq.bulk.gossip.torus
                            place=1: "N cores, R x C", then per core its CPUs, L3,
                            package and neighbours
    kern.kwq.bulk.gossip.place_self, _idle_near, _sibling, _local, _table,
    _neighbours, _fallback  KWQ_CPU_ANY placements by the step that chose them
                            (SPEC-p6g.md S2.5)
    kern.kwq.bulk.gossip.spills, .handoffs, .handed, .merges, .expired_used
                            fixed-CPU items spilled; batches and items handed off;
                            merges run; placements that used an expired entry (0)

## 4. Per-worker counters: `kern.kwq.<class>.cpu.<N>.`

A `compare.sh:` note names the row `tests/compare.sh` derives from that
counter (`kwq_counters` reads the deltas, `kwq_rows` divides them by the
window), so a figure in a battery's TSV can be traced back to its
counter and the other way round.  The `tick*` and `wake*` counters
belong to the two optional regimes and the urgent exemption (section
2), `prio_violations` to the priority tripwire.

| node | meaning |
|---|---|
| `rounds` | DRR rounds completed |
| `passes` | handler invocations |
| `wakeups` | times the worker slept with nothing queued and was woken; a sleep that ended by its timeout counts in `ticks` instead |
| `yields` | end-of-round yields taken because something else was runnable |
| `tick_yields` | yields forced because a pass outlived a clock tick |
| `handbacks` | rounds ended early because a higher class waited on this CPU |
| `switched` | passes during which the worker left the CPU, preempted or blocked on a lock; NET charges them wall time (SCHED.md S1a.4) |
| `cap_demotions` | periods in which the share limit was reached with other threads runnable |
| `cap_yields` | hand-overs at pass end under the share limit |
| `cap_cuts` | budget calls answered 0 by the share limit, cutting a pass short (SCHED.md S6.3) |
| `cap_restores` | period deadlines that raised a waiting worker back to its class priority |
| `cap_ns` | wall time given away under the share limit |
| `cap_runon` | passes that ended over the allowance with nothing else runnable: the limit did not bind |
| `slice_yields` | yields at ULE's own priority for the worker because ULE ended its slice; limit off only (SCHED.md S6.3).  compare.sh: `kwq_slice_s` |
| `prio_violations` | passes that began below the class priority or outside its scheduling class: the tripwire of the priority promise to the handler, 0 on a conforming build; a module built with `KWQ_HARD_INVARIANTS` asserts it as well, a panic on an INVARIANTS kernel (KWQ.md S3).  compare.sh: `kwq_prio_violations`, which must read 0 in every row |
| `ticks` | tick-regime timeouts that fired (`tick_us` > 0).  compare.sh: `kwq_ticks_s`.  With `wake_delay_us` set as well this over-counts: a parked worker that waits in the idle class longer than `tick_us` returns `EWOULDBLOCK` from the timed sleep even though a wakeup arrived, so its park is counted here and not in `wakeups` (WAKE.md S12.1).  `wakes_low` is the trustworthy count of parks |
| `ticks_empty` | of those, the ones that found nothing queued: the price of a silence under the regime.  compare.sh: `kwq_ticks_empty_s` |
| `ticks_single` | of those, the ones that served one element: the regime's exit condition, so this counter and `tick_exits` move together.  compare.sh: `kwq_ticks_single_s` |
| `tick_enters`, `tick_exits` | regime episodes entered and left; equal within one while the flow runs |
| `tick_ns` | wall time the worker spent inside the regime |
| `tick_barren` | episodes in which no tick's round served more than one element (the round that entered the regime does not count), each doubling the entry hold-off from 2L to 1024L; a growing `tick_barren` means the regime is entering for a producer it cannot help |
| `wakes_low` | wakes placed in the idle class at `cap_prio` instead of preempting a same-CPU producer (`wake_delay_us` > 0): one per wait episode.  compare.sh: `kwq_wakes_low_s`, and `kwq_wake_ep_s` as the episode rate.  With `tick_us` set as well this still counts, because the wait is decided before the tick's skip (KWQ.md S16): `wakes_low` near 0 beside a large `doorbells_skipped` on a worker with a same-CPU producer is the symptom of the reversed precedence WAKE.md S12.1 fixed |
| `wake_low_ns` | total length of those episodes; over `wakes_low` it is the mean wait.  compare.sh: `kwq_wake_ep_us` |
| `wake_self` | episodes the worker ended by getting the CPU on its own, the producer having slept: the case the wait exists for |
| `wake_promotions_deadline` | episodes the deadline callout ended, i.e. the producer never slept and the item paid the full deadline.  compare.sh: `kwq_wake_deadline_s` |
| `wake_promotions_urgent` | episodes an urgent caller, a cross-CPU enqueue or the share limit's deadline cut short (`kwq_enqueue_urgent()`, a notifier, a drain, a cancel) |
| `wakes_urgent` | wakes of a sleeping worker by an urgent item or a notifier added to a list already placed with it (the urgent nudge): the wake neither regime would have given it; counted in the queue's `doorbells` too.  compare.sh: `kwq_wakes_urgent_s` |
| `tick_urgent_deferred` | urgent items that found the worker asleep in the tick regime and waited up to `tick_urgent_us` instead of waking it.  compare.sh: `kwq_turg_deferred_s` |
| `tick_urgent_wakes` | wakes by the `tick_urgent_us` bound, one per window of deferred urgent items (a tick that comes first takes the window).  compare.sh: `kwq_turg_wakes_s` |
| `busy_ns` | time charged to passes: CPU time, wall time for NET (SCHED.md S1a.4) |
| `idle_ns` | wall time asleep with nothing queued |
| `round` | current round number |
| `nactive` | entries on the DRR ring right now (a gauge) |
| `nnew` | entries on the new list right now, waiting for a boosted pass (a gauge) |
| `budget_calls` | `kwq_budget_left()` calls by handlers on this worker |
| `budget_reads` | of those, the ones that read the clock |
| `steal_tries`, `steals`, `stolen` | BULK: steal attempts made with nothing of its own to do, the ones that moved a batch here, and the items moved (SCHED.md S16) |
| `steal_wakes` | BULK: wakes from sleep sent to this worker so that it steals (a producer past `steal_wake`, or a stealable pass that left items) |
| `pulls`, `donor_flags`, `starved` | BULK, place=2: batches pulled from a donor at a pass end; times this CPU became a donor; times the starved tick flagged it |
| `handoffs` | BULK, place=1: batches this worker handed off at a pass end |

Examples:

    # how busy is the NET worker on CPU 3: busy / (busy + idle) over its lifetime
    sysctl -n kern.kwq.net.cpu.3.busy_ns kern.kwq.net.cpu.3.idle_ns | paste -s -d' ' - |
        awk '{ printf "%.3f\n", $1 / ($1 + $2) }'

    # is anything else fighting for CPU 3?  yields grow only when a yield had a taker
    sysctl kern.kwq.net.cpu.3.yields kern.kwq.net.cpu.3.tick_yields

    # items per pass over a 5 s window (batching efficiency)
    p0=$(sysctl -n kern.kwq.net.cpu.3.passes); i0=$(sysctl -n kern.kwq.net.queue.pair0a.cpu.3.items); sleep 5
    p1=$(sysctl -n kern.kwq.net.cpu.3.passes); i1=$(sysctl -n kern.kwq.net.queue.pair0a.cpu.3.items)
    echo "$(( (i1 - i0) / (p1 - p0 + 1) )) items per pass"

    # the priority promise: this must be 0 on every worker of the class
    sysctl kern.kwq.net | grep prio_violations | grep -v ': 0$'

    # the tick regime, if tick_us is set: entries, and coalescing
    sysctl kern.kwq.net.tick_us kern.kwq.net.cpu.3.tick_enters \
        kern.kwq.net.cpu.3.tick_exits kern.kwq.net.cpu.3.ticks \
        kern.kwq.net.cpu.3.ticks_empty kern.kwq.net.cpu.3.ticks_single \
        kern.kwq.net.cpu.3.tick_barren \
        kern.kwq.net.queue.pair0a.cpu.3.doorbells \
        kern.kwq.net.queue.pair0a.cpu.3.doorbells_skipped

    # the idle-class wait, if wake_delay_us is set: episodes and their
    # mean length
    sysctl -n kern.kwq.net.cpu.3.wakes_low \
        kern.kwq.net.cpu.3.wake_low_ns | paste -s -d' ' - |
        awk '{ printf "%d episodes, mean %.1f us\n",
                   $1, $1 > 0 ? $2 / $1 / 1000 : 0 }'

    # and what ended them: the worker's own turn, the deadline, a caller
    sysctl kern.kwq.net.cpu.3.wake_self \
        kern.kwq.net.cpu.3.wake_promotions_deadline \
        kern.kwq.net.cpu.3.wake_promotions_urgent

A reading of the tick regime at `tick_us=50` on a worker fed by one
dense cross-CPU sender, the 20 us gap row of WAKE.md S10.2:
`tick_enters` 1 with `tick_exits` 0 (one episode, still running),
`ticks` plus `wakeups` about 16.9 k per second where the sleeps were
49 k per second at `tick_us=0`, `ticks_single` and `tick_barren` 0, and
on the queue `doorbells_skipped` far above `doorbells`.  Read it as: the
regime entered once and has not flapped; the sender rings nothing and
pays the append only, 39 ns against 202; the sleep rate sits on the
interval-stabbing bound for an arrival rate times L of 2.5; and the
price is on the latency side, the queue's mean 28 us, which is the L/2 a
uniform arrival within a tick predicts, and its worst 248 us.  The
opposite picture is
`ticks_empty` growing while `doorbells_skipped` stands still, a producer
that went quiet with the regime still on: one tick serving nothing exits
it, and a `tick_barren` that keeps growing says the entry rule is firing
for a producer the regime cannot help.

## 5. Per-queue nodes: `kern.kwq.<class>.queue.<queue>.`

| node | meaning |
|---|---|
| `weight` | DRR weight as created (1 .. 8) |
| `limit` | per-CPU item limit in effect (`2147483647` for `KWQ_LIMIT_NONE`); writable at run time, 1..INT_MAX, to tune or sweep a queue's depth without recreating it |
| `flags` | `KWQ_F_*` as created (0x1 inactive at create, 0x2 stealable, 0x4 discard on drain, 0x8 reserve, 0x10 vnet, 0x20 spin) |
| `state` | `inactive`, `active`, `draining`, `drained` |
| `reset` | write 1 to zero `maxlat_ns` and `maxdepth` on every CPU of the queue |

Per CPU, `kern.kwq.<class>.queue.<queue>.cpu.<N>.`:

| node | meaning |
|---|---|
| `depth` | elements queued now, pending signals included (sampled) |
| `state` | 0 idle, 1 waking, 2 running, 3 parked (sampled) |
| `items` | elements handed to the handler, notifiers included |
| `signals` | of those, notifiers, counted as the handler consumes them |
| `passes` | handler invocations |
| `cycles` | `cpu_ticks()` spent in passes (divide by the worker's `busy_ns` for this queue's share of the worker) |
| `rejected` | enqueues refused: ENOBUFS at the limit, ENXIO after drain began |
| `coalesced` | `kwq_notify()` calls that found the notifier already pending |
| `requeued` | items the handler gave back with `kwq_requeue()` |
| `maxdepth` | high-water mark of `depth` at pass start, since the last `reset` |
| `maxlat_ns` | longest doorbell-to-first-pass latency, since the last `reset` |
| `overruns` | passes that exceeded their budget by more than one quantum x weight.  compare.sh: `kwq_overruns_s` |
| `overruns_switched` | of those, the passes during which the worker left the CPU, so `overruns - overruns_switched` is the handler's own excess and the rest is the system's (SCHED.md S6.3).  compare.sh: `kwq_overruns_switched_s` |
| `parks` | rounds the queue was skipped for owing time |
| `boosts` | passes served from the new list (a queue that woke up) |
| `grace` | doorbells sent to the ring instead of the new list by the anti-gaming rule, the queue having returned before one ring's worth of service had gone to the others (SCHED.md S4.6) |
| `debts` | doorbells sent to the ring because the queue went idle owing time |
| `doorbells` | wakes performed for this (queue, CPU): doorbells that found the worker asleep, idle-class parks and urgent nudges; a ring that finds the worker awake is not counted.  Identity over a window: the worker's `wakeups` is about the sum of the `doorbells` of the queues on its CPU plus its `tick_urgent_wakes`, and its `ticks` are apart (a park that outlasts a tick's timeout counts as a tick, section 4) |
| `doorbells_skipped` | placements of this list with the worker (the idle-to-placed step that rings the doorbell) that woke nothing because the worker slept in the tick regime (`tick_us` > 0), urgent items held back by `tick_urgent_us` included: the producer paid the append only.  Appends to a list already placed count in neither column.  compare.sh: `kwq_db_skipped_s`, summed over the queues on the worker's CPU |
| `switched` | passes of this queue during which the worker left the CPU; under NET's wall-time accounting these are charged the time away as well |
| `glitches` | passes whose CPU-time delta was negative or over 1 s (a broken clock; charged one quantum) |
| `steals_in`, `stolen_in` | BULK: batches and items this CPU's worker took from other CPUs' lists of the queue |
| `steals_out`, `stolen_out` | BULK: batches and items other CPUs' workers took from this list; summed over the CPUs, in equals out |
| `scattered` | BULK: items `kwq_scatter()` put on this list |

Examples:

    # health of one queue on every CPU: refusals, depth against the limit, misbehaviour
    sysctl kern.kwq.net.queue.pair0a | grep -E 'rejected|maxdepth|overruns|parks|glitches' | grep -v ': 0$'

    # worst queueing delay a client saw since the last reset, then start a new window
    sysctl kern.kwq.net.queue.pair0a.cpu.3.maxlat_ns
    sysctl kern.kwq.net.queue.pair0a.reset=1

    # a handler that ignores its budget shows overruns == passes and parks > 0
    sysctl kern.kwq.net.queue.wg_crypto.cpu.5.overruns kern.kwq.net.queue.wg_crypto.cpu.5.passes kern.kwq.net.queue.wg_crypto.cpu.5.parks

    # which CPUs a hash-steered client actually lands on
    sysctl kern.kwq.net.queue.pair0a | grep '\.items:' | grep -v ': 0$'

`glitches` should stay 0; a non-zero value means `cpu_ticks()` on this
machine is not monotonic (seen on Ampere Altra, SCHED.md S13) and every
CPU-time figure the kernel reports there is suspect.

## 6. DTrace

Provider `kwq`, probes `kwq:::<name>`.  `dtrace -l -P kwq` lists them.
Queue probes carry a translated `kwqinfo_t` as `args[0]`:

    kwq_name  kwq_class  kwq_weight  kwq_limit  kwq_flags  kwq_addr

The translator is `kwq/kwq.d`; `make install` puts it in
`/usr/lib/dtrace/kwq.d` next to the module, and an uninstalled build
tree works with `-L /path/to/kwq/kwq`.  It needs the module's CTF,
which the module build produces by default (section 10); a module built
`WITHOUT_CTF=1` makes `dtrace -l -P kwq` fail with "no struct kwq
definition is available".

| probe | fires | arguments after `args[0]` |
|---|---|---|
| `create`, `activate`, `drain-start`, `drain-end`, `destroy` | lifecycle | none |
| `enqueue` | every accepted `kwq_enqueue()` and `kwq_enqueue_urgent()`; once per `kwq_enqueue_list()` | `arg1` cpu, `arg2` depth after, `arg3` 1 if this enqueue rang the doorbell, whatever the doorbell then did, or its urgent nudge woke the worker |
| `reject` | enqueue or notify refused | `arg1` cpu, `arg2` errno |
| `pass-start` | list swapped, handler about to run | `arg1` cpu, `arg2` items, `arg3` doorbell-to-pass latency ns (0 on later passes of one backlog) |
| `pass-end` | handler returned | `arg1` cpu, `arg2` items, `arg3` ns charged (CPU time, NET wall time), `arg4` items requeued |
| `overrun` | a pass exceeded budget + one quantum | `arg1` cpu, `arg2` ns over |
| `park` | a queue skipped for owing time | `arg1` cpu, `arg2` deficit ns (zero or negative) |
| `budget-hit` | `kwq_budget_left()` answered 0 | `arg1` cpu |
| `round-end` (no `args[0]`) | worker finished a round | `arg0` class, `arg1` cpu, `arg2` ring entries, `arg3` wall ns since the previous round end |
| `yield` (no `args[0]`) | worker yielded | `arg0` class, `arg1` cpu, `arg2` 0 end of round, 1 tick guard, 2 share limit hand-over, 3 slice (ULE ended the slice, limit off) |
| `cap` (no `args[0]`) | the share limit (SCHED.md S6.3) | `arg0` class, `arg1` cpu, `arg2` event (0 a pass begins under the limit, 1 allowance spent and CPU given away, 2 given away again, 3 period ended while over, 4 deadline callout ran), `arg3` ns into the period, `arg4` ns used of it, `arg5` the pass's budget ns (event 0) |
| `idle` (no `args[0]`) | worker about to sleep | `arg0` class, `arg1` cpu, `arg2` busy ns since it last woke |
| `doorbell` (no `args[0]`) | a (queue, CPU) went from idle to placed with the worker, or an urgent nudge woke the worker | `arg0` class, `arg1` cpu, `arg2` the worker's state when rung: 0 awake (a wait in the idle class it ends included), 2 asleep (a `wakeup_one()` follows), 4 asleep and made runnable in the idle class (`wake_delay_us`), 6 asleep in the tick regime: placed, not woken; 1 (polling) is defined and never fires |
| `tick` (no `args[0]`) | a tick-regime sleep ended by its timeout (`tick_us` > 0, KWQ.md S16), as counted in `ticks` | `arg0` class, `arg1` cpu, `arg2` elements served since the worker's previous sleep (the previous tick's round, or at an episode's first tick the round that entered the regime) |
| `wake` (no `args[0]`) | a worker waiting in the idle class was promoted to the class priority (`wake_delay_us` > 0) | `arg0` class, `arg1` cpu, `arg2` reason: 2 deadline, 4 urgent (an urgent ring or nudge, an enqueue from another CPU, the share limit's deadline, an unload), 5 self (the worker got the CPU), `arg3` ns since the wait began |
| `steal` | a batch moved from one CPU's list of a stealable BULK queue to another's (P6, SCHED.md S16) | `arg1` from cpu, `arg2` to cpu, `arg3` items |
| `worker-block`, `worker-spawn`, `worker-exit` | defined for P7, never fire yet | |

One-liners, all verified against the test module:

    # queueing delay per client (us): the age of the burst when its first pass began
    dtrace -n 'kwq:::pass-start /arg3 != 0/ { @[args[0]->kwq_name] = quantize(arg3 / 1000); }'

    # who overruns, how far, and from where
    dtrace -n 'kwq:::overrun { @[args[0]->kwq_name] = quantize(arg2 / 1000); }'
    dtrace -n 'kwq:::overrun /args[0]->kwq_name == "pair0a"/ { @[stack()] = count(); }'

    # doorbell efficiency: doorbells per enqueue (near 0 = well batched)
    dtrace -n 'kwq:::enqueue { @e[args[0]->kwq_name] = count(); @w[args[0]->kwq_name] = sum(arg3); }'

    # per-CPU spread of a client's items
    dtrace -n 'kwq:::pass-end /args[0]->kwq_name == "pair0a"/ { @[arg1] = sum(arg2); }'

    # who is dropping, and who feeds the queue that drops
    dtrace -n 'kwq:::reject { @[args[0]->kwq_name, arg1, arg2] = count(); }'
    dtrace -n 'fbt::kwq_enqueue:return /arg1 != 0/ { @[stack()] = count(); }'

    # is the worker giving the CPU away, and why
    dtrace -n 'kwq:::yield { @[arg1, arg2 == 1 ? "tick" : arg2 == 2 ? "cap" : arg2 == 3 ? "slice" : "round"] = count(); }'

    # the share limit (SCHED.md S6.3): where in its period a worker begins each
    # pass and what budget the pass got (event 0; 1 demote, 2 yield, 3 restore,
    # 4 deadline), ns
    dtrace -n 'kwq:::cap /arg2 == 0/ { @el[arg1] = quantize(arg3 / 1000); @budget[arg1] = quantize(arg5 / 1000); }'

    # round lengths and how busy each burst was
    dtrace -n 'kwq:::round-end { @[arg1] = quantize(arg3 / 1000); }'
    dtrace -n 'kwq:::idle { @[arg1] = quantize(arg2 / 1000); }'

    # parked queues and their debt (us, negative)
    dtrace -n 'kwq:::park { @[args[0]->kwq_name] = quantize(arg2 / 1000); }'

Cost, measured on a 128-core Ampere (KWQ.md S10.3): probes compiled in
but disabled cost 0.6 % of a saturated worker's throughput; an enabled
probe costs about 140 ns per firing with a plain aggregation and 180 ns
with the translator and a string key.  The pass-level probes fire a few
times per pass and are safe to leave running; `enqueue` fires per item
(or per list call) and costs more than kwq's own per-item work, so use it
for short looks and prefer the pass-level answers to the same questions.

## 7. DDB

    db> show kwq

prints every queue with the CPUs it has been used on, then every worker:

    kwq ktb class net active weight 1 limit 4096 flags 0x5
      cpu1   parked  depth 64 notify 0 deficit -229855 passes 2157 items 138032 overruns 2157
    kwq kta class net active weight 1 limit 4096 flags 0x5
      cpu1   running depth 3 notify 0 deficit 167018 passes 5519 items 341212 overruns 7
    worker kwq_net/0 asleep round 10640 ring 0 new 0 passes 10608 cur -
    worker kwq_net/1 in round round 174865 ring 1 new 0 passes 192604 cur kta
    worker kwq_bulk/1 asleep round 0 ring 0 new 0 passes 0 cur -

Read it as: `ktb` is parked on CPU 1 owing 230 us after 2157 passes that
all overran; `kta` is the pass in progress (`cur kta` on the CPU 1
worker); the other workers sleep.  No locks are taken, so the output is
usable from a panic inside kwq itself.  To try it on a live machine with
a serial console: `sysctl debug.kdb.enter=1`, `show kwq`, `c`.

## 8. lockstat, fbt and profile

`tools/clockprobe` (standalone, `kldload`, `sysctl kern.clockprobe.run=1`,
`dmesg | grep clockprobe`) characterises the machine's clocks: cost
warm and fresh from idle, resolution, monotonicity, step jitter,
frequency, cross-CPU ordering, the kernel ticker's behaviour with
interrupts enabled and its per-CPU extension lag.  Run it before
trusting any clock-based number on a new machine (SCHED.md S10.3,
2026-10-20).

A caution before any of them: `lockstat -P` instruments every lock
acquisition kernel-wide and cost a single if_pair TCP flow on a07 about
15 % (29-32 instead of 33-38 Gbit/s, ../NOTES.md 2026-10-09).  Take a
lock profile in a run of its own, never in the run whose throughput is
being compared; hwpmc counting and the sched probes were not seen to
move the result.

Workers are threads named `kwq_net/N`, `kwq_bulk/N`, so the usual tools
attribute to them by name:

    # CPU time by thread on CPU 3
    dtrace -n 'profile-997 /cpu == 3/ { @[curthread->td_name] = count(); }'

    # where a client's handler spends its time
    dtrace -n 'profile-997 /curthread->td_name == "kwq_net/3"/ { @[func(arg0)] = count(); }'

Each (queue, CPU) mutex is named `kwq <queue>`, so lock contention per
queue shows up in lockstat:

    lockstat -A -D 5 sleep 5          # adaptive mutex spins by lock and caller

A producer contending its queue's lock appears as `kwq pair0a` from the
enqueuing function; many producers into one (queue, CPU) list is the
case where this grows (SCHED.md S15.7).  `fbt::kwq_enqueue:entry` with
`stack()` answers "who feeds this queue", `fbt` on the handler symbol
"what does the client do per pass".

## 9. The test module and its runner

`kwq_test.ko` drives kwq with synthetic loads; its knobs live under
`kern.kwq_test` and are not part of the KPI:

    scenario   lifecycle fifo notify reject discard sleep fairness latency gaming
               overrun yield cost scale fanin tq_baseline switch_baseline
               pingpong pingpong_tq clocks ifpair burst
               mixorder mixpattern mixbudget mixstarve mixrearm mixcancel
               mixrequeue mixdrain mixdrain_discard
    items reps cost_us limit cpu allow_panic secs qname   (P0 knobs)
    weight_a weight_b cost_a cost_b                    (fairness and overrun)
    batch      items per kwq_enqueue_list() in the flood producers (1 = kwq_enqueue)
    pairs      scale: independent producer/consumer pairs on disjoint CPUs
    fanin      fanin: producers, each on its own CPU, into one (queue, CPU);
               ifpair: senders per interface
    ifaces spread prod_ns cons_ns
               ifpair: interfaces (one queue each), worker CPUs each interface's
               flows are hashed over, busy work per packet on the sending and
               the receiving side (ns); items is each sender's window
    prod_gap_us
               ifpair: us each sender pauses between packets (0 = flat out)
    prod_prio  priority of the producer threads: -1 as created (a kernel
               priority), 56..223 a user-range one, so that a same-CPU run
               sees the wake's preemption
    burst_items burst_gap_us burst_mode
               burst: one queue on cpu, one producer on pcpu; mode 0
               bursts of burst_items with burst_gap_us after each, 1 one
               item every burst_gap_us, 2 exponential spacing of mean
               burst_gap_us; result_first_* and result_last_* are the age
               of a burst's first and last item at service (us)
    intr_cpu intr_us intr_work_ns intr_secs
               interrupt load beside any scenario: a direct callout on
               intr_cpu busy-waits intr_work_ns every intr_us (duty cycle
               at most 50 %); writing intr_secs starts it, 0 stops it;
               result_intr_* count its runs, late runs and busy ns
    nnf        mix: notifiers in use (1..64)
    run        write 1 to start; result_state, result_msg, result_* hold the outcome

`ifpair` is the shape of if_pair on kwq: worker CPUs 1 .. ifaces x spread,
sender CPUs after them, one kwq_enqueue() per packet (batch > 1: one
kwq_enqueue_list() per batch packets).  Its results: `result_msg` gives
packets/s, the mean enqueue cost and its share of the senders' CPU,
enqueue calls over 2 us (`result_enq_slow`, convoys), the workers' share
of time in packet work (`result_cons_pct`; the rest is kwq's passes, the
queue lock and returning items), queue latency mean and max
(`result_maxlat_a_us`, `result_maxlat_b_us`), rejects and window waits
(`result_pool_waits`: senders blocked on a full window, the receivers are
the limit).  The queues stay alive after the run for `kern.kwq.net.queue.ifp<N>`
and lockstat(1) names their locks "kwq ifp<N>".  While the queue works
off its backlog after "done", the knobs must not change: the handler
uses the run's own copies, but the next run's `kwq_drain()` is what ends
the previous one.

    sysctl kern.kwq_test.scenario=fanin kern.kwq_test.fanin=8 kern.kwq_test.batch=16 \
        kern.kwq_test.limit=1000000 kern.kwq_test.secs=5 kern.kwq_test.run=1
    sysctl kern.kwq_test.result_state kern.kwq_test.result_msg

The `mix*` scenarios (2026-10-08) put notifiers and items on one queue and
check how they interact.  All run one queue on one worker CPU (`cpu`, or
the CPU after the producer's) with the producer bound to another CPU,
and most build an exact list behind a *gate*, an item whose handler
spins until released, so the next pass swaps out precisely what was
built.  A delivery log written by the worker CPU records the order the
handler saw; `result_inversions` counts items delivered on the wrong
side of a notifier relative to arrival order, `result_exact_errors`
notifiers delivered other than once, `result_calls` handler
invocations, `result_requeues` the handler's `kwq_requeue()` calls,
`result_wcpu` the worker CPU.  `mixorder`: `items` items, one signal,
`items` items.  `mixpattern`: every pattern of length 1..4 over
{item, N0, N1}, 120 of them, including tail and head notifiers and
repeated signals (which coalesce).  `mixbudget`: `nnf` notifiers of
`cost_a` us each plus `items` items behind one doorbell; the handler
honours the budget.  `mixstarve` (SCHED.md S14 case 4): a flood of
`cost_us` items for `secs`, alone (`result_without`) and beside a
notifier of `cost_a` us that re-arms itself (`result_with`).
`mixrearm`: `batch` signals landing while the notifier's handler runs
must give exactly one further run that sees the final level.
`mixcancel`: cancel during a run, refusal afterwards, revival by
re-init, then a signal/cancel hammer for `secs`.  `mixrequeue`: a
notifier after every third item with a handler that requeues after
`batch` elements.  `mixdrain` and `mixdrain_discard`: pending notifiers
among items at drain time, every notifier idle afterwards.

`tests/run_p1b.sh` runs the scenarios with their standard knobs and
prints the results and the relevant counters.  It reaches the machine
through `KWQ_SSH` (default: the bhyve guest via `tests/kwqvm.sh ssh`)
and loads the modules from `MODDIR`, which may be a flat directory
(`/root/kwq` in the guest, `/boot/modules` after `make install`) or the
kwq source tree after an in-tree build:

    tests/run_p1b.sh                                   # everything, in the guest
    tests/run_p1b.sh fairness latency                  # a subset
    KWQ_SSH="ssh a07 doas -n" MODDIR=/home/crest/if_pair/kwq tests/run_p1b.sh cost cost16
    PAIRS=16 tests/run_p1b.sh scale
    FANIN=8 BATCH=1 tests/run_p1b.sh fanin
    IFACES=8 SPREAD=8 FANIN=7 tests/run_p1b.sh ifpair          # 120 CPUs
    FANIN=8 PROD_NS=500 CONS_NS=500 LOCKSTAT=1 tests/run_p1b.sh ifpair

`tests/kwq_test.sh` is the ATF test program and `tests/Kyuafile` its
suite: twenty-six cases that load the two modules, run a scenario and
assert the property it proves (per-CPU FIFO and exactly-once delivery,
rejects and discards, notifier coalescing, name validation, the class
limit refusing 0, fairness
within 10 %, the light queue's latency bound, the grace rule, overrun
parking, tick yields, two flow smoke tests, the eight `mix` cases on
notifiers among items, the priority promise with every moderation knob
on, the tick regime's entry and exit, and no timer left armed in a
silence).  It needs root and at
least 4 CPUs and skips otherwise; each case unloads what it loaded.
Run it as root on the test machine, pointing `moddir` at a flat module
directory or the source tree, or leave it unset to load installed
modules by name:

    kyua -v test_suites.kwq.moddir=/root/kwq test -k /root/kwq/tests/Kyuafile
    kyua -v test_suites.kwq.moddir=/home/crest/if_pair/kwq test -k tests/Kyuafile
    kyua report                                        # last run, per case
    kyua report --verbose --results-filter failed      # stdout of the failures

The "sleep" scenario (a sleeping handler must panic an INVARIANTS
kernel) is not in the suite; run it by hand in the guest with
`kern.kwq_test.allow_panic=1`.  Two machine-specific notes: on a07 the
FreeBSD-kyua package lacks `/usr/share/kyua/store` (copy it from another
15.1 machine and point `KYUA_STOREDIR` at it), and kyua itself dies with
"PID already in all_exec_data" when it runs the whole suite there
(`kern.randompid` is on); one case per invocation, `kyua test -k
Kyuafile kwq_test.sh:<case>`, works.  In the bhyve guest the `yield`
case skips itself when the VM's own 20-40 ms callout stall hits.

`pingpong` and `pingpong_tq` hand one item back and forth between a
producer on one CPU and the consumer on another, so every enqueue finds
the consumer asleep and pays the whole wake path; the two variants differ
only in the request half (kwq against a taskqueue thread).  Results:
`result_ns_per_item` wall ns per round trip, `result_prod_ns_per_item`
the producer's ns inside the enqueue call, `result_lat_ns` the average
and `result_maxlat_a_us` the worst enqueue-to-handler latency.  In-kernel
timing, no probes: the tool for A/B tests of the wake path at ~20 ns
resolution (SCHED.md S15.6, S10.3).  `tests/run_p1b.sh pingpong
pingpong_tq` prints the four numbers.

`kern.kwq_test.pcpu` places the two-CPU scenarios' producer (default: the
CPU after the target, which on an SMT machine with adjacent sibling ids
is the worker's own core; `KT_EXTRA="cpu=2 pcpu=4"` in run_p1b.sh).

`clocks` prices the clocks: ns per call of `cpu_ticks()`,
`get_cyclecount()`, `sbinuptime()`, `getsbinuptime()` and
`cpu_tickrate()` on the producer CPU, and on the worker CPU, inside a
handler after a 20 ms warm-up spin, the first three again plus
`kwq_budget_left()` under the current `budget_check_every` (best and
median of 20 batches of 100 calls, and the first batch on the CPU fresh
from idle as "cold").  `result_clocks` holds the line; `tests/run_p1b.sh
clocks` prints it.  A CPU fresh from idle runs everything 2.7-3.5x slower
for a while on both test machines, which is what the cold number shows
and why the warm-up exists (2026-10-20).

## 10. Build defaults

A plain `make` in `kwq/` produces modules with everything above:

- the probes: standalone builds (no `KERNBUILDDIR`) get a fixed
  `opt_global.h` from `sys/conf/config.mk` without `KDTRACE_HOOKS`; the
  Makefile defines it on the compiler command line for that case.
  Builds against a kernel object tree use that kernel's options (GENERIC
  has the hooks);
- the CTF the translator needs: the Makefile sets `WITH_CTF=1` and
  `DEBUG_FLAGS=-g` (ctfconvert needs the DWARF).  `make WITHOUT_CTF=1`
  turns it off;
- `make install` ships the modules into `/boot/modules` and the
  translator into `/usr/lib/dtrace`, which is everything DTrace and DDB
  need; the split-off `kwq.ko.debug` is installed too only with
  `make install KWQ_INSTALL_DEBUG=1`, into `/usr/lib/debug/boot/modules`,
  which exists only after a full `installkernel`.

`make uninstall` (as root, after `kldunload kwq_test kwq`) removes the
two modules and the translator again and refreshes `linker.hints`.  Do
that before testing a build from another tree: `kldload kwq` by name,
and a client module's `MODULE_DEPEND` on kwq, pick up
`/boot/modules/kwq.ko` in preference to anything loaded by path.

Checks:

    ctfdump -S kwq/kwq.ko | grep 'total number of types'
    nm kwq/kwq.ko | grep -c __set_sdt          # non-zero: probes compiled in

`make KWQ_LIST_CHECK=1` walks a (queue, CPU) list after every change
that moves more than one element (a pass's cut, a steal, a requeue, a
scatter's chunk) and asserts its length and tail: O(depth), for test
kernels, where a corrupted list should panic where it was corrupted.

`make KWQ_NO_SDT=1` compiles every probe site out, for measuring what
the disabled probes cost.
