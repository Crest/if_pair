# kwq: tunables and introspection

How to steer a running kwq and how to see what it is doing, with the
commands to type.  Everything here exists in the module as of
2026-09-29 (P2); KWQ.md S10 is the design behind it and the reference
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
| `limit` (RWTUN) | any | 4096 | per-CPU item limit for queues created with `limit = 0`; applies to queues created afterwards.  A queue at its limit refuses enqueues with ENOBUFS and counts them in `rejected` |
| `penalty_rounds` | 1 .. 1024 | 32 | how many rounds a queue can be parked for overrunning its budget, in quanta of debt.  Smaller forgives a misbehaving handler sooner |
| `budget_check_every` | 1 .. 64 | 8 | `kwq_budget_left()` reads the clock at least every this many calls and whenever the budget is nearly spent; between reads it estimates.  1 = a clock read per call (exact, expensive with cheap items) |
| `yield_prio` | class priority .. 223 | 56 (PUSER) | the priority the worker yields at after a round when something else is runnable.  223 lets every user thread run a full time slice per round |
| `cap_pct` | 1 .. 1000 | 100 (off) | CPU-share cap: when the worker's busy fraction over `cap_window_us` exceeds this and other threads are runnable, it sleeps `cap_sleep_us`.  100 or more = off |
| `cap_sleep_us` | 1 .. 1000000 | 100 | length of that sleep |
| `cap_window_us` | 100 .. 10000000 | 10000 | window of the busy-fraction measurement |

Examples:

    # give NET handlers longer turns on a machine with big batches
    sysctl kern.kwq.net.quantum_us=500

    # a client that legitimately queues deep: raise the default before it is created
    sysctl kern.kwq.bulk.limit=16384

    # confine a runaway BULK class to two thirds of its CPUs when others compete
    sysctl kern.kwq.bulk.cap_pct=66

    # preloaded module: the same in /boot/loader.conf
    kern.kwq.net.quantum_us="500"
    kern.kwq.net.limit="8192"

    # exact per-item budget accounting while diagnosing a handler
    sysctl kern.kwq.net.budget_check_every=1

The grace window of the anti-gaming rule (one round) is compile time.

## 3. Service-wide read-only nodes

    kern.kwq.version        KPI version (1)
    kern.kwq.ncpu           CPUs with workers
    kern.kwq.nqueues        queues currently created, active or not
    kern.kwq.<class>.priority   scheduler priority of the class's workers (net 1, bulk 2)

## 4. Per-worker counters: `kern.kwq.<class>.cpu.<N>.`

| node | meaning |
|---|---|
| `rounds` | DRR rounds completed |
| `passes` | handler invocations |
| `wakeups` | times the worker slept with nothing queued and was woken |
| `yields` | end-of-round yields taken because something else was runnable |
| `tick_yields` | yields forced because a pass outlived a clock tick |
| `handbacks` | rounds ended early because a higher class waited on this CPU |
| `cap_sleeps` | sleeps taken by the CPU-share cap |
| `busy_ns` | CPU time spent inside handlers |
| `idle_ns` | wall time asleep with nothing queued |
| `round` | current round number |
| `nactive` | entries on the DRR ring right now (a gauge) |
| `nnew` | entries on the new list right now, waiting for a boosted pass (a gauge) |
| `budget_calls` | `kwq_budget_left()` calls by handlers on this worker |
| `budget_reads` | of those, the ones that read the clock |

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

## 5. Per-queue nodes: `kern.kwq.<class>.queue.<queue>.`

| node | meaning |
|---|---|
| `weight` | DRR weight as created (1 .. 8) |
| `limit` | per-CPU item limit in effect (`2147483647` for `KWQ_LIMIT_NONE`) |
| `flags` | `KWQ_F_*` as created (0x1 inactive at create, 0x2 stealable, 0x4 discard on drain, 0x8 reserve, 0x10 vnet, 0x20 spin) |
| `state` | `inactive`, `active`, `draining`, `drained` |
| `reset` | write 1 to zero `maxlat_ns` and `maxdepth` on every CPU of the queue |

Per CPU, `kern.kwq.<class>.queue.<queue>.cpu.<N>.`:

| node | meaning |
|---|---|
| `depth` | items queued now (sampled) |
| `state` | 0 idle, 1 waking, 2 running, 3 parked (sampled) |
| `items` | items and notifiers handed to the handler |
| `passes` | handler invocations |
| `cycles` | `cpu_ticks()` spent in passes (divide by the worker's `busy_ns` for this queue's share of the worker) |
| `rejected` | enqueues refused: ENOBUFS at the limit, ENXIO after drain began |
| `coalesced` | `kwq_notify()` calls that found the notifier already pending |
| `requeued` | items the handler gave back with `kwq_requeue()` |
| `maxdepth` | high-water mark of `depth` at pass start, since the last `reset` |
| `maxlat_ns` | longest doorbell-to-first-pass latency, since the last `reset` |
| `overruns` | passes that exceeded their budget by more than one quantum x weight |
| `parks` | rounds the queue was skipped for owing time |
| `boosts` | passes served from the new list (a queue that woke up) |
| `grace` | doorbells sent to the ring instead of the new list by the anti-gaming rule |
| `debts` | doorbells sent to the ring because the queue went idle owing time |
| `glitches` | passes whose CPU-time delta was negative or over 1 s (a broken clock; charged one quantum) |

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

