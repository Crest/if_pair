# kwq(9): a FreeBSD-native kernel work queue - design proposal

Draft 2026-09-23.  Synthesis of ../NOTES.md (the if_pair pool, its governor
and the big-iron measurements), ../DISPATCH.md (libdispatch) and ../POOLS.md
(cross-OS survey and its ten lessons).  Names are illustrative; the
contracts are the point.  Where a rule is a direct consequence of a
lesson in ../POOLS.md S4, the lesson number is given as [L#].

## 0. One-paragraph summary

kwq is one system-wide service of per-CPU worker threads, organised in
three fixed CLASSES (NET, BULK, BLOCKING), to which kernel components
attach QUEUES.  A queue fixes, at creation, its class, its handler, its
ordering domain and its bound; an item is an intrusive link the client
owns.  Enqueue names a CPU and never allocates, never sleeps, and may
fail with ENOBUFS.  Each worker drains its CPU's queues in deficit
round robin measured in CPU time, with a hard quantum per queue and a
demote-yield at the end of every round so that softclock, epoch
reclamation and userland always run.  Every queue is accounted per CPU
(items, cycles, slices, quantum expiries, rejections, max queueing
latency) under sysctl and DTrace, by its label.  Clients never see a
thread, a priority, or a lock of the service.

## 1. Threading model

- **Threads per (class, CPU), not per client.**  At `SI_SUB_TASKQ` the
  service creates, for each online CPU, one NET worker and one BULK
  worker, pinned with `taskqueue_start_threads_cpuset`-style cpusets
  (`kthread_add` + `sched_bind` in the thread itself; never `sched_bind`
  the creating thread - the preload-boot hang in ../NOTES.md).  BLOCKING is
  a managed pool (S1.3), not one-per-CPU.  Thread names are
  `kwq_net/N`, `kwq_bulk/N`, `kwq_blk/N` so `top` and `ps` attribute time
  to the class; per-client attribution comes from the accounting, not
  from thread names [L3].
- **Priorities are fixed per class and not exposed.**  NET runs at
  `PI_NET` (`PRI_MIN_ITHD + 1`, the priority of NIC ithreads and netisr,
  deliberately equal so a worker running a peer's protocol input shares
  round-robin with them - ../NOTES.md priority audit).  BULK runs at
  `PI_SOFT` (`PRI_MIN_ITHD + 2`, with softclock and `qgroup_softirq`).
  BLOCKING runs at `PRI_MIN_KERN` (40), the top of the timeshare-adjacent
  kernel band, preemptible by every ithread.  No API takes a priority
  [L4].  A queue may declare a WEIGHT in 1..8 (default 1) that only
  affects its share within its class on its CPU, and can only be lowered
  by the client below the default share of others (declaring "I am
  background"), never raised above other clients' default.
- **NET and BULK never sleep.**  Their workers run every handler with
  `THREAD_NO_SLEEPING()` in effect (the `td_no_sleeping` counter that
  `epoch_enter_preempt` uses, `sys/proc.h`), so a sleep attempt panics
  under INVARIANTS and is diagnosable with WITNESS
  (`WITNESS_WARN(WARN_PANIC, NULL, ...)` at handler entry in debug
  kernels).  This is what lets the class be exactly one thread per CPU:
  a worker that cannot block never strands its queue [L1].
- **BLOCKING is a concurrency-managed pool** (cmwq's lesson, ../POOLS.md
  S3): a small number of threads per CPU that may grow, bounded by a
  ceiling, when a worker blocks; idle threads exit after a timeout.  The
  blocked-worker signal is the open question named in ../POOLS.md S5; the
  design reserves two mechanisms: a `sched_switch` hook counting
  runnable BLOCKING workers per CPU (the exact analog of cmwq's
  `wq_worker_sleeping`), or, initially, a 1 Hz callout comparing
  `TD_IS_RUNNING` over the pool (the libdispatch monitor, in-kernel and
  without the /proc detour).  Additionally, a BLOCKING queue may reserve
  `nrescue` threads that serve only it (`KWQ_F_RESCUE`), for clients on
  the memory-reclaim or root-I/O path [L2].  GELI is such a client; until
  the rescue mechanism exists, GELI keeps its own threads.
- **Stealing is a class property, off by default.**  NET queues are
  always drained by the CPU they were enqueued on (per-flow ordering
  depends on it).  BULK queues may be created `KWQ_F_STEALABLE`: an idle
  BULK worker may take a batch from another CPU's queue of that kind, in
  whole-batch units, preserving FIFO within the batch but not across
  CPUs.  Stealing follows cache topology outward (SMT sibling, then
  L3/cache domain, then package, then any) using the topology
  `sched_ule` already builds; this is Linux's affinity scope as a default
  policy rather than a knob [L9].

## 2. Objects and their lifecycle

    struct kwq_item {                     /* embedded in the client's object */
        STAILQ_ENTRY(kwq_item) kwi_link;
    };
    typedef void kwq_handler_t(struct kwq *q, struct kwq_item *head,
                               int n, void *ctx);

    enum kwq_class { KWQ_NET, KWQ_BULK, KWQ_BLOCKING };

    struct kwq *kwq_create(const char *label, enum kwq_class cls,
                           uint32_t flags, const struct kwq_params *p,
                           kwq_handler_t *fn, void *ctx);
    void        kwq_activate(struct kwq *q);          /* publish */
    int         kwq_enqueue(struct kwq *q, int cpu, struct kwq_item *it);
    int         kwq_enqueue_list(struct kwq *q, int cpu,
                                 struct kwq_item *head,
                                 struct kwq_item *tail, int n);
    int         kwq_scatter(struct kwq *q, struct kwq_item *list, int n,
                            void (*done)(void *), void *arg);  /* BULK */
    void        kwq_drain(struct kwq *q);             /* run or discard all */
    void        kwq_destroy(struct kwq *q);

    struct kwq_params {
        u_int   limit;      /* items per CPU queue; 0 = class default */
        u_int   weight;     /* 1..8, default 1 */
        u_int   nrescue;    /* BLOCKING + KWQ_F_RESCUE only */
        int     domain;     /* NUMA domain for internal memory, or -1 */
    };
    #define KWQ_F_INACTIVE   0x01   /* create unpublished; kwq_activate() */
    #define KWQ_F_STEALABLE  0x02   /* BULK: idle workers may take batches */
    #define KWQ_F_DISCARD    0x04   /* BLOCKING: items may be dropped on
                                       destroy instead of run */
    #define KWQ_F_RESCUE     0x08   /* BLOCKING: nrescue dedicated threads */
    #define KWQ_F_VNET       0x10   /* handler runs CURVNET_SET(q->kwq_vnet) */

**Allocation happens exactly twice, both at configuration time.**
`kwq_create()` allocates the queue object and its per-CPU state (heads,
tails, counters, a `struct mtx` per CPU queue) with `M_WAITOK` in a
context that may sleep; it registers the label under
`kern.kwq.<label>`, allocates `counter(9)` cells, and returns an
unpublished queue.  `kwq_activate()` publishes it (a single store under
the per-CPU queue locks) - the create-then-activate split is the cloner
create-return-window lesson: nothing can be enqueued before the handler
and storage exist [../DISPATCH.md S10].  A queue created without
`KWQ_F_INACTIVE` is activated by `kwq_create()` itself before returning.
Nothing is allocated on the data path: items are embedded in client
objects, the drain uses the per-CPU list heads, and the handler receives
an intrusive list.

**Release happens exactly once**, in `kwq_destroy()`, which requires the
queue to be quiesced first by `kwq_drain()` and asserts it.  `kwq_drain()`
is the unload contract: it deactivates the queue (later `kwq_enqueue()`
calls return ENXIO), then on every CPU either runs the pending items
through the handler (default) or, for `KWQ_F_DISCARD` queues, hands them
to the handler with `n < 0` meaning "discard these", and finally waits
for any in-progress batch on any CPU to finish.  `kwq_drain()` may sleep
and must be called from a context that can (module unload, cloner
destroy), never from a handler [L1 - the illumos/OpenBSD "never wait for
the queue from inside it" rule, asserted with `td_no_sleeping` and a
recursion check].

**Locking.**  Each (queue, CPU) has one mutex protecting its list and
state word.  `kwq_enqueue()` takes it, appends, and if the state is IDLE
sets WAKING and signals the CPU's class worker; one lock hold covers both
(the if_pair invariant: doorbell before publication, so a wakeup can
never be lost, ../NOTES.md).  The worker takes the same lock once per slice
to swap the list out (`mbufq_flush` shape), releases it, and runs the
handler with NO service lock held.  The handler may therefore call
`kwq_enqueue()` on any queue, including its own (self-enqueue lands in
the current CPU's list and is seen next slice), without lock-order
concerns; WITNESS sees the queue mutexes as leaves.  Locks the client
holds while calling `kwq_enqueue()`: anything non-sleepable is fine,
because enqueue never sleeps and never calls out; a spin mutex is fine
too (the queue mutex is a sleep mutex acquired uncontended in the fast
path, but see S6 for the ithread-filter case).  Stealing takes the victim
CPU's queue mutex, the only cross-CPU lock in the design.

## 3. API contracts for the client developer

**kwq_enqueue(q, cpu, item)**
- Preconditions: `q` activated; `item` not currently queued anywhere
  (the item's link is the client's proof; double-enqueue is a bug and is
  caught under INVARIANTS by a poisoned link, Windows' "corruption of
  system data structures" made impossible rather than documented).
- `cpu` is the CPU whose worker should run the item: the client's
  steering decision (flow hash, receive queue, or `curcpu` for "here").
  `KWQ_CPU_ANY` lets BULK pick the least loaded CPU in the caller's
  cache domain; it is invalid for NET (ordering).  An offline or
  non-existent CPU maps to the caller's CPU.
- Never sleeps, never allocates, never calls the handler.  Safe from any
  context that may take a sleep mutex: threads, ithreads, callouts,
  other handlers.  Not safe from interrupt FILTERS (S6).
- Returns 0, or ENOBUFS when the (queue, CPU) list holds `limit` items -
  the item is NOT queued and remains the caller's.  ENXIO after
  `kwq_drain()` began.  The client decides drop-and-count versus
  backpressure; the service counts rejections per queue per CPU [L6].
- Ordering: items enqueued to the same (queue, CPU) are delivered FIFO in
  one or more batches; nothing is promised across CPUs or across queues.
  This is netisr's `NETISR_POLICY_SOURCE` made explicit by the client
  choosing the CPU.

**kwq_enqueue_list(q, cpu, head, tail, n)**: as above for a pre-linked
list; all-or-nothing against the bound.

**kwq_scatter(q, list, n, done, arg)** (BULK only): distributes `n` items
over the CPUs of the caller's cache domain, in chunks sized to the class's
idle capacity, and calls `done(arg)` from the worker that finishes last.
The `dispatch_apply` + `dispatch_group_notify` pair as one call; it
nests by dividing the budget (the SEDA/libdispatch lesson) and never
waits [L8].

**The handler `fn(q, head, n, ctx)`**
- Receives a private list of `n` items in FIFO order and OWNS them; it
  may process, re-enqueue, free, or hand them on.  It must consume every
  item (the service keeps no reference).  `n < 0` on `KWQ_F_DISCARD`
  queues means "release these without processing" during drain.
- Runs on the worker of the CPU the items were enqueued to (NET, and BULK
  unless stolen), with the class's priority, inside the network epoch for
  NET (the `NET_TASK_INIT` convention), with `CURVNET_SET` to the queue's
  vnet if `KWQ_F_VNET`, and with `THREAD_NO_SLEEPING()` asserted for NET
  and BULK.
- Is given a budget it can query: `kwq_budget_left(q)` returns the
  nanoseconds remaining in the current quantum.  A handler processing a
  long list should stop when the budget is gone, re-enqueue the
  remainder to itself with `kwq_enqueue_list`, and return; the service
  then treats the remainder as new work in the next round.  A handler
  that ignores the budget is preempted by nothing (it is at ithread
  priority) but is accounted, and its queue is flagged (S5); the service
  does not kill it.
- Must not call `kwq_drain()`, `kwq_destroy()`, or any function that may
  sleep (NET/BULK), and must not hold any service lock on return (it
  cannot: none is passed).

**kwq_drain / kwq_destroy**: sleepable contexts only; `kwq_destroy`
asserts drained; both are idempotent for the destroy-after-drain
sequence and panic on misuse under INVARIANTS.

## 4. Integration with existing kernel services

- **taskqueue(9) / gtaskqueue(9)**: kwq is implemented as an extension
  of gtaskqueue's per-CPU group machinery (its attach/detach/drain and
  boot-time reattachment are reused), and `taskqueue(9)` gains a
  compatibility shim: `taskqueue_create_kwq(class)` returns a taskqueue
  whose `taskqueue_enqueue` maps to `kwq_enqueue(q, curcpu, ...)` with
  one item per task, so existing single-task drivers migrate by
  changing the create call.  The `TASK_IS_NET` flag becomes the NET
  class.
- **netisr(9)**: netisr's workstreams are the NET class's first
  in-tree client: each `netisr_handler`'s per-protocol queue becomes a
  kwq NET queue whose CPU is chosen by the existing `m2cpuid`/`m2flow`
  policy functions; `net.isr.maxthreads`/`bindthreads` become no-ops
  (the pool is always per-CPU), `net.isr.dispatch` keeps its meaning
  (direct dispatch bypasses the queue exactly as today).  This retires
  the boot-frozen single-thread default without changing a protocol.
- **epoch(9)**: NET handlers run in `net_epoch_preempt`; the service
  enters it once per batch, not per item.  Epoch reclamation callbacks
  keep their own `qgroup_softirq` context (BULK-priority, un-stealable)
  and the NET worker's end-of-round yield is what guarantees they run
  (S5); this answers ../POOLS.md's open question by keeping reclamation OUT
  of the NET class.
- **callout(9)**: kwq does not schedule time; a client that needs a
  timer arms a callout whose function enqueues.  The service's own use of
  callouts is limited to the BLOCKING pool's idle reaper and, initially,
  the runnability monitor.
- **ithreads and filters**: an interrupt handler (ithread) may
  `kwq_enqueue()` directly.  A FILTER (primary interrupt context, spin
  locks only) may not, because the queue mutex is a sleep mutex; filters
  should schedule their ithread (`FILTER_SCHEDULE_THREAD`) which then
  enqueues, exactly as they do for taskqueues today.  A future
  `kwq_enqueue_filter()` using a spin-mutex-protected per-CPU staging
  list is possible but not in the first version.
- **cpuset(9), NUMA, hot-plug**: workers are created for `CPU_FOREACH`
  at boot and follow `cpu_online`/`cpu_offline` events (the GELI
  `hlt_cpus_mask` check generalised): an offlined CPU's queues are
  drained to a neighbour once and its workers parked.  Per-queue internal
  memory is allocated from the `domain` given at create, default the
  CPU's own.
- **VNET**: queues are global objects; a handler that must run in a vnet
  gets it from the queue (`KWQ_F_VNET`) or from the item (an mbuf's
  `rcvif`), as if_pair does today.  `kwq_drain()` is the vnet-teardown
  contract: a cloner's `destroy` drains its queues before detaching, so
  no item outlives the interface it references (the if_pair teardown
  argument, ../NOTES.md).
- **Module unload**: `kwq_destroy()` in the module's SYSUNINIT at
  `SI_SUB_TASKQ` order AFTER cloner teardown, the ordering if_pair
  adopted because `MOD_UNLOAD` events fire before file SYSUNINITs.
- **WITNESS / INVARIANTS**: the queue mutexes are registered as leaf
  locks; handlers run under `THREAD_NO_SLEEPING` (NET/BULK) and, in
  debug kernels, `WITNESS_WARN(WARN_PANIC | WARN_GIANTOK, NULL, "kwq
  handler")` at entry and a check at exit that no locks were leaked
  (`witness_warn` with the entry lock count).
- **sysctl(9) and DTrace**: `kern.kwq.<class>.<label>.cpu<N>.{items,
  bytes?, cycles, slices, expiries, rejected, maxlat_ns}` as `counter(9)`
  where possible; a `kern.kwq.<class>.quantum_us` per class (RW, default
  200 us NET, 1 ms BULK, none for BLOCKING); SDT probes
  `kwq:::enqueue(q, cpu, n)`, `kwq:::reject(q, cpu)`, `kwq:::slice-start(q,
  cpu, n)`, `kwq:::slice-end(q, cpu, ns)`, `kwq:::expire(q, cpu, ns)`,
  `kwq:::yield(cpu, round_ns)`, `kwq:::steal(q, from, to, n)`.  This is the
  observability that every system in ../POOLS.md eventually rebuilt its
  deferral mechanism to obtain [L3].
- **Panic and debugger**: `show kwq` in DDB lists queues, per-CPU depths,
  states and the last handler run; the worker's `td_name` and the queue
  label appear in backtraces.

## 5. Accounting: fairness, latency, throughput

**Unit of account: CPU time, not items** [L5].  Every slice is bracketed
by `cpu_ticks()`/TSC reads; the difference is charged to the queue's
per-CPU `cycles` counter and to its DRR deficit.

**Fairness: deficit round robin per (class, CPU).**  A worker keeps the
CPU's active queues of its class in a ring.  Each round, each queue's
deficit is increased by `quantum * weight`; the worker swaps out the
queue's list and runs the handler while the deficit is positive, charging
consumed time; a queue whose list empties keeps its unused deficit up to
one quantum (so bursty clients are not penalised) and one whose handler
overran is parked with a negative deficit and skipped until it recovers.
With one active queue this degenerates to "drain until empty or
quantum", i.e. today's behaviour at zero extra cost (the libdispatch
observation that fairness machinery costs nothing until there is someone
to be fair to).  Between classes there is no scheduling at all: NET and
BULK are different threads at different fixed priorities, arbitrated by
ULE exactly as ithreads and softclock are today.

**Latency: the quantum and the round.**  A NET quantum of 200 us bounds
how long any one queue can hold the worker; a round over k active queues
bounds the queueing delay of a new item at roughly k quanta.  At the end
of every round, or when a `hz` tick has elapsed since the worker last
yielded (if_pair's tick check, kept as the safety net for long items),
the worker performs the demote-yield: `kern_yield(PRI_USER)` then restore
the class priority, giving softclock, epoch callbacks, BULK, BLOCKING and
userland a turn.  Without this a saturated NET worker starves its own
CPU's timers - the callout-starvation the 128-core measurements exposed
(../NOTES.md 2026-08-20).  Per-item queueing latency is sampled (enqueue
timestamp in the item is NOT stored - items are the client's - but the
per-slice `maxlat_ns` records the age of the oldest item at slice start,
using the timestamp of the first enqueue into an empty list, kept in the
per-CPU queue state).

**Jitter: no punting, time-based budgets, pinned NET.**  A parked queue
resumes in the next round at the same priority; there is no
lower-priority fallback thread for overrun work (Linux's ksoftirqd cliff),
so a client's tail latency is bounded by its own overrun plus one round.
Budgets are time, so a 64 KB TSO chain and a 64-byte ACK are charged
what they cost.  NET is never stolen, so a flow's items never wait behind
a cross-CPU cache miss they did not cause.

**Throughput: batching everywhere it is free.**  The handler receives a
whole list (one lock hold per slice, not per item); the doorbell is
rung only on IDLE->WAKING (one wakeup per burst); NET enters the network
epoch once per slice; BULK stealing moves whole batches.  The quantum is
the throughput/latency dial and is a per-class sysctl; the defaults are
chosen so that a quantum is one to two orders of magnitude longer than
the per-item cost of the heaviest common item (a TSO chain through
`ip_input` is ~10 us, so 200 us is ~20 chains).

**What is exposed so a controller can be added later** [L8]: per-queue
expiries (a queue that keeps overrunning its quantum is either
mis-classed or should re-enqueue its remainder), per-CPU round length,
rejection counts (a queue at its bound needs a larger bound or upstream
backpressure), and steal counts (BULK locality vs utilisation).  No
automatic adjustment in the first version; the numbers exist so that the
Linux-style "flag after 10 ms" or a SEDA-style batching controller can be
justified by data rather than added speculatively.

## 6. What a handler may do, by class

| | NET | BULK | BLOCKING |
|---|---|---|---|
| sleep (`tsleep`, `cv_wait`, sx/lockmgr, `M_WAITOK`, `uiomove`, `copyin`) | never (asserted) | never (asserted) | yes |
| take sleep mutexes / rw / rm locks | yes, briefly | yes | yes |
| take spin mutexes | yes (leaf, brief) | yes | yes |
| `critical_enter`/`sched_pin` | yes, must exit before return | yes | yes |
| `kwq_enqueue` to any queue incl. own | yes | yes | yes |
| `kwq_drain` / `kwq_destroy` | never | never | never (from a handler) |
| block on another queue's completion | never | never | only via `done` callback |
| long computation | allowed but charged; check `kwq_budget_left()` and re-enqueue remainder | same, larger quantum | allowed; the pool spawns a worker if this one blocks, not if it computes |
| voluntary yield | `kwq_yield(q)`: ends the slice cleanly (remainder re-enqueued by the service) | same | `maybe_yield()` also fine |
| allocate | `M_NOWAIT` only; handle NULL | `M_NOWAIT` only | `M_WAITOK` permitted; reserve for reclaim clients |
| run in network epoch | always (entered by service) | no (enter it yourself) | no |
| touch user memory | never | never | with the usual `copyin` rules |
| re-enter the stack synchronously | never call a peer's input path inline from a NET handler if it can loop back to a lock the caller of `kwq_enqueue` holds - the queue IS the reentrancy break (the 2026-08-14 postmortem) | n/a | n/a |

Two rules deserve emphasis because every surveyed system got burned by
them: (a) "long computation" is legal but budgeted - the right pattern
is to process until `kwq_budget_left()` is exhausted, re-enqueue the
rest, return; the service records an expiry if a handler does not, and a
queue that expires persistently is visible in `expiries` and DDB; (b)
"waiting for the queue" from inside it is illegal in every class, and
asserted [L1].

## 7. The programming model, in one page

A component that today creates a taskqueue or a thread does this:

    static struct kwq *pair_q;

    static void
    pair_handler(struct kwq *q, struct kwq_item *head, int n, void *ctx)
    {
        struct mbuf *m, *next;                 /* items are mbufs here */
        for (m = KWQ_ITEM_TO_MBUF(head); m != NULL; m = next) {
            next = KWQ_NEXT_MBUF(m);
            pair_input(ifp_of(m), m);          /* runs in net epoch */
            if (next != NULL && kwq_budget_left(q) == 0) {
                kwq_enqueue_list(q, curcpu, KWQ_MBUF_ITEM(next), ..., n_left);
                return;
            }
        }
    }

    /* attach: sleepable context */
    pair_q = kwq_create("pair", KWQ_NET, KWQ_F_INACTIVE, NULL,
                        pair_handler, NULL);
    ... finish configuring ...
    kwq_activate(pair_q);

    /* transmit path: any non-filter context */
    if (kwq_enqueue(pair_q, flowid % mp_ncpus, KWQ_MBUF_ITEM(m)) != 0) {
        m_freem(m);
        if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
    }

    /* detach: sleepable context, after the interface is down */
    kwq_drain(pair_q);
    kwq_destroy(pair_q);

The developer decides three things and nothing else: the class (may my
handler sleep, and is it packet-latency work or bulk compute), the CPU
per item (my ordering domain), and what to do on ENOBUFS.  They get, for
free, per-CPU pinned execution at the right priority, FIFO per CPU, a
lost-wakeup-proof doorbell, epoch entry, fairness against every other
client on the CPU, a bounded hold time, per-CPU counters and probes
under their label, and a teardown contract that composes with cloner
destroy and module unload.  They give up: choosing a priority, owning
threads, synchronous completion, and unbounded batches.

## 8. Migration and proof

Order, from ../POOLS.md L10 (infrastructure ships with consumers):

1. Land kwq with **if_pair and epair** converted together (identical
   shape; epair is in-tree, and its RSS-only pool becomes unconditional
   per-CPU for free).  The t_17/t_19/t_20 suite on the Ampere is the
   acceptance test: no regression at P=32 peak, no callout starvation at
   P=128, per-client counters replacing the DTrace archaeology.
2. **netisr** onto the NET class (retires `net.isr.maxthreads=1`).
3. **if_wg** onto BULK with `KWQ_F_STEALABLE` and `kwq_scatter` for
   crypto, serial per-peer NET queues for delivery.
4. **BLOCKING** with concurrency management and `KWQ_F_RESCUE`; only
   then GELI.
5. **iflib** last, its `if_io_tqg` being the largest user and the most
   conservative reviewer base.

Non-goals for the first version: queue hierarchies or target queues,
priority inheritance, synchronous execution APIs, per-item priorities,
automatic quantum tuning, filter-context enqueue.  Each was either the
source of a documented mistake elsewhere (../DISPATCH.md S6, ../POOLS.md L4,
L8) or is addable without changing a contract above.

## 9. Locks or Concurrency Kit lock-free structures for the per-CPU queues?

Locks by default; a lock-free ring as a per-class backend where a
measured hot spot or a context constraint demands it.  The reasons are
kernel-specific and mostly not about speed.

**Why a mutex is the right default**

1. **Preemption and priority.**  Every lock-free MPSC list publishes in
   two steps (exchange the tail, then link the predecessor); between them
   the consumer cannot traverse and must spin (libdispatch's
   `_dispatch_wait_for_enqueuer`).  In the kernel the producer can be
   preempted or migrate between those two stores, and the spinning
   consumer is a `PI_NET` thread: a priority inversion with nothing to
   resolve it.  A mutex has turnstiles: the owner is lent the waiter's
   priority and an adaptive spinner stops spinning when the owner is not
   running.  The lock-free mitigation is `critical_enter()` around the
   publish, which costs about what the uncontended lock does.
2. **Observability.**  Everything the big-iron investigation learned came
   from the `lockstat` provider, WITNESS and turnstile accounting
   (../NOTES.md, t_20).  A lock-free structure is invisible to all three
   unless it grows its own probes; contention shows up as unexplained
   cycles.
3. **The doorbell protocol.**  The lost-wakeup proof with a lock is two
   lines: enqueue task before item under one hold; re-check emptiness
   under the lock before declaring IDLE.  The lockless equivalent is the
   DIRTY-bit three-step exit in libdispatch (`queue_internal.h:183-295`),
   the longest comment in that file, with its own documented race.
   Concurrency Kit gives you a correct queue; it does not give you a
   correct idle/wake protocol around it.
4. **Measured gain is small.**  The per-item lock is uncontended in the
   common case (one `fcmpset_acq` + one `fcmpset_rel`, the same cacheline
   traffic as an exchange plus a store), the consumer takes it once per
   slice, and on 128 cores at P=128 the pair queue mutexes cost ~0.17
   CPU-equivalents (`pairq` block + spin, t_20).  The win is the hold
   window and the adaptive-spin/turnstile fallback, worth less than that.
5. **Batch enqueue.**  `kwq_enqueue_list()` is an O(1) `STAILQ_CONCAT`
   under the lock; a ring needs one CAS per element.

**Where a lock-free ring wins, and which one**

- `ck_ring` (MPSC enqueue, SPSC dequeue by the pinned consumer) is
  already in the tree and in production use (`kern/subr_epoch.c` via
  `ck_epoch`; `ck_ring` in rack, gve, cxgb, mps/mpr, xen netback, qat).
  It is bounded by construction, so ENOBUFS is structural rather than a
  counter; it holds pointers, so mbufs need no `m_nextpkt` link; and it
  has no hold window, so many remote producers (NIC ithreads on other
  CPUs feeding one NET queue under a FLOW policy) cannot convoy on a
  lock line.  Its cost is `capacity x 8` bytes per (queue, CPU) and the
  loss of O(1) list splicing.
- **Filter context.**  Interrupt filters may take only spin locks; a
  `kwq_enqueue_filter()` is only possible on a lock-free ring (or a
  spin-mutex staging list).  This is the one case where the choice is
  forced rather than measured.
- `ck_fifo_mpmc` and `ck_hp_*` are the wrong tools here: they need
  per-node allocation or hazard-pointer reclamation, and our items are
  client-owned with no reclamation problem to solve.

**Design decision.**  The backend is a per-class property hidden behind
`kwq_enqueue()`: NET and BULK default to mutex + intrusive `STAILQ`
(batch-friendly, turnstile-safe, visible to lockstat); a queue may be
created `KWQ_F_RING` to use a `ck_ring` MPSC backend, in which case the
IDLE/WAKING/RUNNING word becomes a separate atomic driven by the
producer index observed at enqueue (empty-before-enqueue rings the
doorbell) and `critical_enter()` brackets the publish.  Switching a
queue's backend is a measurement decision, made with `lockstat` showing
the queue mutex above the noise, never a default.  The first version
ships the mutex backend only.

**Is a lock-free queue ever required?**  Not by the API as specified.
Every context the design admits as a producer - threads, ithreads,
callouts, other handlers - may take a sleep mutex, and a mutex is legal
under `THREAD_NO_SLEEPING()` and inside an epoch section (blocking on a
mutex is not sleeping).  The requirement appears only if the API is
extended to producers that may not take a sleep mutex:

- **interrupt filters** (primary interrupt context, spin locks only);
- **callers holding a spin mutex**, such as scheduler hooks under
  `thread_lock`, callout-wheel or sleepqueue-chain code - relevant if the
  BLOCKING class's blocked-worker detection is implemented inside
  `sched_switch`;
- **NMI or post-panic paths**, which are not supported producers and
  should stay that way (`mtx_lock` already degrades to a no-op once the
  scheduler is stopped, so `kwq_drain()` at shutdown needs nothing
  special).

Even there, lock-free is one of three options, and usually not the first:
(1) if producer and consumer are the same CPU (self-enqueue, a
`sched_switch` hook recording its own CPU's state), a CPU-local
structure under `critical_enter()` needs no lock and no atomics at all -
the DPCPU pattern; (2) for cross-CPU producers a spin-mutex staging list
is the kernel's conventional answer (the callout wheel's `cc_lock`, the
sleepqueue chains) - correct, visible to lockstat, and exactly the class
of lock the 128-core measurements found saturating first, so at high
fan-in it is the worse engineering choice; (3) a `ck_ring` MPSC backend
is the right answer when the producer context is constrained AND the
fan-in is high enough that a spin lock would be hot.  So: never required
for correctness in the first version; required by construction the day
`kwq_enqueue_filter()` or an in-scheduler hook is added; and preferable
on performance grounds only where a spin lock would otherwise sit on a
hot cross-CPU path.

**Decision for the first version: locked queues only, in two mutex
flavours.**  Checked against every intended client:

| client | producer contexts | needs |
|---|---|---|
| if_pair, epair | `if_output` callers: threads, netisr, ithreads, handlers | sleep mutex |
| netisr protocols | ithreads, iflib RX tasks, other handlers | sleep mutex |
| if_wg | `wg_transmit` (threads), tunnel socket upcall (netisr/ithread), handlers | sleep mutex |
| GELI (BLOCKING, later) | GEOM `g_down`/`g_up` threads, callers of `g_io_request` | sleep mutex |
| iflib (last) | **interrupt filters** (`iflib_fast_intr*` call `GROUPTASK_ENQUEUE`) | spin mutex |

iflib is the only client that enqueues from filter context, and FreeBSD
already solved that case with a lock: `taskqgroup_cpu_create()` builds
its per-CPU queues with `gtaskqueue_create_fast()`, i.e. `MTX_SPIN`, the
same choice as `taskqueue_create_fast()` in taskqueue(9).  kwq therefore
needs one flag, `KWQ_F_SPIN`, selecting a spin mutex for the (queue, CPU)
lists of a queue that filters feed; the consumer's hold is a single list
swap either way, and the handler runs outside the lock, so the cost of
the spin flavour is the interrupt disable around the swap and the
append, exactly what iflib pays today.  Everything else uses the sleep
mutex.  No lock-free backend, no `KWQ_F_RING`, no second doorbell
protocol in the first version; the API is unchanged if a ring backend is
added later behind the same `kwq_enqueue()`, justified by a `lockstat`
profile of a specific queue.  Complexity budget spent instead on the
things no client can do without: the class model, DRR in CPU time, the
yield, drain semantics, accounting.

## 10. DTrace visibility

The goal is that an operator can answer "who is loading this CPU, how
long do items wait, who is being throttled, and why" with one-liners,
without reading kernel headers.  FreeBSD's own `io`, `sched`, `ip` and
`tcp` providers show the pattern: statically defined tracing (SDT) probes
in the kernel with translated argument types in `/usr/lib/dtrace/*.d`, so
scripts name fields, not struct offsets.  kwq follows it exactly.

### 10.1 Provider and probes

`SDT_PROVIDER_DEFINE(kwq)`; module and function fields left empty as the
in-tree providers do, so probes read `kwq:::name`.  Probes are defined
with `SDT_PROBE_DEFINEn_XLATE` (the mechanism `ip`/`tcp` use) so that
`args[0]` is a translated `kwqinfo_t`, never a raw `struct kwq *`.

| probe | when | args (after translation) | cost class |
|---|---|---|---|
| `kwq:::create`, `activate`, `drain-start`, `drain-end`, `destroy` | lifecycle | `kwqinfo_t *` | negligible |
| `kwq:::enqueue` | every accepted item | `kwqinfo_t *`, `int cpu`, `int depth_after`, `int woke` (1 if this enqueue rang the doorbell) | per item: hot, see 10.3 |
| `kwq:::reject` | `kwq_enqueue` returned ENOBUFS/ENXIO | `kwqinfo_t *`, `int cpu`, `int errno` | per event |
| `kwq:::slice-start` | worker swapped a list and is about to run the handler | `kwqinfo_t *`, `int cpu`, `int n`, `uint64_t oldest_age_ns` | per slice |
| `kwq:::slice-end` | handler returned | `kwqinfo_t *`, `int cpu`, `int n`, `uint64_t ns`, `int remaining_reenqueued` | per slice |
| `kwq:::expire` | a slice overran its quantum | `kwqinfo_t *`, `int cpu`, `uint64_t over_ns` | per event |
| `kwq:::park` | queue parked with negative deficit | `kwqinfo_t *`, `int cpu`, `int64_t deficit_ns` | per event |
| `kwq:::round-end` | worker finished a DRR round | `int class`, `int cpu`, `int nqueues`, `uint64_t round_ns` | per round |
| `kwq:::yield` | the demote-yield taken | `int class`, `int cpu`, `int reason` (round / tick) | per round |
| `kwq:::idle` | worker found nothing and went to sleep | `int class`, `int cpu`, `uint64_t busy_ns` | per burst |
| `kwq:::steal` | BULK batch moved between CPUs | `kwqinfo_t *`, `int from`, `int to`, `int n` | per event |
| `kwq:::worker-block`, `worker-spawn`, `worker-exit` | BLOCKING pool management | `int cpu`, `int nworkers`, `int nrunnable` | per event |
| `kwq:::budget-hit` | a handler asked `kwq_budget_left()` and received 0 | `kwqinfo_t *`, `int cpu` | per event |

Everything an operator would otherwise compute is exposed as an
argument: the age of the oldest item at slice start (queueing latency
without per-item timestamps), whether an enqueue caused a wakeup (the
doorbell coalescing ratio), the deficit at park time, the remaining
items a cooperative handler re-enqueued.

### 10.2 Translator (`/usr/lib/dtrace/kwq.d`)

    typedef struct kwqinfo {
        string   kwq_label;        /* "pair", "netisr/ip", "wg/crypto" */
        string   kwq_class;        /* "net", "bulk", "blocking" */
        int      kwq_weight;
        uint32_t kwq_limit;        /* per-CPU bound */
        uint32_t kwq_flags;
        uintptr_t kwq_addr;        /* for correlating with lockstat/fbt */
    } kwqinfo_t;

    translator kwqinfo_t < struct kwq *Q > {
        kwq_label  = stringof(Q->kwq_label);
        kwq_class  = Q->kwq_class == 0 ? "net" : Q->kwq_class == 1 ? "bulk" : "blocking";
        kwq_weight = Q->kwq_weight;
        kwq_limit  = Q->kwq_limit;
        kwq_flags  = Q->kwq_flags;
        kwq_addr   = (uintptr_t)Q;
    };

The label is a fixed-size array in `struct kwq` (not a pointer to client
memory) so `stringof` is always safe, including during drain/destroy.
A `dtrace_kwq(4)` manual page documents probes and types alongside the
existing `dtrace_io(4)`, `dtrace_sched(4)`, `dtrace_tcp(4)`.

### 10.3 Cost control

- The per-slice, per-round and per-event probes are cheap by
  construction and always compiled in.
- The per-item `kwq:::enqueue` probe sits on the hottest path in the
  system.  A disabled SDT probe costs a predicted-not-taken branch on
  `sdt_probes_enabled`; argument computation is guarded with
  `SDT_PROBES_ENABLED()` (as `tcp_input` does) so `depth_after`/`woke`
  are only evaluated when someone is tracing.  When enabled, at ~1M
  items/s per CPU the probe is expensive and the scripts below are
  written so the common questions never need it: slice-level data
  answers them.
- No probe takes a lock; all arguments are read from the per-CPU queue
  state the worker or enqueuer already holds or just published.

### 10.4 What the workers look like to other providers

- Threads are named `kwq_net/N`, `kwq_bulk/N`, `kwq_blk/N`, so
  `sched:::on-cpu`, `sched:::off-cpu` and the `profile` provider
  attribute CPU time per worker with `curthread->td_name` and per class
  with a prefix match; per-client attribution comes from `kwq:::slice-*`.
- The (queue, CPU) mutexes carry the label in their lock name
  (`"kwq pair"`), so the `lockstat` provider reports contention per
  queue - the tool that found the callout-wheel and mbuf-zone hot spots
  keeps working unchanged.
- `fbt::kwq_enqueue:entry` with `stack()` answers "who is feeding this
  queue"; `fbt` on the handler symbol answers "what does this client do
  per slice".

### 10.5 Always-on counters and DDB

DTrace is for investigation; steady-state health needs no probe enabled.
Every (queue, CPU) exports `counter(9)` cells under
`kern.kwq.<class>.<label>.cpu<N>.{items,slices,cycles,expiries,parks,
rejected,steals,maxlat_ns}` plus class-level `kern.kwq.<class>.cpu<N>.
{rounds,yields,idle_ns}`; `sysctl kern.kwq` is the first thing to look
at, and `dtrace` the second.  `show kwq` in DDB prints every queue's
per-CPU depth, state (IDLE/WAKING/RUNNING/PARKED) and the last handler
run, for the post-mortem case.

### 10.6 One-liners the design is built to support

Queueing latency per client (age of the oldest item when its slice began):

    dtrace -n 'kwq:::slice-start { @[args[0]->kwq_label] = quantize(arg3 / 1000); }'

Which clients overrun their quantum, and from where:

    dtrace -n 'kwq:::expire { @[args[0]->kwq_label, args[0]->kwq_class] = count(); }'
    dtrace -n 'kwq:::expire /args[0]->kwq_label == "pair"/ { @[stack()] = count(); }'

Doorbell efficiency (wakeups per enqueue; near 0 = good batching):

    dtrace -n 'kwq:::enqueue { @e[args[0]->kwq_label] = count(); @w[args[0]->kwq_label] = sum(arg3); }'

Per-CPU imbalance of a client's work:

    dtrace -n 'kwq:::slice-end /args[0]->kwq_label == "netisr/ip"/ { @[arg1] = sum(arg3); }'

Who is dropping, and who feeds the queue that drops:

    dtrace -n 'kwq:::reject { @[args[0]->kwq_label, arg1, arg2] = count(); }'
    dtrace -n 'fbt::kwq_enqueue:return /arg1 != 0/ { @[stack()] = count(); }'

Is the yield doing its job (rounds that hit the tick guard instead of
finishing naturally):

    dtrace -n 'kwq:::yield { @[arg1, arg2 == 1 ? "tick" : "round"] = count(); }'

CPU time by class versus everything else on a CPU:

    dtrace -n 'profile-997 { @[curthread->td_name] = count(); }'

Where a BLOCKING worker blocks, when the pool spawns:

    dtrace -n 'kwq:::worker-block { @[stack()] = count(); } kwq:::worker-spawn { printf("cpu %d -> %d workers", arg0, arg1); }'

Each of these replaces a step of the DTrace archaeology that the 128-core
investigation had to do by hand against unnamed taskqueue threads and
unlabelled mutexes; the probes exist so that the next operator does not.

## 11. What FreeBSD already provides, and what is missing

Checked against the `releng/15.0` tree (2026-09-23).  Almost everything
exists; three things do not, and one of them changes a claim this project
has been making about its own driver.

### Present and sufficient

| need | facility |
|---|---|
| pinned per-CPU worker threads at a fixed priority | `kthread_add(9)` + `sched_bind()`/cpuset, as `taskqueue_start_threads_cpuset()` does |
| non-sleeping enforcement in NET/BULK handlers | `THREAD_NO_SLEEPING()` / `THREAD_SLEEPING_OK()` (`sys/proc.h`, the epoch(9) mechanism); `WITNESS_WARN(WARN_PANIC, ...)` for lock-leak checks |
| doorbell-safe queue lock, filter-context variant | sleep mutex; `MTX_SPIN` for filter producers (the `gtaskqueue_create_fast()` precedent) |
| priority lending under contention | turnstiles, adaptive mutexes |
| per-slice CPU time | `cpu_ticks()` (TSC-backed, what ULE charges `td_runtime` with) |
| tick guard | `ticks` |
| per-CPU state without atomics | DPCPU, `critical_enter()` |
| counters | `counter(9)`, `SYSCTL_ADD_COUNTER_U64`, dynamic sysctl nodes per label |
| network epoch per slice | `NET_EPOCH_ENTER()`; `NET_TASK_INIT` shows the convention |
| vnet context | `CURVNET_SET()` |
| cache topology for BULK stealing | `smp_topo()` / `struct cpu_group` (what ULE itself uses) |
| DTrace with typed, translated arguments | `SDT_PROBE_DEFINEn_XLATE`, translators in `cddl/lib/libdtrace/*.d` (`ip.d`, `io.d`) |
| lock contention per queue | `lockstat` provider keyed by the `mtx_init` name |
| post-mortem | `DB_SHOW_COMMAND` |
| lifecycle ordering | `SYSINIT/SYSUNINIT(SI_SUB_TASKQ)`, cloner destroy ordering (if_pair's) |

Two design cautions that need no new feature: worker wait channels
should live in cache-line-padded per-worker structures, because sleep
channels hash into 256 chains by address (`SC_HASH`, `subr_sleepqueue.c`)
and t_20 already showed `sleepq_chain` contention with ~130 workers; and
`kern_yield()` must be followed by `sched_prio()` back to the class
priority, as if_pair does.

### Missing 1: bounded deference to lower priority classes (the yield)

The design's latency and fairness story rests on the end-of-round
demote-yield, `kern_yield(PRI_USER)` then restore.  Reading `kern_yield()`
shows what it actually does for a kernel thread:

    if (prio == PRI_USER) prio = td->td_user_pri;
    sched_prio(td, prio); mi_switch(SW_VOL | SWT_RELINQUISH);

and `td_user_pri` of a kernel thread is inherited from thread0, `PUSER`
= 56 = `PRI_MIN_TIMESHARE` - the BEST timeshare priority.  So the worker
drops below every ithread, softclock (`PI_SOFT`) and the kernel band
(40-55), which is what it was built for and does achieve (the callout
starvation fix holds), but it stays above every user thread except those
ULE scores as maximally interactive (also 56); CPU-bound "batch" user
threads sit at `PRI_MIN_BATCH` and higher and never get the CPU from a
saturated worker on that CPU.  ULE's balancer will migrate them to other
CPUs on an SMP machine, so this is starvation of a CPU, not of a process,
and it is why the 128-core runs never showed it; on a uniprocessor or a
fully saturated machine it is real.  Mogul and LRP's requirement, that
protocol processing "guarantee some progress for user-level code", is
therefore NOT met by `kern_yield(PRI_USER)`, and FreeBSD has no primitive
that meets it: nothing lets an ithread-class thread say "run anyone lower
than me for at most N microseconds".  Dropping to `PRI_MAX_TIMESHARE`
instead would let all user threads run but with no bound (they would run
full slices, tens of ms), destroying NET latency.

Options, in order of preference:
(a) **Emulate with existing primitives** (Mogul's "limit on CPU usage"
    feedback): each worker tracks its busy fraction over a short window;
    when it exceeds a class cap (say 90 %) and the CPU has runnable
    timeshare threads, it sleeps for a fixed short interval with
    `pause_sbt(..., C_PREL(1))` at its normal priority instead of the
    plain yield.  Bounded (the interval), tunable, no scheduler change;
    cost is one callout arm per cap event and added jitter equal to the
    interval.  Good enough for a first version.
(b) **A scheduler primitive**: `sched_relinquish_to(prio, sbintime_t
    max)` - demote to `prio`, switch, and have ULE restore the base
    priority and requeue the thread when `max` elapses or when the
    lower-priority queue empties.  ~50 lines in `sched_ule.c` plus 4BSD
    parity; the honest fix, and the one worth proposing on
    freebsd-arch because netisr, iflib and every pinned taskqueue at
    `PI_NET` have the same unstated gap today.

### Missing 2: a "worker is about to sleep" hook for the BLOCKING class

cmwq's concurrency management (../POOLS.md S3) relies on the scheduler
calling into the workqueue when a worker thread blocks.  FreeBSD has the
natural hook point - `sched_sleep(td, prio)` is called from
`sleepq_switch()` with the thread lock held - but no callback mechanism.
Adding one is small (a `TDP_`-style per-thread flag and a function
pointer, ~20 lines in `sched_ule.c`/`sched_4bsd.c`), and the hook body
must be spin-lock-only (it runs under `thread_lock`), which is exactly
the case S9 identified as needing CPU-local state under
`critical_enter()` rather than the queue mutex.  Interim: the 1 Hz
runnability poll over the pool's threads (libdispatch's monitor, in
kernel), which needs no change and is adequate until GELI joins.

### Missing 3: CPU online/offline notifications

FreeBSD has no CPU hot-plug events to subscribe to; CPUs are enumerated
once (`CPU_FOREACH`, `hlt_cpus_mask` for administratively idle ones).
Workers are therefore created for all CPUs at boot; a halted CPU still
runs bound threads when an IPI wakes it, so items enqueued there are
delayed, not lost.  Acceptable for a first version; if real hot-plug
arrives, the pool needs a drain-to-neighbour path and an event.

### Consequence for if_pair today

`if_pair(4)`'s description of `net.link.pair.batch` ("so that timers and
user processes keep running under sustained load") overstates the user
half: the yield restores timers and kernel threads; user processes keep
running only if interactive or migrated by ULE.  The manual page is
corrected to say so, and ../NOTES.md records the finding.  The driver's
behaviour is unchanged; the gap is FreeBSD's, and the same wording
applies to every `PI_NET` worker pool in the tree.

## 12. Module-first implementation

Yes: the pool itself, its first clients, its DTrace provider and its
tests can all live in loadable modules, and the absence of CPU hot-plug
makes that simpler, not harder.  What cannot be a module is the two
scheduler additions of S11 and the in-tree integrations (netisr,
taskqueue shim), which are patches against `/usr/src` developed
alongside.  This is how gtaskqueue itself arrived: inside iflib, then
moved to `kern/subr_gtaskqueue.c` (commit 23ac9029f96b).

**Everything the pool needs is exported KPI.**  `kthread_add`,
`sched_bind`, `sched_prio`, `kern_yield`, `mtx_init` (both flavours),
turnstiles, `THREAD_NO_SLEEPING()` (a macro on `curthread`, the field is
part of the KBI modules already compile against), `WITNESS_WARN`,
`NET_EPOCH_ENTER`, `counter(9)`, dynamic sysctl, `cpu_ticks`, `smp_topo()`,
`pause_sbt`.  SDT providers are routinely defined in modules
(`netpfil/pf/pf.c`, `dev/random/fortuna.c`, `dev/ice`): the `sdt`
framework registers them at load and tears them down at unload.  DDB
commands from modules are supported (`ddb/db_command.c` registers a
module's `db_show_cmd_set` on load).  The translator is a file in
`cddl/lib/libdtrace/`, which for a module lives in the module's
distribution and is installed to `/usr/lib/dtrace/kwq.d`.

**Shape.**

    kwq.ko          the service: workers, queues, DRR, yield, sysctl,
                    SDT provider, DDB command.  MODULE_VERSION(kwq, 1).
    kwq_test.ko     synthetic clients: N queues with handlers of known
                    cost, counters checked by an ATF test in tests/
                    (fairness: two queues, equal weight, cycles within
                    5 %; latency: oldest-item age never exceeds
                    quantum x active queues; yield: softclock keeps
                    firing under saturation; drain: no item lost or run
                    twice across 1000 load/unload cycles).
    if_pair.ko      first real client: MODULE_DEPEND(if_pair, kwq, 1, 1, 1),
                    pool code removed, t_17/t_19/t_20/t_21 as the
                    regression suite on the Ampere.
    if_epair.ko     patched copy of the in-tree driver as the second
                    client, built out of tree (the epair TSO patches
                    already use this workflow).

**No hot-plug is a simplification.**  `mp_ncpus`/`CPU_FOREACH` are fixed
after boot, so a module loaded at runtime sizes every per-CPU array once
at load and never revisits it; there is no online/offline path to test.
The one boot-time subtlety is the preloaded case (`kwq_load="YES"`):
SYSINITs of a preloaded module run at their subsystem order, before the
APs are released, so workers must not bind until `smp_started` - GELI's
pattern, each worker sleeping on `smp_started` before `sched_bind`
(`geom/eli/g_eli.c`), rather than binding the loading thread (the boot
hang if_pair hit).  A runtime `kldload` never sees this.

**What the module gives testing that an in-tree implementation would not.**
`kldload`/`kldunload` cycles exercise `kwq_drain`/`kwq_destroy` and worker
teardown thousands of times an hour, the lifecycle area that produced
epair's unload-vs-jail-removal deadlock and the create-return panic; the
service can be swapped under a running if_pair to A/B the mutex versus a
later ring backend without rebooting the Ampere (the `LD_LIBRARY_PATH`
trick of the libdispatch work, in kernel form); DTrace probes register
and unregister with the module, so the provider is tested as a unit; and
clients declare `MODULE_DEPEND`, so the linker refuses to unload the pool
while any client holds queues, which is the correct contract and free.

**Limits of the module form.**  (1) The bounded relinquish and the
`sched_sleep` hook need kernel patches; the module uses the `pause_sbt`
cap and the polling monitor until they land, and both fallbacks stay as
the module's behaviour on unpatched kernels.  (2) netisr, the taskqueue
compatibility constructor and epoch-callback placement are in-tree
changes and are developed as patches to the same tree the module is
built against, exactly as the TSO series was.  (3) A module cannot claim
`SI_SUB_TASKQ` ordering relative to in-tree consumers that start earlier;
irrelevant while all clients are modules that `MODULE_DEPEND` on it.
(4) Unload must be refused while queues exist (`EBUSY` from
`MOD_UNLOAD`), and all workers must be joined before the module text is
unmapped - taskqueue's `taskqueue_free` teardown is the template.

Graduation path: once if_pair, epair and wg run on `kwq.ko` and the two
scheduler patches are in review, the module moves to `kern/subr_kwq.c`
with the netisr conversion as its first in-tree client.