The translator is `kwq/kwq.d`.  Install it as `/usr/lib/dtrace/kwq.d`
or pass `-L /path/to/kwq/kwq` to `dtrace`.  It needs the module's CTF:
build with `WITH_CTF=1` (section 10); without it, `dtrace -l -P kwq`
fails with "no struct kwq definition is available".

| probe | fires | arguments after `args[0]` |
|---|---|---|
| `create`, `activate`, `drain-start`, `drain-end`, `destroy` | lifecycle | none |
| `enqueue` | every accepted `kwq_enqueue()`; once per `kwq_enqueue_list()` | `arg1` cpu, `arg2` depth after, `arg3` 1 if this enqueue woke the worker |
| `reject` | enqueue or notify refused | `arg1` cpu, `arg2` errno |
| `pass-start` | list swapped, handler about to run | `arg1` cpu, `arg2` items, `arg3` doorbell-to-pass latency ns (0 on later passes of one backlog) |
| `pass-end` | handler returned | `arg1` cpu, `arg2` items, `arg3` CPU time ns, `arg4` items requeued |
| `overrun` | a pass exceeded budget + one quantum | `arg1` cpu, `arg2` ns over |
| `park` | a queue skipped for owing time | `arg1` cpu, `arg2` deficit ns (negative) |
| `budget-hit` | `kwq_budget_left()` answered 0 | `arg1` cpu |
| `round-end` (no `args[0]`) | worker finished a round | `arg0` class, `arg1` cpu, `arg2` ring entries, `arg3` wall ns since the previous round end |
| `yield` (no `args[0]`) | worker yielded | `arg0` class, `arg1` cpu, `arg2` 0 end of round, 1 tick guard |
| `idle` (no `args[0]`) | worker about to sleep | `arg0` class, `arg1` cpu, `arg2` busy ns since it last woke |
| `steal`, `worker-block`, `worker-spawn`, `worker-exit` | defined for later phases, never fire yet | |

One-liners, all verified against the test module:

    # queueing delay per client (us): the age of the burst when its first pass began
    dtrace -n 'kwq:::pass-start /arg3 != 0/ { @[args[0]->kwq_name] = quantize(arg3 / 1000); }'

    # who overruns, how far, and from where
    dtrace -n 'kwq:::overrun { @[args[0]->kwq_name] = quantize(arg2 / 1000); }'
    dtrace -n 'kwq:::overrun /args[0]->kwq_name == "pair0a"/ { @[stack()] = count(); }'

    # doorbell efficiency: wakeups per enqueue (near 0 = well batched)
    dtrace -n 'kwq:::enqueue { @e[args[0]->kwq_name] = count(); @w[args[0]->kwq_name] = sum(arg3); }'

    # per-CPU spread of a client's items
    dtrace -n 'kwq:::pass-end /args[0]->kwq_name == "pair0a"/ { @[arg1] = sum(arg2); }'

    # who is dropping, and who feeds the queue that drops
    dtrace -n 'kwq:::reject { @[args[0]->kwq_name, arg1, arg2] = count(); }'
    dtrace -n 'fbt::kwq_enqueue:return /arg1 != 0/ { @[stack()] = count(); }'

    # is the worker giving the CPU away, and why
    dtrace -n 'kwq:::yield { @[arg1, arg2 == 1 ? "tick" : "round"] = count(); }'

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
    items reps cost_us limit cpu allow_panic secs     (P0 knobs)
    weight_a weight_b cost_a cost_b                    (fairness and overrun)
    batch      items per kwq_enqueue_list() in the flood producers (1 = kwq_enqueue)
    pairs      scale: independent producer/consumer pairs on disjoint CPUs
    fanin      fanin: producers, each on its own CPU, into one (queue, CPU)
    run        write 1 to start; result_state, result_msg, result_* hold the outcome

    sysctl kern.kwq_test.scenario=fanin kern.kwq_test.fanin=8 kern.kwq_test.batch=16 \
        kern.kwq_test.limit=1000000 kern.kwq_test.secs=5 kern.kwq_test.run=1
    sysctl kern.kwq_test.result_state kern.kwq_test.result_msg

`tests/run_p1b.sh` runs the scenarios with their standard knobs and
prints the results and the relevant counters.  It reaches the machine
through `KWQ_SSH` (default: the bhyve guest via `tests/kwqvm.sh ssh`)
and loads the modules from `MODDIR`:

    tests/run_p1b.sh                                   # everything, in the guest
    tests/run_p1b.sh fairness latency                  # a subset
    KWQ_SSH="ssh a07 doas -n" MODDIR=/home/crest/kwq-mods tests/run_p1b.sh cost cost16
    PAIRS=16 tests/run_p1b.sh scale
    FANIN=8 BATCH=1 tests/run_p1b.sh fanin

## 10. Building for the translator

The kwq.d translator needs `struct kwq` in the module's CTF, and kmod
builds only produce CTF when asked:

    make WITH_CTF=1                     # in kwq/ (DEBUG_FLAGS=-g is the Makefile default)
    ctfdump -S kwq/kwq.ko | grep 'total number of types'

`tests/kwqvm.sh mods` and `mods generic` pass `WITH_CTF=1`.  Standalone
builds (no `KERNBUILDDIR`) get `KDTRACE_HOOKS` from the Makefile so the
provider exists in them too.  `make KWQ_NO_SDT=1` compiles every probe
site out, for measuring what the disabled probes cost.
