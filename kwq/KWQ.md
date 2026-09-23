# kwq(9): a FreeBSD-native kernel work queue - design proposal

Draft 2026-09-23; terminology normalised 2026-09-24 - see GLOSSARY.md for
every defined term and the aliases it replaces.  Synthesis of ../NOTES.md (the if_pair pool, its governor
and the big-iron measurements), ../DISPATCH.md (libdispatch) and ../POOLS.md
(cross-OS survey and its ten lessons).  Names are illustrative; the
contracts are the point.  Where a rule is a direct consequence of a
lesson in ../POOLS.md S4, the lesson number is given as [L#].

## 0. One-paragraph summary

kwq is one system-wide service of per-CPU worker threads, organised in
three fixed WORK CLASSES (NET, BULK, BLOCKING; see GLOSSARY.md), to which kernel components
attach QUEUES.  A queue fixes, at creation, its class, its handler, its
ordering domain and its bound; an item is an intrusive link the client
owns.  Enqueue names a CPU and never allocates, never sleeps, and may
fail with ENOBUFS.  Each worker drains its CPU's queues in deficit
round robin measured in CPU time, with a hard quantum per queue and a
demote-yield at the end of every round so that softclock, epoch
reclamation and userland always run.  Every queue is accounted per CPU
(items, cycles, passes, quantum overruns, rejections, max queueing
delay) under sysctl and DTrace, by its name.  Clients never see a
thread, a priority, or a lock of the service.  Failure exists only at
configuration (`kwq_create`) and admission (`kwq_enqueue` rejecting with
the item handed back); everything else is void.  Enqueue is legal from
any interrupt thread, and from interrupt filters for `KWQ_F_SPIN` queues;
a handler that runs out of budget hands its leftovers back with
`kwq_requeue()`, which prepends and so keeps per-CPU FIFO.  Terms are
defined in GLOSSARY.md.

## 1. Threading model

- **Threads per (work class, CPU), not per client.**  At `SI_SUB_TASKQ` the
  service creates, for each online CPU, one NET worker and one BULK
  worker, bound to its CPU with `taskqueue_start_threads_cpuset`-style cpusets
  (`kthread_add` + `sched_bind` in the thread itself; never `sched_bind`
  the creating thread - the preload-boot hang in ../NOTES.md).  BLOCKING is
  a replaced-worker pool (the BLOCKING bullet below), not one-per-CPU.  Thread names are
  `kwq_net/N`, `kwq_bulk/N`, `kwq_blk/N` so `top` and `ps` attribute time
  to the class; per-client attribution comes from the accounting, not
  from thread names [L3].
- **How workers are shared.**  A queue never owns a thread and never
  creates one.  On each CPU there is exactly one NET worker and one BULK
  worker, and every queue of that class with items on that CPU is served
  by that one worker, in deficit round robin (S5): the worker keeps a ring
  of its CPU's active queues, gives each up to its quantum per round, and
  sleeps only when no queue of its class on its CPU has items.  A queue is
  therefore run by up to ncpu workers at once (one pass per CPU), which is
  why FIFO is promised per (queue, CPU) and handlers must be MP-safe
  across CPUs, and a worker is shared by every client on its CPU, which is
  why the quantum, the tick guard and the yield exist.  `kwq_create()`
  adds a queue to the rings; it adds no thread.  BULK workers additionally
  share across CPUs: an idle BULK worker may take a whole batch from a
  `KWQ_F_STEALABLE` queue's list on another CPU.  BLOCKING is the one
  class whose worker set is not fixed: each CPU has a small pool of
  replaceable workers shared by all BLOCKING queues on that CPU, plus, for
  queues created `KWQ_F_RESERVE`, workers that belong to that queue alone
  and serve nothing else.  There is no way for a NET or BULK client to
  obtain a dedicated thread; that restriction is the point (../POOLS.md
  L1).  Total thread count: 2 x ncpu fixed, plus the BLOCKING pools and
  reserved workers.
- **Priorities are fixed per class and not exposed.**  NET runs at
  `PI_NET` (`PRI_MIN_ITHD + 1`, the priority of NIC ithreads and netisr,
  deliberately equal so a worker running a peer's protocol input shares
  round-robin with them - ../NOTES.md priority audit).  BULK runs at
  `PI_SOFT` (`PRI_MIN_ITHD + 2`, with softclock and `qgroup_softirq`).
  BLOCKING runs at `PUSER` (56), the top of the timeshare range, where
  GELI's workers run today (S14): bulk work at a kernel priority would
  starve user processes, and blocking work does not need to beat them.
  No API takes a priority
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
- **BLOCKING is a replaced-worker pool** (cmwq's "concurrency-managed" lesson, ../POOLS.md
  S3): a small number of threads per CPU that may grow, bounded by a
  ceiling, when a worker blocks; idle threads exit after a timeout.  The
  blocked-worker signal is the open question named in ../POOLS.md S5; the
  design reserves two mechanisms: a `sched_switch` hook counting
  runnable BLOCKING workers per CPU (the exact analog of cmwq's
  `wq_worker_sleeping`), or, initially, a 1 Hz callout comparing
  `TD_IS_RUNNING` over the pool (the libdispatch monitor, in-kernel and
  without the /proc detour).  Additionally, a BLOCKING queue may reserve
  `nreserve` workers that serve only it (`KWQ_F_RESERVE`), for clients on
  the memory-reclaim or root-I/O path [L2].  GELI is such a client; until
  the reserved-worker mechanism exists, GELI keeps its own threads.
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

    struct kwq *kwq_create(const char *name, enum kwq_class cls,
                           uint32_t flags, const struct kwq_params *p,
                           kwq_handler_t *fn, void *ctx);
    void        kwq_activate(struct kwq *q);          /* make enqueue-able */
    int         kwq_enqueue(struct kwq *q, int cpu, struct kwq_item *it);
    int         kwq_enqueue_list(struct kwq *q, int cpu,
                                 struct kwq_item *head,
                                 struct kwq_item *tail, int n);
    void        kwq_requeue(struct kwq *q, struct kwq_item *head,
                            struct kwq_item *tail, int n);  /* handler only */
    uint64_t    kwq_budget_left(struct kwq *q);       /* handler only, ns */
    int         kwq_cpu_for_hash(uint32_t hash);       /* hash -> online CPU */
    #define KWQ_CPU_ANY      (-1)   /* BULK only: least loaded in caller's domain */
    int         kwq_scatter(struct kwq *q, struct kwq_item *list, int n,
                            void (*done)(void *), void *arg);  /* BULK */
    void        kwq_drain(struct kwq *q);             /* run or discard all */
    void        kwq_destroy(struct kwq *q);

    struct kwq_params {
        u_int   limit;      /* items per CPU queue; 0 = class default;
                               KWQ_LIMIT_NONE = unbounded (see S3) */
        u_int   weight;     /* 1..8, default 1 */
        u_int   nreserve;   /* BLOCKING + KWQ_F_RESERVE only */
        int     domain;     /* NUMA domain for internal memory, or -1 */
    };
    #define KWQ_F_INACTIVE   0x01   /* create inactive; kwq_activate() later */
    #define KWQ_F_STEALABLE  0x02   /* BULK: idle workers may take batches */
    #define KWQ_F_DISCARD    0x04   /* on drain, hand pending items to the
                                       handler with n < 0 to release, not run
                                       (a NET queue frees its mbufs) */
    #define KWQ_F_RESERVE    0x08   /* BLOCKING: nreserve reserved workers */
    #define KWQ_F_VNET       0x10   /* handler runs CURVNET_SET(q->kwq_vnet) */
    #define KWQ_F_SPIN       0x20   /* per-CPU lists use MTX_SPIN: producers may
                                       be interrupt filters or hold spin locks */

**Allocation happens exactly twice, both at configuration time.**
`kwq_create()` allocates the queue object and its per-CPU state (heads,
tails, counters, a `struct mtx` per CPU queue) with `M_WAITOK` in a
context that may sleep; it registers the name under
`kern.kwq.<class>.<name>`, allocates `counter(9)` cells, and returns an
inactive queue.  `kwq_activate()` activates it (a single store under
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
never be lost, ../NOTES.md).  The worker takes the same lock once per pass
to swap the list out (`mbufq_flush` shape), releases it, and runs the
handler with NO service lock held.  The handler may therefore call
`kwq_enqueue()` on any queue, including its own (self-enqueue lands in
the current CPU's list and is seen next pass), without lock-order
concerns; WITNESS sees the queue mutexes as leaves.  Locks the client
holds while calling `kwq_enqueue()`: any sleep mutex, rwlock or rmlock is
fine, because enqueue never sleeps and never calls out.  A caller holding
a SPIN mutex, or in an interrupt filter, may enqueue only to a
`KWQ_F_SPIN` queue, whose per-CPU lists are protected by spin mutexes
(taking a sleep mutex while holding a spin mutex is illegal, and WITNESS
says so).  Stealing takes the victim CPU's queue mutex, the only
cross-CPU lock in the design.

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
  other handlers.  From an interrupt filter, or while holding a spin
  mutex, only on a `KWQ_F_SPIN` queue (context table below).
- Returns 0, or ENOBUFS when the (queue, CPU) list holds `limit` items -
  the item is NOT queued and remains the caller's.  ENXIO after
  `kwq_drain()` began or before `kwq_activate()`.  The client decides
  drop-and-count versus backpressure; the service counts rejections per
  queue per CPU [L6].
- Ordering: items enqueued to the same (queue, CPU) are delivered FIFO in
  one or more batches; nothing is promised across CPUs or across queues.
  This is netisr's `NETISR_POLICY_SOURCE` made explicit by the client
  choosing the CPU.

**Why bound the queue at all when items are intrusive.**  The limit is
not a memory bound on the queue - an intrusive list costs nothing per
node - it is a bound on everything the queue's depth stands for: the
memory the queued items themselves pin (an mbuf chain per item, a bio
and its data buffer per item, drawn from pools shared with the whole
system, which is why netisr has `net.isr.maxqlimit` and interface send
queues have `IFQ_MAXLEN`); the queueing delay of the next item (depth
times per-item cost, unbounded if depth is); the time `kwq_drain()` must
take at teardown; and, most important, the only signal the producer ever
receives that the consumer is not keeping up.  Without the reject, an
overloaded system fails late and globally - the mbuf zone empties and
every interface suffers - instead of early and locally at the queue that
is behind, which is Mogul and Ramakrishnan's "discard early" argument
(../POOLS.md L6).  A queue may be unbounded only where something upstream
already limits it; that is exactly the notifier pattern (S14), where the
client's own list is bounded by GEOM's pacing and the buffer cache, and
kwq carries one item per CPU.

The argument generalises beyond packets, but the RESPONSE to a full
queue does not.  Queueing theory's distinction is the useful one: in an
*open* system arrivals come from outside and do not wait for service
(network receive, interrupts from a link partner), so when arrival rate
exceeds service rate the only stable response is to discard, and the
cheapest place to discard is before any work is invested.  In a *closed*
system the producers are inside the machine and each waits for its
previous request to complete before issuing the next (a process doing
disk I/O, `g_down` dispatching bios, the pager writing to swap), so the
population of outstanding requests is finite and bounded by resources the
kernel already limits (`nswbuf`, the buffer cache, `vfs.hirunningspace`,
one bio per requester); depth can never exceed that population, and a
reject would break a contract instead of enforcing one.  Device drivers'
deferred work is usually a third case, *idempotent*: one "TX completion",
"link change" or "refill" item per device queue, so depth is bounded by
construction (the Windows "queue it only if the list was empty" rule).

kwq therefore keeps the per-CPU limit for every queue, because it is
free, but its meaning differs by client type: for open clients (NET
carrying packets) it is flow control and rejects are expected under
overload; for closed and idempotent clients (GELI notifiers, driver
completions, wg's delivery notifiers) it is set to the maximum possible
population and a reject is a FAULT - a leak, a loop or a runaway producer
- which is why `rejected` is a counter and `kwq:::reject` a probe rather
than a silent drop.  The memory-pinning, queueing-delay and drain-time
arguments hold for all three; only the overload-signal argument is
specific to open arrivals, and there it is the whole point.

Classification of the clients discussed so far:

| client | items | producer type | limit means | on reject |
|---|---|---|---|---|
| if_pair, epair | mbufs from `if_output` | mixed: forwarded/UDP traffic is open; locally originated TCP is closed (the socket buffer and cwnd make the sender wait) but tolerates loss by retransmission | flow control; designed for the open component | drop, `OQDROPS` |
| netisr protocols | mbufs from NIC ithreads (open) and loopback (closed-ish) | open | flow control (today's `net.isr.maxqlimit`) | drop, `qdrops` |
| if_wg crypto (BULK) | packets to encrypt/decrypt | open on RX; TX inherits the socket's flow control but is dropped, not held, on overload | flow control | drop (wg already drops on its ring limits) |
| if_wg delivery (NET, CPU keyed by peer) | decrypted packets | open | flow control | drop |
| iflib RX/TX tasks | one task per hardware queue | idempotent | population = number of queues; a reject is a fault | assert/counter |
| GELI (BLOCKING, notifier) | one notifier per (provider, CPU); bios stay in GELI's list | closed | population = ncpu per provider; a reject is a fault | cannot happen |
| netisr `NETISR_POLICY_SOURCE` protocols with a single source | mbufs | closed-ish (one producer, self-paced) | flow control | drop |
| callout- or event-driven maintenance (link state, statistics, refill) | one item per device or object | idempotent | population = objects | fault |
| epoch callbacks, deferred frees, resource reclamation | one item per object awaiting release | **none of the three**: internally generated, not flow-controlled, and undroppable because dropping leaks the resource | must not reject: either kept out of kwq (the design's choice for epoch callbacks, S4) or carried by a notifier over a client-owned list | cannot happen |

So the answer to "can every conceivable client be classified as one of
the three" is no, for two reasons.  First, real queues are often mixed:
if_pair carries closed TCP and open UDP through one queue, and must be
designed for the open component while relying on the closed one to
recover from loss (TCP does; that is why the drop is acceptable there).
Second, there is a fourth kind - *reclamation* work, where the item IS a
resource to be released, arrivals are not paced by anyone waiting, and
dropping is the one thing that must not happen.  Linux carved this out as
`WQ_MEM_RECLAIM`; kwq's answer is that such work never travels as
droppable items: it stays out of kwq (epoch callbacks) or rides the
notifier pattern with a client-owned, memory-bounded list and reserved
workers, so that admission cannot fail and processing itself relieves
the pressure that created it.  The practical test when classifying a new
client is three questions in order: does the producer wait for
completion (closed)?  Is the item a signal that can be coalesced
(idempotent)?  Would dropping the item leak a resource (reclamation)?
Only if all three are "no" is the client open, and only then is the
limit flow control rather than a fault detector.

**Three follow-up questions about the limit, answered.**

*What if a client's population outgrows a limit it cannot know in
advance - say GELI notifiers with more providers than the limit?*  It
must not be allowed to arise, and the structure that prevents it is the
one the design already wants for other reasons: one kwq queue PER
PROVIDER, so each (queue, CPU) list holds at most one notifier and the
number of providers never meets any limit; per-provider queues are also
what gives per-provider accounting and a per-provider `kwq_drain()` on
detach.  The cost is O(ncpu) per queue (per-CPU state, roughly 32 KB per
queue on 128 CPUs), fine for hundreds of providers; a system with
thousands would instead use one GELI queue whose per-CPU lists hold one
notifier per provider and declare `KWQ_LIMIT_NONE` (next paragraph),
keeping per-provider counters on the GELI side.  The general rule: a
closed or idempotent client sizes its limit from a population it
controls; if it cannot bound the population, it must restructure so that
the queue depth is bounded by construction, or declare the queue
unbounded on purpose.

*What do epoch callbacks, deferred frees and resource release do instead
of rejecting?*  They never have a per-object kwq item in the first
place.  epoch(9) is the model: callbacks are recorded in per-CPU
`ck_epoch_record` lists owned by the epoch code, and one per-CPU task is
the signal that a batch is ready; the work list lives in the client,
depth in the deferral mechanism is structurally 1 per CPU, and the batch
is released in bulk.  That is the notifier pattern again.  If a client
genuinely needs one item per object, the item is intrusive in the object
itself, so an unbounded queue is memory-safe by construction: each queued
item pins exactly the memory it exists to free, depth is bounded by the
number of objects that exist, and draining relieves the pressure that
filled it.  For that case, and only that case, the queue should be
declared unbounded rather than given a guessed limit.

*Would an effectively infinite limit (`SIZE_MAX`) solve anything?*  As a
number, no; as a declared policy, one thing.  It cannot restore the
properties the limit protects - queueing delay, drain time and the
overload signal are unbounded exactly when depth is - so for open
producers it is a regression, and for closed or idempotent producers it
changes nothing in correct operation because the population already
bounds depth.  What it changes is failure visibility: with a generous
finite limit a leak, loop or runaway producer surfaces as a `rejected`
count within seconds; with an infinite limit it grows silently until
memory is exhausted somewhere else.  The design therefore offers
`KWQ_LIMIT_NONE` as an explicit, named declaration - "this queue's items
are the memory they pin and its population is bounded by that memory" -
usable only for closed and reclamation clients, and never as a way to
make rejects go away.  For an unbounded queue the health signal moves
from `rejected` to `cpu<N>.depth` and `maxdepth`, which the sysctl
reference exports for that reason.

**Choosing the CPU.**  The `cpu` argument is the client's statement of
its ordering domain: items that must stay in order relative to each
other must be given the same CPU, and items with no ordering relation
should be spread.  The service does no steering of its own; it offers
four ways to pick, in descending order of how often they are right:

1. **A flow hash**, `kwq_cpu_for_hash(m->m_pkthdr.flowid)` when
   `M_HASHTYPE_GET(m) != M_HASHTYPE_NONE`, else a hash the client
   computes (if_pair's `pair_hash_mbuf`, netisr's `m2flow`).  Keeps a flow
   on one CPU for its lifetime regardless of which thread produces it,
   which is the property `curcpu` lacks.  The helper maps the hash as
   `hash % (mp_maxid + 1)` and falls back to the caller's CPU if that id
   is offline: the same function TCP uses for `net.inet.tcp.per_cpu_timers`
   (`tcp_timer.c`), so a connection's kwq worker, its timers and, on RSS
   kernels, its NIC queue land on the same CPU, and every kwq client
   agrees on the mapping (../NOTES.md, the flowid-learning chain).
2. **The source's CPU**: the NIC receive queue's CPU (`rss_m2cpuid` on RSS
   kernels, netisr's `NETISR_POLICY_CPU`), or the CPU an ithread is bound
   to.  Correct when the producer is itself bound and ordering is per
   source.
3. **`curcpu`**: cheapest and cache-local, correct only for work with no
   ordering relation to other items (idempotent notifiers, per-object
   maintenance) or when the producer is bound.  For ordered flows produced
   by migrating threads it silently splits the flow across CPUs; the
   2026-08-25 steering analysis in ../NOTES.md is the cautionary case.
4. **`KWQ_CPU_ANY`** (BULK only): the least loaded CPU in the caller's
   cache domain, for unordered CPU-heavy work such as bulk crypto; the
   item may then also be stolen.  Invalid for NET, which must name its
   CPU.

An offline or nonexistent CPU number is mapped to the caller's CPU
rather than rejected.  Clients with their own affinity policy (GELI's
`kern.geom.eli.threads`, a per-peer table in wg) simply pass the CPU
they computed; the service treats it as an ordering key and nothing more.

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
  may process, enqueue elsewhere, requeue, free, or hand them on.  It must consume every
  item (the service keeps no reference).  `n < 0` on `KWQ_F_DISCARD`
  queues means "release these without processing" during drain.
- Runs on the worker of the CPU the items were enqueued to (NET, and BULK
  unless stolen), with the class's priority, inside the network epoch for
  NET (the `NET_TASK_INIT` convention), with `CURVNET_SET` to the queue's
  vnet if `KWQ_F_VNET`, and with `THREAD_NO_SLEEPING()` asserted for NET
  and BULK.
- Is given a budget it can query: `kwq_budget_left(q)` returns the
  nanoseconds remaining in the current quantum.  A handler processing a
  long list should stop when the budget is gone, hand the unprocessed
  remainder back with `kwq_requeue(q, head, tail, n)`, and return.
  `kwq_requeue()` PREPENDS the remainder to the current CPU's list of the
  same queue, ahead of items that arrived during the pass, so per-CPU FIFO
  order is preserved; a plain `kwq_enqueue_list()` would append behind
  them and reorder.  It is valid only from inside that queue's handler on
  that CPU, cannot fail (the items were admitted before the swap and are
  not counted against the limit again), and the service treats the
  remainder as the first work of the queue's next pass.  A handler
  that ignores the budget is preempted by nothing (it is at ithread
  priority) but is accounted, and its queue is flagged (S5); the service
  does not kill it.
- Must not call `kwq_drain()`, `kwq_destroy()`, or any function that may
  sleep (NET/BULK), and must not hold any service lock on return (it
  cannot: none is passed).

**kwq_drain / kwq_destroy**: sleepable contexts only; `kwq_destroy`
asserts drained; both are idempotent for the destroy-after-drain
sequence and panic on misuse under INVARIANTS.

**Which operations can fail.**  Failure is confined to two moments,
configuration and admission; everything on the teardown side is
infallible so that no client needs an error path there.

| operation | can fail? | how, and what the caller does |
|---|---|---|
| module load | yes | worker thread creation fails -> `kldload` fails; nothing is half-created |
| `kwq_create()` | only on caller error | `M_WAITOK` allocations cannot fail; returns `NULL` for an invalid class, flags, weight or limit, a name that is too long or already registered, or `KWQ_BLOCKING` before P7 (`EOPNOTSUPP` semantics); under INVARIANTS these are assertions, not return values |
| `kwq_activate()` | no | `void`; activating twice or after drain is a bug, asserted |
| `kwq_enqueue()` | yes, by design | `ENOBUFS`: the (queue, CPU) list for the chosen CPU already holds `limit` items (the limit is per CPU, not per queue, so one hot CPU rejects while others are empty - a steering symptom); `ENXIO`: `kwq_drain()` has begun, or the queue was created `KWQ_F_INACTIVE` and not yet activated (the latter is a bug, asserted under INVARIANTS); `EINVAL`: `KWQ_CPU_ANY` on a NET queue.  Never for contention, a busy worker, memory pressure or interrupt context.  The item is untouched and remains the caller's; the caller drops and counts, or applies backpressure upstream |
| `kwq_enqueue_list()` | yes, all-or-nothing | same codes; either every item is queued or none is |
| `kwq_scatter()` | yes, all-or-nothing | `EINVAL` for a non-BULK queue; `ENOBUFS` if the chunks do not all fit, in which case nothing is queued and `done` is never called; on success `done` is called exactly once |
| `kwq_budget_left()` | no | returns 0 when the quantum is exhausted; never an error |
| `kwq_requeue()` | no | `void`; handler-only, prepends already-admitted items, never counts against the limit |
| the handler | no | has no return value and owns every item it is given; it cannot refuse work, only requeue it |
| `kwq_drain()` | no | `void`; may sleep; calling it from a handler or non-sleepable context is a bug, asserted |
| `kwq_destroy()` | no | `void`; calling it on an undrained queue is a bug, asserted |
| module unload | yes | `EBUSY` while any queue exists; otherwise joins every worker and cannot fail afterwards |

The rule behind the table: allocation happens only in `kwq_create()`,
in a sleepable context with `M_WAITOK`, so the data path never
allocates and the teardown path never needs memory; admission is the one
place work is refused, and it refuses by returning the item, never by
consuming it.  Everything that "cannot fail" panics under INVARIANTS on
misuse rather than returning a code a caller would ignore.

**Which operations are allowed from which caller context.**

| caller context | allowed | not allowed |
|---|---|---|
| thread (syscall, kernel thread, sleepable) | everything | - |
| interrupt thread (ithread), SWI, callout, netisr, another kwq handler | `kwq_enqueue`, `kwq_enqueue_list`, `kwq_scatter`; inside a handler also `kwq_budget_left`, `kwq_requeue` | `kwq_create`, `kwq_drain`, `kwq_destroy` (they sleep); `kwq_activate` (configuration-time only, asserted) |
| interrupt filter (primary interrupt context) | `kwq_enqueue`, `kwq_enqueue_list`, `kwq_scatter` **on a `KWQ_F_SPIN` queue only** | the same calls on a sleep-mutex queue (WITNESS panics: blockable lock in filter context); everything else |
| inside `critical_enter()` or holding a spin mutex | as for a filter: `KWQ_F_SPIN` queues only | sleep-mutex queues |
| NMI, `SCHEDULER_STOPPED()`, KDB | nothing | everything (the mutexes degrade to no-ops after panic, so `kwq_drain` at shutdown needs no special case, but no new work may be submitted) |

Why the filter row works: a `KWQ_F_SPIN` queue's per-CPU lock is a spin
mutex, and the doorbell is `wakeup_one(9)`, which takes only spin locks
(sleepqueue chain and thread locks) and is what `taskqueue_create_fast()`
queues already call from filters via `taskqueue_thread_enqueue()`.  The
SDT probes and `counter(9)` updates on the enqueue path are safe in every
context.  Nothing on the enqueue path allocates, so there is no `M_NOWAIT`
failure to handle in any context; the only failure is a *reject*.

**Which operations are safe from inside a kwq handler.**  The handler runs
on a worker with no service lock held, so the service's own calls are
re-entrant from it; the class decides the rest.

| operation | NET / BULK handler | BLOCKING handler | notes |
|---|---|---|---|
| `kwq_enqueue`, `kwq_enqueue_list` to another queue | yes | yes | any class, any CPU; ordinary reject rules |
| `kwq_enqueue`, `kwq_enqueue_list` to the SAME queue | yes | yes | lands in the chosen CPU's list as NEW work (behind items already there); use for genuinely new items, not for leftovers |
| `kwq_requeue` (leftovers of this pass) | yes | yes | prepends to the current CPU's list of this queue; FIFO preserved; only for items received in this pass |
| `kwq_budget_left` | yes | yes (informational) | 0 means: requeue and return |
| `kwq_scatter` | yes | yes | to a BULK queue; completion runs later on some BULK worker |
| `kwq_create` | no | no | sleeps; configuration-time only |
| `kwq_activate` | no | no | configuration-time only, asserted |
| `kwq_drain`, `kwq_destroy` | no | no | sleep, and waiting for a queue from a handler deadlocks; asserted |
| waiting for another queue's items to finish | no | no | use the `kwq_scatter` completion callback |
| taking sleep mutexes, rwlocks, rmlocks | yes, briefly | yes | queue locks are leaves, so client locks may be held across `kwq_enqueue` |
| taking a spin mutex, `critical_enter`, `sched_pin` | yes, release before return | yes | while held, only `KWQ_F_SPIN` queues may be enqueued to |
| sleeping (`tsleep`, `cv_wait`, sx, lockmgr, `M_WAITOK`, `copyin`) | no, asserted | yes | |
| `M_NOWAIT` allocation | yes, handle `NULL` | yes | |
| entering the network epoch | already inside (NET); may nest | enter yourself | |
| freeing or handing on the items | yes | yes | the handler owns them |
| calling a peer stack's input path synchronously | only if it cannot loop back to a lock the enqueuer holds | n/a | the queue is the reentrancy break (2026-08-14 postmortem) |

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
  `kwq_enqueue()` to any queue.  A FILTER (primary interrupt context,
  spin locks only) may enqueue only to a queue created `KWQ_F_SPIN`,
  whose lists are spin-mutex protected and whose doorbell is
  `wakeup_one(9)` - the `taskqueue_create_fast()`/`gtaskqueue_create_fast()`
  precedent that iflib's filters use today (S9).  A filter feeding an
  ordinary queue schedules its ithread (`FILTER_SCHEDULE_THREAD`) and lets
  it enqueue.
- **cpuset(9), NUMA, hot-plug**: workers are created for `CPU_FOREACH`
  at load; FreeBSD has no CPU online/offline events (S11 Missing 3), so
  the per-CPU arrays are sized once and never revisited, and a CPU halted
  through `hlt_cpus_mask` still runs its bound worker when an IPI wakes
  it.  Per-queue internal memory is allocated from the `domain` given at
  create, default the CPU's own.
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
- **sysctl(9) and DTrace**: `kern.kwq.<class>.<name>.cpu<N>.{items,
  cycles, passes, overruns, rejected, maxlat_ns}` as `counter(9)`
  where possible; a `kern.kwq.<class>.quantum_us` per class (RWTUN,
  default 200 us NET, 1 ms BULK, 5 ms BLOCKING - the quantum shares one
  worker between queues, which ULE's time slice does not do); SDT probes
  `kwq:::enqueue(q, cpu, n)`, `kwq:::reject(q, cpu)`, `kwq:::pass-start(q,
  cpu, n)`, `kwq:::pass-end(q, cpu, ns)`, `kwq:::overrun(q, cpu, ns)`,
  `kwq:::yield(cpu, round_ns)`, `kwq:::steal(q, from, to, n)`.  This is the
  observability that every system in ../POOLS.md eventually rebuilt its
  deferral mechanism to obtain [L3].
- **Panic and debugger**: `show kwq` in DDB lists queues, per-CPU depths,
  states and the last handler run; the worker's `td_name` and the queue
  name appear in backtraces.

## 5. Accounting: fairness, latency, throughput

**Unit of account: CPU time, not items** [L5].  Every pass is bracketed
by `cpu_ticks()`/TSC reads; the difference is charged to the queue's
per-CPU `cycles` counter and to its DRR deficit.

**Fairness: deficit round robin per (class, CPU).**  A worker keeps the
CPU's active queues of its class in a ring.  Each round, each queue's
deficit is increased by `quantum * weight`; the worker swaps out the
queue's list and runs the handler while the deficit is positive, charging
consumed time; a queue whose list empties keeps its unused deficit up to
one quantum (so bursty clients are not penalised) and one whose handler
overran is parked with a negative deficit and skipped until it recovers.
A queue whose list goes from empty to non-empty is placed on the worker's
*new* list and served before the ring for its first quantum, then joins
the ring (fq_codel's new/old flow rule, S15), so a light queue is not
made to wait a whole round behind heavy ones.
With one active queue this degenerates to "drain until empty or
quantum", i.e. today's behaviour at zero extra cost (the libdispatch
observation that fairness machinery costs nothing until there is someone
to be fair to).  Between classes there is no scheduling at all: NET and
BULK are different threads at different fixed priorities, arbitrated by
ULE exactly as ithreads and softclock are today.

**Latency: the quantum and the round.**  A NET quantum of 200 us bounds
how long any one queue can hold the worker; a round over k active queues
bounds the queueing delay of an item on a continuously backlogged queue
at roughly k quanta; an item that wakes an empty queue waits about one
quantum plus the pass in progress (the new list, above).  At the end
of every round, or when a `hz` tick has elapsed since the worker last
yielded (if_pair's tick check, kept as the safety net for long items),
the worker performs the demote-yield: `kern_yield()` to the class's
fixed `yield_prio` (default `PUSER`, never `PRI_USER`: S11 Missing 1)
then restore the class priority, giving softclock, epoch callbacks,
BULK, BLOCKING and interactive userland a turn; CPU-bound userland gets
its share from the CPU-share cap (S11), not from the yield.  Without this a saturated NET worker starves its own
CPU's timers - the callout-starvation the 128-core measurements exposed
(../NOTES.md 2026-08-20).  Per-item queueing latency is sampled (enqueue
timestamp in the item is NOT stored - items are the client's - but the
per-pass `maxlat_ns` records the age of the oldest item at pass start,
using the timestamp of the first enqueue into an empty list, kept in the
per-CPU queue state).

**Jitter: no punting, time-based budgets, bound NET workers.**  A parked queue
resumes in the next round at the same priority; there is no
lower-priority fallback thread for overrun work (Linux's ksoftirqd cliff),
so a client's tail latency is bounded by its own overrun plus one round.
Budgets are time, so a 64 KB TSO chain and a 64-byte ACK are charged
what they cost.  NET is never stolen, so a flow's items never wait behind
a cross-CPU cache miss they did not cause.

**Throughput: batching everywhere it is free.**  The handler receives a
whole list (one lock hold per pass, not per item); the doorbell is
rung only on IDLE->WAKING (one wakeup per burst); NET enters the network
epoch once per pass; BULK stealing moves whole batches.  The quantum is
the throughput/latency dial and is a per-class sysctl; the defaults are
chosen so that a quantum is one to two orders of magnitude longer than
the per-item cost of the heaviest common item (a TSO chain through
`ip_input` is ~10 us, so 200 us is ~20 chains).

**What is exposed so a controller can be added later** [L8]: per-queue
overruns (a queue that keeps overrunning its quantum is either
mis-classed or should requeue its remainder), per-CPU round length,
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
| long computation | allowed but charged; check `kwq_budget_left()` and `kwq_requeue()` the remainder | same, larger quantum | allowed; the pool spawns a worker if this one blocks, not if it computes |
| ending a pass early | `kwq_requeue()` the remainder and return | same | same; `maybe_yield()` also fine |
| allocate | `M_NOWAIT` only; handle NULL | `M_NOWAIT` only | `M_WAITOK` permitted; reserve for reclaim clients |
| run in network epoch | always (entered by service) | no (enter it yourself) | no |
| touch user memory | never | never | with the usual `copyin` rules |
| re-enter the stack synchronously | never call a peer's input path inline from a NET handler if it can loop back to a lock the caller of `kwq_enqueue` holds - the queue IS the reentrancy break (the 2026-08-14 postmortem) | n/a | n/a |

Two rules deserve emphasis because every surveyed system got burned by
them: (a) "long computation" is legal but budgeted - the right pattern
is to process until `kwq_budget_left()` is exhausted, `kwq_requeue()` the
rest, return; the service records an overrun if a handler does not, and a
queue that overruns persistently is visible in `overruns` and DDB; (b)
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
                kwq_requeue(q, KWQ_MBUF_ITEM(next), KWQ_MBUF_ITEM(last), n_left);
                return;                          /* FIFO kept: prepended */
            }
        }
    }

    /* clone create, per pair side: sleepable context */
    sc->sc_q = kwq_create(if_name(ifp), KWQ_NET, KWQ_F_INACTIVE | KWQ_F_DISCARD,
                          NULL, pair_handler, sc);
    ... finish configuring ...
    kwq_activate(sc->sc_q);

    /* transmit path (any non-filter context): enqueue to the PEER's queue */
    if (kwq_enqueue(peer->sc_q, kwq_cpu_for_hash(flowid), KWQ_MBUF_ITEM(m)) != 0) {
        m_freem(m);
        if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
    }

    /* clone destroy: both sides down, NET_EPOCH_WAIT(), then per side */
    kwq_drain(sc->sc_q);          /* KWQ_F_DISCARD: pending mbufs freed */
    kwq_destroy(sc->sc_q);

The developer decides three things and nothing else: the class (may my
handler sleep, and is it packet-latency work or bulk compute), the CPU
per item (my ordering domain), and what to do on ENOBUFS.  They get, for
free, execution on a CPU-bound worker at the right priority, FIFO per CPU, a
lost-wakeup-proof doorbell, epoch entry, fairness against every other
client on the CPU, a bounded hold time, per-CPU counters and probes
under their name, and a teardown contract that composes with cloner
destroy and module unload.  They give up: choosing a priority, owning
threads, synchronous completion, and unbounded batches.

## 8. Migration and proof

Order, from ../POOLS.md L10 (infrastructure ships with its clients):

1. Land kwq with **if_pair and epair** converted together (identical
   shape: one NET queue per pair side, drained by the cloner's destroy
   before the interface detaches, as both drivers' per-side queues are
   today; epair is in-tree, and its RSS-only pool becomes unconditional
   per-CPU for free).  The t_17/t_19/t_20 suite on the Ampere is the
   acceptance test: no regression at P=32 peak, no callout starvation at
   P=128, per-client counters replacing the DTrace archaeology.
2. **netisr** onto the NET class (retires `net.isr.maxthreads=1`).
3. **if_wg** onto BULK with `KWQ_F_STEALABLE` and `kwq_scatter` for
   crypto, and one NET delivery queue whose CPU is keyed by peer, so each
   peer's packets stay in order without a queue per peer.
4. **BLOCKING** with worker replacement and `KWQ_F_RESERVE`; only
   then GELI.
5. **iflib** last, its `if_io_tqg` being the largest user and the most
   conservative reviewer base.

Non-goals for the first version: queue hierarchies or target queues,
priority inheritance, synchronous execution APIs, per-item priorities,
automatic quantum tuning, a lock-free backend.  Each was either the
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
   pass, and on 128 cores at P=128 the pair queue mutexes cost ~0.17
   CPU-equivalents (`pairq` block + spin, t_20).  The win is the hold
   window and the adaptive-spin/turnstile fallback, worth less than that.
5. **Batch enqueue.**  `kwq_enqueue_list()` is an O(1) `STAILQ_CONCAT`
   under the lock; a ring needs one CAS per element.

**Where a lock-free ring wins, and which one**

- `ck_ring` (MPSC enqueue, SPSC dequeue by the bound consumer) is
  already in the tree and in production use (`kern/subr_epoch.c` via
  `ck_epoch`; `ck_ring` in rack, gve, cxgb, mps/mpr, xen netback, qat).
  It is bounded by construction, so ENOBUFS is structural rather than a
  counter; it holds pointers, so mbufs need no `m_nextpkt` link; and it
  has no hold window, so many remote producers (NIC ithreads on other
  CPUs feeding one NET queue under a FLOW policy) cannot convoy on a
  lock line.  Its cost is `capacity x 8` bytes per (queue, CPU) and the
  loss of O(1) list splicing.
- **Filter context.**  Interrupt filters may take only spin locks, so a
  queue they feed needs either a lock-free ring or spin-mutex lists.  The
  design takes the spin mutex (`KWQ_F_SPIN`, below); a ring would be the
  alternative only if that spin mutex measured hot.
- `ck_fifo_mpmc` and `ck_hp_*` are the wrong tools here: they need
  per-node allocation or hazard-pointer reclamation, and our items are
  client-owned with no reclamation problem to solve.

**How a ring backend would fit, if ever.**  The backend is hidden behind
`kwq_enqueue()`, so a `ck_ring` MPSC backend could later be selected per
queue by a flag, with the IDLE/WAKING/RUNNING word as a separate atomic
driven by the producer index observed at enqueue (empty-before-enqueue
rings the doorbell) and `critical_enter()` around the publish.  It is
not in the first version (decision below) and would be justified only by
a `lockstat` profile of a specific queue.

**Is a lock-free queue ever required?**  Not by the API as specified.
Every context the design admits as a producer of an ordinary queue -
threads, ithreads, callouts, other handlers - may take a sleep mutex, and
a mutex is legal under `THREAD_NO_SLEEPING()` and inside an epoch section
(blocking on a mutex is not sleeping).  Producers that may not take a
sleep mutex get a spin mutex (`KWQ_F_SPIN`), not a lock-free queue:

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
for correctness, since `KWQ_F_SPIN` covers filters and spin-lock holders;
and preferable on performance grounds only where that spin lock would
sit on a hot cross-CPU path.

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
| `kwq:::pass-start` | worker swapped a list and is about to run the handler | `kwqinfo_t *`, `int cpu`, `int n`, `uint64_t oldest_age_ns` | per pass |
| `kwq:::pass-end` | handler returned | `kwqinfo_t *`, `int cpu`, `int n`, `uint64_t ns`, `int remaining_requeued` | per pass |
| `kwq:::overrun` | a pass overran its quantum | `kwqinfo_t *`, `int cpu`, `uint64_t over_ns` | per event |
| `kwq:::park` | queue parked with negative deficit | `kwqinfo_t *`, `int cpu`, `int64_t deficit_ns` | per event |
| `kwq:::round-end` | worker finished a DRR round | `int class`, `int cpu`, `int nqueues`, `uint64_t round_ns` | per round |
| `kwq:::yield` | the demote-yield taken | `int class`, `int cpu`, `int reason` (round / tick) | per round |
| `kwq:::idle` | worker found nothing and went to sleep | `int class`, `int cpu`, `uint64_t busy_ns` | per burst |
| `kwq:::steal` | BULK batch moved between CPUs | `kwqinfo_t *`, `int from`, `int to`, `int n` | per event |
| `kwq:::worker-block`, `worker-spawn`, `worker-exit` | BLOCKING pool management | `int cpu`, `int nworkers`, `int nrunnable` | per event |
| `kwq:::budget-hit` | a handler asked `kwq_budget_left()` and received 0 | `kwqinfo_t *`, `int cpu` | per event |

Everything an operator would otherwise compute is exposed as an
argument: the age of the oldest item at pass start (queueing delay
without per-item timestamps), whether an enqueue caused a wakeup (the
doorbell coalescing ratio), the deficit at park time, the remaining
items a cooperative handler requeued.

### 10.2 Translator (`/usr/lib/dtrace/kwq.d`)

    typedef struct kwqinfo {
        string   kwq_name;         /* "pair0a", "netisr/ip", "wg/crypto" */
        string   kwq_class;        /* "net", "bulk", "blocking" */
        int      kwq_weight;
        uint32_t kwq_limit;        /* per-CPU bound */
        uint32_t kwq_flags;
        uintptr_t kwq_addr;        /* for correlating with lockstat/fbt */
    } kwqinfo_t;

    translator kwqinfo_t < struct kwq *Q > {
        kwq_name   = stringof(Q->kwq_name);
        kwq_class  = Q->kwq_class == 0 ? "net" : Q->kwq_class == 1 ? "bulk" : "blocking";
        kwq_weight = Q->kwq_weight;
        kwq_limit  = Q->kwq_limit;
        kwq_flags  = Q->kwq_flags;
        kwq_addr   = (uintptr_t)Q;
    };

The name is a fixed-size array in `struct kwq` (not a pointer to client
memory) so `stringof` is always safe, including during drain/destroy.
A `dtrace_kwq(4)` manual page documents probes and types alongside the
existing `dtrace_io(4)`, `dtrace_sched(4)`, `dtrace_tcp(4)`.

### 10.3 Cost control

- The per-pass, per-round and per-event probes are cheap by
  construction and always compiled in.
- The per-item `kwq:::enqueue` probe sits on the hottest path in the
  system.  A disabled SDT probe costs a predicted-not-taken branch on
  `sdt_probes_enabled`; argument computation is guarded with
  `SDT_PROBES_ENABLED()` (as `tcp_input` does) so `depth_after`/`woke`
  are only evaluated when someone is tracing.  When enabled, at ~1M
  items/s per CPU the probe is expensive and the scripts below are
  written so the common questions never need it: pass-level data
  answers them.
- No probe takes a lock; all arguments are read from the per-CPU queue
  state the worker or enqueuer already holds or just published.

### 10.4 What the workers look like to other providers

- Threads are named `kwq_net/N`, `kwq_bulk/N`, `kwq_blk/N`, so
  `sched:::on-cpu`, `sched:::off-cpu` and the `profile` provider
  attribute CPU time per worker with `curthread->td_name` and per class
  with a prefix match; per-client attribution comes from `kwq:::pass-*`.
- The (queue, CPU) mutexes carry the queue name in their lock name
  (`"kwq pair"`), so the `lockstat` provider reports contention per
  queue - the tool that found the callout-wheel and mbuf-zone hot spots
  keeps working unchanged.
- `fbt::kwq_enqueue:entry` with `stack()` answers "who is feeding this
  queue"; `fbt` on the handler symbol answers "what does this client do
  per pass".

### 10.5 Always-on counters and DDB

DTrace is for investigation; steady-state health needs no probe enabled.
Every (queue, CPU) exports `counter(9)` cells under
`kern.kwq.<class>.<name>.cpu<N>.` (items, rejected, passes, cycles,
overruns, parks, requeued, steals_in, depth/maxdepth, maxlat_ns) plus
class-level `kern.kwq.<class>.cpu<N>.` (rounds, yields, tick_yields,
cap_sleeps, idle_ns, busy_ns, steals_out); the full list with types and
defaults is S10.7.  `sysctl kern.kwq` is the first thing to look at, and
`dtrace` the second.  `show kwq` in DDB prints every queue's
per-CPU depth, state (IDLE/WAKING/RUNNING/PARKED) and the last handler
run, for the post-mortem case.

### 10.6 One-liners the design is built to support

Queueing delay per client (age of the oldest item when its pass began):

    dtrace -n 'kwq:::pass-start { @[args[0]->kwq_name] = quantize(arg3 / 1000); }'

Which clients overrun their quantum, and from where:

    dtrace -n 'kwq:::overrun { @[args[0]->kwq_name, args[0]->kwq_class] = count(); }'
    dtrace -n 'kwq:::overrun /args[0]->kwq_name == "pair0a"/ { @[stack()] = count(); }'

Doorbell efficiency (wakeups per enqueue; near 0 = good batching):

    dtrace -n 'kwq:::enqueue { @e[args[0]->kwq_name] = count(); @w[args[0]->kwq_name] = sum(arg3); }'

Per-CPU imbalance of a client's work:

    dtrace -n 'kwq:::pass-end /args[0]->kwq_name == "netisr/ip"/ { @[arg1] = sum(arg3); }'

Who is dropping, and who feeds the queue that drops:

    dtrace -n 'kwq:::reject { @[args[0]->kwq_name, arg1, arg2] = count(); }'
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
unnamed mutexes; the probes exist so that the next operator does not.

### 10.7 sysctl reference

All nodes live under `kern.kwq`.  `<class>` is `net`, `bulk` or
`blocking`; `<name>` is the queue name given to `kwq_create()`; `<N>` a
CPU id.  Counters are `counter(9)` cells (64-bit, per-CPU internally,
read with `sysctl` as totals) unless marked otherwise.  RW knobs take
effect at the next round; RDTUN knobs are read at module load.

Service-wide

| node | type | access | default | meaning |
|---|---|---|---|---|
| `kern.kwq.version` | int | RD | 1 | KPI version, matches `MODULE_VERSION(kwq)` |
| `kern.kwq.ncpu` | int | RD | `mp_ncpus` | CPUs with workers |
| `kern.kwq.nqueues` | int | RD | - | queues currently created (active or not) |

Per class: `kern.kwq.<class>.`

| node | type | access | default | meaning |
|---|---|---|---|---|
| `yield_prio` | int | RW | `PUSER` (56) | priority the worker yields at after each round; `PRI_MAX_TIMESHARE` (223) lets every user thread run a full slice per round |
| `quantum_us` | int | RWTUN | net 200, bulk 1000, blocking 5000 | CPU time a queue of weight 1 may consume per round (BLOCKING too: the quantum shares a worker between queues, which ULE's slice does not) |
| `limit` | int | RWTUN | 4096 | default per-CPU item limit for queues created with `limit = 0`; applies to queues created afterwards |
| `cap_pct` | int | RW | 0 (off) | CPU-share cap: worker busy fraction above which it sleeps when user threads are runnable (S11 Missing 1) |
| `cap_sleep_us` | int | RW | 100 | length of that sleep |
| `cap_window_us` | int | RW | 10000 | window over which the busy fraction is measured |
| `priority` | int | RD | `PI_NET` / `PI_SOFT` / `PUSER` | scheduler priority of the class's workers |
| `nworkers` | int | RD | ncpu (blocking: current) | worker threads in the class |
| `max_workers` | int | RW | blocking only, 4 x ncpu | ceiling for worker replacement |
| `idle_timeout_s` | int | RW | blocking only, 30 | reap an idle replacement worker after this long |
| `cpu<N>.rounds` | counter | RD | | DRR rounds completed by this CPU's worker |
| `cpu<N>.yields` | counter | RD | | end-of-round yields taken |
| `cpu<N>.tick_yields` | counter | RD | | yields forced by the tick guard (a pass outlived a tick) |
| `cpu<N>.cap_sleeps` | counter | RD | | CPU-share cap sleeps taken |
| `cpu<N>.idle_ns` | counter | RD | | time the worker spent asleep with nothing queued |
| `cpu<N>.busy_ns` | counter | RD | | time spent in passes |
| `cpu<N>.steals_out` | counter | RD | | bulk only: batches taken from this CPU by others |

Per queue: `kern.kwq.<class>.<name>.`

| node | type | access | meaning |
|---|---|---|---|
| `weight` | int | RD | as created (1..8) |
| `limit` | int | RD | per-CPU item limit in effect |
| `flags` | uint | RD | `KWQ_F_*` as created |
| `state` | string | RD | `inactive`, `active`, `draining`, `drained` |
| `cpu<N>.depth` | int | RD (sampled) | items currently queued on this CPU |
| `cpu<N>.maxdepth` | int | RD, reset via `reset` | high-water mark of `depth`; the health signal for `KWQ_LIMIT_NONE` queues |
| `cpu<N>.items` | counter | RD | items accepted |
| `cpu<N>.rejected` | counter | RD | enqueues refused with `ENOBUFS` |
| `cpu<N>.passes` | counter | RD | handler invocations |
| `cpu<N>.cycles` | counter | RD | `cpu_ticks()` consumed by passes; divide by `kern.kwq.<class>.cpu<N>.busy_ns` for the queue's share of its worker |
| `cpu<N>.overruns` | counter | RD | passes that exceeded the remaining quantum |
| `cpu<N>.parks` | counter | RD | times the queue was skipped for a negative deficit |
| `cpu<N>.requeued` | counter | RD | items handed back with `kwq_requeue()` |
| `cpu<N>.steals_in` | counter | RD | bulk only: batches this CPU's worker took from others for this queue |
| `cpu<N>.maxlat_ns` | uint64 | RD, reset via `reset` | largest oldest-item age seen at pass start since last reset |
| `cpu<N>.reset` | int | WR | write 1 to zero `maxlat_ns` and `maxdepth` |

Only `kern.kwq.<class>.limit` and `quantum_us` are loader tunables
(RWTUN), for the preloaded case; everything else is runtime only.  The test module's knobs
live under `kern.kwq_test` and are described in PLAN.txt (P3); they are
not part of the kwq KPI.

### 10.8 Status of the documentation

S10.1-10.6 are the DTrace provider's specification (probes, arguments,
translator, cost, worker naming, one-liners) and S10.7 the sysctl
reference; both are design-level and will be the source for the
`dtrace_kwq(4)` and `kwq(9)` manual pages, which PLAN.txt schedules in P8
and which do not exist yet.  Until then this section is the reference,
and the code must match it or the section must change first.

## 11. What FreeBSD already provides, and what is missing

Checked against the `releng/15.0` tree (2026-09-23).  Almost everything
exists; three things do not, and one of them changes a claim this project
has been making about its own driver.

### Present and sufficient

| need | facility |
|---|---|
| CPU-bound per-CPU worker threads at a fixed priority | `kthread_add(9)` + `sched_bind()`/cpuset, as `taskqueue_start_threads_cpuset()` does |
| non-sleeping enforcement in NET/BULK handlers | `THREAD_NO_SLEEPING()` / `THREAD_SLEEPING_OK()` (`sys/proc.h`, the epoch(9) mechanism); `WITNESS_WARN(WARN_PANIC, ...)` for lock-leak checks |
| doorbell-safe queue lock, filter-context variant | sleep mutex; `MTX_SPIN` for filter producers (the `gtaskqueue_create_fast()` precedent) |
| priority lending under contention | turnstiles, adaptive mutexes |
| per-pass CPU time | `cpu_ticks()` (TSC-backed, what ULE charges `td_runtime` with) |
| tick guard | `ticks` |
| per-CPU state without atomics | DPCPU, `critical_enter()` |
| counters | `counter(9)`, `SYSCTL_ADD_COUNTER_U64`, dynamic sysctl nodes per name |
| network epoch per pass | `NET_EPOCH_ENTER()`; `NET_TASK_INIT` shows the convention |
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
demote-yield.  if_pair's `pair_yield()` does `kern_yield(PRI_USER)` then
restores `PI_NET`; `kern_yield()` resolves `PRI_USER` as

    if (prio == PRI_USER) prio = td->td_user_pri;
    sched_prio(td, prio); mi_switch(SW_VOL | SWT_RELINQUISH);

The first version of this section claimed `td_user_pri` of a kernel
thread is a constant `PUSER` inherited from thread0.  That is wrong, and
the correction (2026-09-25) changes the yield's character.  A worker
created with `kthread_add()` inherits its *scheduling class* from the
creating thread through `sched_fork_thread()`, and that class is
`PRI_TIMESHARE` (proc0 is created so, `init_main.c:515`; a `kldload`
thread is a user thread); `taskqueue_start_threads()` only calls
`sched_prio()`, never `sched_class()` (`subr_taskqueue.c:760`), so a
`PI_NET` taskqueue thread is a timeshare-class thread with an interrupt
priority.  ULE's `sched_clock()` therefore treats it like any timeshare
thread on every tick it runs: charges `ts_runtime`, updates the
interactivity score and calls `sched_priority()`, which rewrites
`td_user_pri` from that score (`sched_ule.c`, `sched_clock` and
`sched_priority`).  `td_priority` stays `PI_NET`; only the value
`PRI_USER` resolves to moves.  Two regimes follow:

- **A lightly loaded worker** sleeps more than it runs, scores
  interactive, and `td_user_pri` sits in 56..119: the yield lets the
  kernel range (40-55), softclock and the most interactive user threads
  run, and no CPU-bound ("batch", 120-223) user thread.
- **A saturated worker** has `ts_runtime` >> `ts_slptime` over ULE's ~5 s
  window (`SCHED_SLP_RUN_MAX`), scores non-interactive, and
  `td_user_pri` becomes `PRI_MIN_BATCH` + a CPU-usage offset (0..63,
  high for a saturated thread) + 20 for nice 0: roughly 180-200, among
  or behind the CPU-bound user threads on that CPU.  The yield then
  parks the worker in ULE's circular timeshare queue at that position,
  and it runs again only after the runnable threads ahead of it have
  each had a slice: `sched_slice` ~94 ms divided by the CPU's load, at
  least ~16 ms (`tdq_slice()`).  That is tens of milliseconds of NET
  latency per yield whenever a CPU-bound user thread shares the CPU -
  unbounded from the worker's point of view.

So `kern_yield(PRI_USER)` is neither the stingy behaviour the earlier
text described nor a bounded deference: it flips between "users get
nothing" and "users get whole slices" with the worker's own recent
history, and the flip happens exactly when load arrives.  The 128-core
runs did not show the second regime because the CPU-bound iperf3 threads
were rarely runnable on a worker's CPU at yield time; a uniprocessor or a
fully loaded box would.  FreeBSD has no primitive for what the design
wants - "run anyone lower than me for at most N microseconds" - and no
fixed target gives it either: yielding at `PUSER` runs only the kernel
range and the very top of the interactive range; yielding at
`PRI_MAX_TIMESHARE` runs every user thread for full slices.

Options, in order of preference:
(a) **Fixed target plus feedback cap, no scheduler change.**  The
    per-round yield demotes to a fixed, per-class, operator-visible
    priority (`kern.kwq.<class>.yield_prio`, default `PUSER`): it
    reliably lets timers, epoch callbacks, the kernel range and the most
    interactive threads run, each of which sleeps again soon, so its
    latency cost is small in practice though not bounded by
    construction.  CPU-bound user threads get their share from Mogul's
    "limit on CPU usage" feedback instead: each worker tracks its busy
    fraction over a short window and, above a class cap (say 90 %) with
    runnable timeshare threads on the CPU (`sched_runnable()`), sleeps
    for a fixed short interval with `pause_sbt(..., C_PREL(1))` at its
    normal priority.  Bounded by the interval, tunable, and the cost is
    one callout arm per cap event plus jitter equal to the interval.  An
    operator who prefers throughput for user processes over NET latency
    may set `yield_prio` to `PRI_MAX_TIMESHARE` and get the generous
    regime deterministically.  `PRI_USER` is never used.
(b) **A scheduler primitive**: `sched_relinquish_to(prio, sbintime_t
    max)` - demote to `prio`, switch, and have ULE restore the base
    priority and requeue the thread when `max` elapses or when the
    lower-priority queue empties.  ~50 lines in `sched_ule.c` plus 4BSD
    parity; the honest fix, and the one worth proposing on
    freebsd-arch because netisr, iflib and every CPU-bound taskqueue at
    `PI_NET` have the same unstated gap today - and, as shown above,
    the same undocumented regime flip.

Not an option: making the workers `PRI_ITHD` class with `sched_class()`
(exported, `sys/sched.h`).  It would freeze `td_user_pri` at whatever the
loading thread had, and ULE demotes and preempts an ithread-class thread
that runs a full slice (`sched_clock`, `ithread_preemptions`), adding
scheduler-driven jitter inside the ithread range that helps no user
thread.  kwq workers stay timeshare-class threads at interrupt priority,
as taskqueue's are.

### Missing 2: a "worker is about to sleep" hook for the BLOCKING class

cmwq's concurrency management (kwq: worker replacement; ../POOLS.md S3) relies on the scheduler
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

`if_pair(4)`'s description of `net.link.pair.batch` said the yield keeps
"timers and user processes" running; the 2026-09-23 correction said it
keeps timers and interactive processes running and never CPU-bound
ones.  Both are wrong in the same way: the priority the worker yields
at is whatever ULE has computed for it from its recent history.  A
lightly loaded worker yields only to kernel threads and interactive
processes; a saturated worker yields into the batch range and lets
CPU-bound processes run full time slices, tens of milliseconds, at the
cost of its own latency.  The manual page now says so (2026-09-25) and
../NOTES.md records the finding.  The driver's code is unchanged; P4
replaces its yield with kwq's fixed-target one.  The same wording applies
to every `PI_NET` taskqueue pool in the tree that calls
`kern_yield(PRI_USER)`.

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
                    2 x quantum x active queues; yield: softclock keeps
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
`SI_SUB_TASKQ` ordering relative to in-tree clients that start earlier;
irrelevant while all clients are modules that `MODULE_DEPEND` on it.
(4) Unload must be refused while queues exist (`EBUSY` from
`MOD_UNLOAD`), and all workers must be joined before the module text is
unmapped - taskqueue's `taskqueue_free` teardown is the template.

**Running on an unpatched GENERIC: the trade-offs in one place.**
Everything above is the module's behaviour on a stock kernel; the
right-hand column is what the two scheduler patches (S11 Missing 1 and
2) would change.

| concern | module on stock GENERIC | with the scheduler patches |
|---|---|---|
| deference to user threads (S11 Missing 1) | fixed-priority yield (`yield_prio`, default `PUSER`): timers, epoch callbacks, kernel range and the most interactive threads run; CPU-bound users get a share only through the `pause_sbt` cap, which adds jitter equal to `cap_sleep_us` and sleeps for the whole interval even if the user thread finishes early | `sched_relinquish_to(prio, max)`: every lower thread may run, for at most `max`; no cap, no jitter beyond `max` |
| a BLOCKING worker blocks (S11 Missing 2) | the 1 Hz monitor notices within up to a second; queues on that CPU stall that long unless the queue has reserved workers (`KWQ_F_RESERVE`) | `sched_sleep` hook starts a replacement within microseconds; fewer reserved workers needed |
| a NET or BULK worker hogs the CPU | cooperative only: quantum via `kwq_budget_left()` and the tick guard; ULE's slice-end for a timeshare-class kernel thread sets an AST the thread never services | unchanged (the patches do not add preemption; a hog is a client bug either way) |
| CPU choice for `KWQ_CPU_ANY` | kwq's own per-CPU depth and `sched_runnable()`; ULE's per-CPU load (`tdq_load`) is not exported, `sched_load()` is global | unchanged unless a per-CPU load accessor is added, which neither patch does |
| CPU hot-plug (S11 Missing 3) | none exists; arrays sized once at load | unchanged |
| in-tree clients (netisr, epoch callbacks, taskqueue shim) | cannot use a module: they start before it and cannot `MODULE_DEPEND` | unchanged; these need the in-tree move, not the scheduler patches |
| KBI | built per kernel like any module; `THREAD_NO_SLEEPING()` touches `struct thread`, so a kernel rebuild means a module rebuild (normal) | unchanged |
| testing and iteration | load/unload cycles, A/B under a running client, provider tested as a unit (above) | lost once in-tree; the reason to stay a module until the design settles |

The module form costs nothing in per-item overhead (all calls are
already indirect or out of line) and gives up exactly two things a stock
kernel cannot provide: bounded deference and prompt blocked-worker
detection.  Both fallbacks are explicit, measured by counters
(`cap_sleeps`, `tick_yields`, the monitor's replacements) and remain the
module's behaviour on kernels without the patches.

Graduation path: once if_pair, epair and wg run on `kwq.ko` and the two
scheduler patches are in review, the module moves to `kern/subr_kwq.c`
with the netisr conversion as its first in-tree client.

## 14. GELI: reliable I/O on a best-effort queue

Disk I/O may not be dropped, so GELI is the client that tests whether a
service built around "enqueue may reject" can carry it at all.  It can,
because of three facts about GEOM and GELI as they are today
(`geom/geom_io.c`, `geom/eli/g_eli.c`, `releng/15.0`):

1. **GEOM already has a retry channel for refused work.**  A provider that
   cannot take a request delivers it with `ENOMEM`; `g_io_deliver()` then
   does not complete it upward but resets it and calls `g_io_request()`
   again, setting `pace = 1`, and the `g_down` thread pauses 1 ms before
   its next dispatch ("There has been at least one memory allocation
   failure since the last I/O completed. Pause 1ms to give the system a
   chance to free up memory").  GELI uses exactly this in `g_eli_start()`
   when `g_clone_bio()` fails and in its crypto paths when
   `g_eli_alloc_data()` fails.  A kwq `ENOBUFS` maps onto it one for one:
   refused means retried with pacing, never lost.
2. **GELI already never allocates with `M_WAITOK` on the request path.**
   Its buffer pool comment: "Swap-related requests are special: they can
   only use the UMA pool, they use M_USE_RESERVE to let them dip farther
   into system resources, and they always use M_NOWAIT to prevent swap
   operations from deadlocking."  kwq adds no allocation to that path
   (S3), so the swap-deadlock argument GELI makes today still holds.
3. **GELI's workers do not sleep while holding work**, except to wait out
   an administrative suspend (`geli suspend`) or for the queue to become
   non-empty.  The crypto itself is synchronous CPU work through
   `crypto_dispatch()`; hardware crypto completes through the crypto
   framework's own threads.  What its workers DO need is the priority
   they run at: `sched_prio(curthread, PUSER)`, i.e. timeshare, not
   interrupt-class.  Bulk encryption at `PI_SOFT` would starve user
   processes in a way GELI has never done; that rules out BULK and makes
   GELI a BLOCKING-class client for its priority, not for its sleeping.

**The notifier pattern.**  A client that must never lose a request does
not put the requests themselves into kwq.  It keeps its own unbounded
request list (GELI's `sc_queue` bioq, as now) and uses kwq only as the
execution context: one preallocated item per (provider, CPU) - the
*notifier* - enqueued when the client's list goes from empty to
non-empty, exactly the idempotent-requeue rule the Windows work-item
documentation prescribes and the doorbell already implements one level
down.  With at most one notifier per CPU ever in flight, the (queue, CPU)
list never reaches its limit and `kwq_enqueue()` can never return
`ENOBUFS`; the handler drains the client's list under the client's lock
with `kwq_budget_left()` as its batch bound, requeues its notifier when
work remains, and admission control stays where GEOM already does it.
kwq becomes "give me a bound worker on this CPU with this priority and
this accounting", which is all GELI ever wanted from its per-provider
kprocs.  The pattern is the recommended shape for every reliable client;
the mbuf-carrying shape (items are the work) is for clients whose items
are droppable by contract.

**Reserved workers are the memory-pressure guarantee.**  GELI creates its
worker processes at attach and never needs more, so under memory pressure
it never waits for a thread that cannot be created.  A BLOCKING queue
with `KWQ_F_RESERVE`, `nreserve = 1` per CPU (or `kern.geom.eli.threads`
semantics), gets workers allocated at `kwq_create()` time with `M_WAITOK`
in the attach path and owned by that queue for its lifetime; worker
replacement never applies to reserved workers, and they never serve other
queues.  That is Linux's `WQ_MEM_RECLAIM` rescuer stated positively, and
it is why S8 gates GELI on the reserved-worker mechanism existing.

**Suspend, detach, ordering.**  `geli suspend` is handled by the client:
its handler sees `G_ELI_FLAG_SUSPEND` and simply returns without draining
its list (no requeue), so the notifier is re-armed by the next enqueue
after resume; no kwq state is involved.  Detach drains the client's own
list (`g_eli_cancel()` semantics), then `kwq_drain()` runs any pending
notifiers to completion, then `kwq_destroy()`.  Ordering: GEOM does not
promise completion order across requests, and GELI's multiple workers
already reorder; the client chooses the CPU (round-robin, or the
originating CPU for locality) and stealing across the reserved workers of
one queue is a client option, not a requirement.

**Net effect.**  For GELI the design changes nothing about reliability -
refused work is retried by GEOM as today, memory is never allocated on
the request path as today, threads exist from attach as today - and adds
what GELI's private pool lacks: per-provider per-CPU accounting under
`kern.kwq.blocking.geli-<provider>`, DTrace probes, the tick guard and
quantum so that a saturating encryption stream cannot monopolise a CPU
against other BLOCKING clients, and one fewer thread pool in the system.
The prerequisites are the reserved-worker mechanism and the BLOCKING
class itself (PLAN.txt P7); until then GELI keeps its own workers, and
nothing in kwq's first version pretends otherwise.

## 15. Scheduler choice: DRR (1995) and what came after

The fairness core of S5 is deficit round robin (Shreedhar & Varghese,
SIGCOMM 1995 / IEEE-ACM ToN 1996).  Its weaknesses were known within a
year: a newly active queue waits a whole round, so its latency bound is
`active queues x quantum`, and its worst-case fairness index (Bennett &
Zhang, INFOCOM 1996) grows with both.  What happened since falls into
four lines, and each has something to say about kwq.

**1. Timestamp schedulers made cheap.**  Virtual-time (weighted fair
queueing) schedulers - PGPS/WFQ (Demers, Keshav, Shenker 1989; Parekh &
Gallager 1993), SCFQ (Golestani 1994), start-time FQ (Goyal, Vin, Cheng,
SIGCOMM 1996), WF2Q and WF2Q+ (Bennett & Zhang 1996/1997) - give a
latency bound of about one quantum independent of the number of queues,
at O(log N) per operation.  The cost objection was removed by QFQ
(Checconi, Rizzo, Valente, IEEE-ACM ToN 2013) and QFQ+ (Valente,
Computer Communications 2013): O(1) with WF2Q+ guarantees within a
constant.  FreeBSD already ships the whole family in dummynet -
`sys/netpfil/ipfw/dn_sched_wf2q.c`, `dn_sched_qfq.c` (copyright 2010
Checconi, Rizzo, Valente), next to the plain round robin
`dn_sched_rr.c` - and Linux has `sch_qfq`.  Their unit is bytes; the
algorithms do not care.

**2. Round robin refined but kept.**  Smoothed RR (Guo, SIGCOMM 2001),
Aliquem (Lenzini, Mingozzi, Stea 2002/2004) and Stratified RR
(Ramabhadran & Pasquale, SIGCOMM 2003) attack the same latency bound
while staying O(1).  The refinement that won in practice is simpler:
fq_codel (Nichols, Jacobson, Høiland-Jørgensen et al., RFC 8290, 2018)
and CAKE (Høiland-Jørgensen et al., 2018) are DRR with two lists.  A
queue that becomes non-empty joins a "new" list that is served before the
"old" list for its first quantum, then moves to the old list; a light
("sparse") queue therefore sees roughly one pass of latency instead of a
whole round, and a heavy one is unaffected.  Dummynet's
`dn_sched_fq_codel.c` implements exactly this (`newflows`/`oldflows`,
`deficit`, `quantum`).  DRR is not obsolete; it is the base of the
schedulers deployed most widely today.

**3. Proportional share for CPUs, the same theory.**  Lottery and stride
scheduling (Waldspurger & Weihl 1994/1995), EEVDF (Stoica &
Abdel-Wahab 1995), BVT (Duda & Cheriton, SOSP 1999), VTRR (Nieh, Vaill,
Zhong, USENIX 2001) and GR3 (Caprita, Nieh, Chan, USENIX 2005; O(1)
proportional share on multiprocessors) are the CPU-side literature.
Linux's CFS (2007) is weighted fair queueing over virtual runtime; in
Linux 6.6 (2023) its placement heuristics were replaced by EEVDF -
1995 theory adopted three decades later - completed in 6.12 (2024),
which also added the BPF-programmable `sched_ext`.  Two lessons: EEVDF
separates a task's *slice* (how much it asks for at once, the latency
knob) from its *weight* (its share), two knobs kwq currently folds into
one class quantum plus a per-queue weight; and `sched_ext` is an
admission that no single policy suits every workload, answered there by
pluggability and in kwq by classes, sysctls and DTrace instead.

**4. Budgets in service units, with feedback.**  BFQ (Valente &
Checconi 2010; Linux 4.12, 2017) is WF2Q+ over budgets measured in
sectors, not requests, and sizes each queue's budget from what it
actually used - the closest published analog of "DRR in CPU time".
blk-iocost (Heo, Linux 5.4, 2019) adds a device cost model, virtual time
with donation from idle to busy cgroups, latency-target feedback, and
*debt* for I/O that may not be refused (memory reclaim) - the same
problem S14 solves with reserved workers.  The microsecond-scale
dataplane work (ZygOS SOSP 2017, Shinjuku and Shenango NSDI 2019,
Caladan OSDI 2020, Perséphone SOSP 2021, Concord SOSP 2023) shows that
for tail latency the preemption granularity and head-of-line blocking by
long items matter more than weights, and that work stealing across cores
beats static partitioning when service times vary: kwq's per-CPU FIFO
with `KWQ_F_STEALABLE` for BULK and the quantum as the head-of-line bound
is the same shape, with cooperative `kwq_budget_left()` where those
systems use interrupts.

**Assessment for kwq.**  The DRR weakness scales with the number of
queues active on one CPU in one class, which is a handful, times a
200 us quantum: a worst case of one to two milliseconds, the same order
as ULE's own tick.  A timestamp scheduler would tighten that at the cost
of virtual-time bookkeeping around every pass, for a benefit the batch
model largely erases (a handler runs a whole batch regardless of which
scheduler chose the queue).  The fq_codel refinement, however, is nearly
free and removes the case that hurts most, a light queue (a GELI
notifier, one quiet pair among busy ones) waiting behind heavy ones, so
it is adopted: a (queue, CPU) list going empty->non-empty enters the
worker's *new* list and is served ahead of the ring for its first
quantum, then joins the ring.  With it the latency bound for a newly
active queue is about one quantum plus the pass in progress, independent
of the active count; the `active queues x quantum` bound remains for
continuously backlogged queues, which is what fairness means.

Upgrade path, if `maxlat_ns` measurements contradict this: replace the
ring by virtual time in `cpu_ticks()` (WF2Q+/EEVDF; with fewer than
sixteen queues per CPU a sorted insertion is O(1) in practice), keeping
the API, counters and probes unchanged - `dn_sched_qfq.c`,
`dn_sched_wf2q.c` and Linux `kernel/sched/fair.c` are the reference
implementations.  A per-queue quantum (EEVDF's slice) would be the next
knob after that, not before: it is a second parameter for clients to get
wrong.

## 16. Pseudo interfaces: enqueue per packet, batch at the consumer

The question for epair and if_pair is whether the driver should enqueue
each packet as it arrives at `if_transmit`/`if_output`, or collect
packets and enqueue them in batches.  The answer is per packet on the
producer side and whole-list on the consumer side, which is what both
drivers already do (`if_epair.c` `epair_menq()` per packet,
`epair_tx_start_deferred()` flushing the whole `mbufq`; `if_pair.c`
`pair_output()` and `pair_task_deferred()` likewise) and what kwq's
drain loop gives for free.

**The producer has no batch to offer.**  FreeBSD's transmit path hands
the driver one packet at a time: `tcp_output()` calls `ip_output()` per
segment (under the inpcb lock, ../NOTES.md), `ip_tryforward()` forwards
one packet, `if_transmit`/`if_output` take one mbuf.  There is no
`xmit_more` equivalent.  The only multi-segment unit a producer ever
holds is a TSO chain, and that is one mbuf with one packet header, i.e.
one kwq item; the pair delivers it whole (`PAIR_TSO_*`), so it is one
item on the far side too.  The one producer that does hold a list is the
software-TSO split for a peer that cannot take large frames
(`tcp_tso_chop()`, ../CHOPPER.txt): that is `kwq_enqueue_list()`'s case,
an O(1) splice of an already-linked list, and nothing else in these
drivers qualifies.

**Producer-side staging would be a loss.**  Holding packets in a
per-producer buffer until N have accumulated or a timer fires buys one
mutex acquisition per packet (the queue mutex is uncontended in the fast
path and costs tens of nanoseconds against microseconds of `ip_input`)
and pays for it three times: a flush timer, i.e. latency for the last
packet of every burst and a new tunable; a staging area per (producing
CPU, peer side, target CPU), since the target CPU is chosen per packet
by `kwq_cpu_for_hash(flowid)` and a batch would have to be split by it
anyway; and a second copy of the state machine kwq already runs.  The
doorbell makes the buffer unnecessary: the first enqueue into an idle
list wakes the worker (IDLE->WAKING) and every enqueue after that, until
the worker has swapped the list out, is a lock-append with no wakeup.
Under load the batch a pass receives is exactly what arrived during the
previous pass - Mogul's and LRP's "batch what has accumulated", the
NAPI/`softnet` shape Linux's veth uses (`__netif_rx()` per packet,
`netif_receive_skb_list()` and GRO at poll time) - with no added latency
when the system is idle.  The `wakeups`/`items` ratio in S10.6 measures
how well this works, and inline delivery to bypass the queue is not an
option (the panic recorded in ../NOTES.md).

**The consumer batches, in three ways.**  The handler receives the whole
list of a (queue, CPU) pair, so per pass rather than per packet it does
what if_pair's worker does today: `if_ref()`, `CURVNET_SET()`, network
epoch entry (kwq does the epoch), and the wakeup handshake.  Beyond that:

1. *Delivery stays per packet.*  `netisr_dispatch(NETISR_IP, m)` takes
   one packet (hybrid dispatch runs `ip_input()` directly in the
   worker); `ether_input()` accepts an `m_nextpkt` list but only loops
   over it (`if_ethersubr.c:826-834`), so epair may hand it the whole
   list for one call but gains no per-packet work.  There is no list
   input API for IP in the stack, and kwq does not pretend to add one.
2. *Software LRO is the batching that pays.*  Feeding each mbuf of the
   pass to `tcp_lro_queue_mbuf()` and calling `tcp_lro_flush_all()` at
   the end of the pass merges same-connection segments of the batch into
   one `tcp_input()` call each (sorted mode makes interleaving
   irrelevant, ../NOTES.md LRO block), dividing per-packet lock traffic
   by the aggregation factor.  It only works because the consumer sees
   batches, it is refused on a forwarding vnet (`V_ipforwarding`,
   documented in ../if_pair.4 TUNING), and a whole TSO frame passes
   through it unmerged.  Optional, measured in P4b; not a kwq feature
   but the reason the batch structure is worth keeping.
3. *The budget is checked per packet.*  `kwq_budget_left()` is a
   `cpu_ticks()` read, cheaper than any packet; checking every 16
   packets would let a run of TSO chains at ~10 us each overrun a 200 us
   quantum by most of a quantum.  When the budget is gone with items
   left: flush LRO (mbufs handed to LRO are no longer kwq items), then
   `kwq_requeue()` the untouched remainder, which prepends and keeps
   FIFO.

**No batch-size knob.**  `net.link.pair.batch` counted packets, a unit
that stretched with MTU (64 x ~20 us at mtu 65535 overran the tick, the
comment above `pair_task_deferred()`); the class quantum in CPU time
replaces it and PLAN P4 deprecates the sysctl.  The batch is whatever
accumulated, bounded by time, and the driver decides nothing about its
size.

## 17. Memory layout: cache lines, sleep channels, NUMA

Yes, and the rules are few.  The hot state is per (queue, CPU), and the
only intended cross-CPU traffic is a producer on CPU A appending to CPU
B's list: one cache line moving A->B->A per burst is the price of the
design and is fine.  What must not happen is two CPUs' private state
sharing a line (false sharing), or two workers' wait channels hashing to
the same sleepqueue chain.  if_pair today gets both wrong in a small way:
`struct pair_queue` (~116 bytes: mutex, `mbufq`, state, `task`, pointer)
is allocated as a plain `malloc()` array with pointer alignment
(`if_pair.c:958`; `malloc(9)` zones align to `UMA_ALIGN_PTR`,
`kern_malloc.c:1292`), so neighbouring queues, drained by different
CPUs, straddle lines, and the `task` used as wait channel sits wherever
the array puts it.  Neither was measured; t_20's `sleepq_chain`
contention is the visible half.

**Constants.**  `CACHE_LINE_SIZE` is 64 on amd64 (`CACHE_LINE_SHIFT` 6,
`amd64/include/param.h:86`) and 128 on arm64 (shift 7,
`arm64/include/param.h:82`).  Always use the macro, never a number; the
Ampere is the 128-byte case.  Sleep channels hash into 256 chains by
`SC_HASH(wc) = ((wc >> 8) ^ wc) & 255` (`subr_sleepqueue.c:98-103`):
only the low two bytes of the address matter, so two wait channels a
multiple of 65536 bytes apart always share a chain, while a stride of
64, 128 or 256 bytes maps up to 256 consecutive elements to distinct
chains (a stride of 512 already collides after 128).

**Rule 1: one line per (queue, CPU) for the shared state, lock
included.**  The producer takes the lock and writes head, tail, depth,
state and the empty-since timestamp; putting them in the same line as
the mutex means one line transfer per enqueue, not five.  Do not split
the lock from the data it protects into different lines (a common
"optimisation" that doubles the traffic), and do not put anything the
consumer writes without the lock into that line.

    struct kwq_cpu {
        /* line 0: shared, producer and consumer, under kc_mtx */
        struct mtx      kc_mtx;
        STAILQ_HEAD(, kwq_item) kc_list;
        u_int           kc_depth;
        u_int           kc_state;       /* IDLE/WAKING/RUNNING/PARKED */
        uint64_t        kc_empty_since; /* first enqueue into empty list */
        uint64_t        kc_rejected;    /* producer writes, holds the lock */
        /* line 1..: consumer-private, written only by the owning CPU */
        int64_t         kc_deficit    __aligned(CACHE_LINE_SIZE);
        TAILQ_ENTRY(kwq_cpu) kc_ring;
        uint64_t        kc_items, kc_passes, kc_cycles, kc_overruns,
                        kc_parks, kc_requeued, kc_steals_in,
                        kc_maxdepth, kc_maxlat_ns;
    } __aligned(CACHE_LINE_SIZE);

`__aligned(CACHE_LINE_SIZE)` on the struct makes `sizeof` a multiple of
the line so an array of them never shares lines; `struct mtx_padalign`
(`sys/mutex.h`) is the tree's idiom for a lone lock and is not needed
when the lock already heads a padded struct.

**Rule 2: the per-queue counters are plain fields, not `counter(9)`.**
`counter(9)` exists to let any CPU increment without atomics; here every
counter except `rejected` is written only by the owning CPU's worker,
which already holds the line, so a plain `uint64_t` in the private line
is cheaper (no `critical_enter`, no per-CPU offset) and never shared.
`rejected` is written by the producer under the lock, in line 0.  The
sysctl handler sums across CPUs on read, which touches the lines once per
read and is irrelevant.  S10.7's "counter(9) cells" means this: per-CPU
cells with `counter(9)` semantics, not the `counter(9)` allocator.  The
per-(class, CPU) worker statistics (`rounds`, `yields`, `idle_ns`, ...)
are likewise plain fields in the worker struct.

**Rule 3: the read-mostly queue header gets its own line.**  `struct
kwq` (name, class, flags, handler, ctx, limit, weight, vnet, the pointer
to the per-CPU array) is read on every enqueue and every pass and written
only at create, activate and drain.  Keep those fields together in one
line and put anything written at runtime (drain state, active count,
sysctl context) in a separate line at the end, so a drain on one CPU does
not invalidate the header in every other CPU's cache while they are
still enqueueing.

**Rule 4: per-CPU allocation, not one array.**  S4 promises per-CPU
memory from the CPU's own domain.  A single contiguous array of `struct
kwq_cpu` lives in one domain; on a two-socket machine half the CPUs then
take their lock across the interconnect on every pass.  Allocate one
`struct kwq_cpu` per CPU with `malloc_domainset_aligned(sizeof, 
CACHE_LINE_SIZE, M_KWQ, DOMAINSET_PREF(pcpu_find(cpu)->pc_domain),
M_WAITOK | M_ZERO)` (`kern_malloc.c:823`; plain `malloc()` gives only
pointer alignment, so the aligned variant is required, not optional),
and keep the `mp_maxid + 1` pointers in a read-mostly array inside
`struct kwq`.  The cost is one dependent load per enqueue from a line
that never changes after create; the alternative, `DPCPU`, is for
module-static data sized at load and cannot back a per-queue allocation.
The per-(class, CPU) worker structs are static in number and are
`DPCPU_DEFINE_STATIC` candidates, or the same per-CPU aligned allocation
at module load; either keeps each worker's ring, deficits and wait
channel on its own CPU's domain.

**Rule 5: wait channels spread over sleepqueue chains.**  Each worker
sleeps on the address of a field in its own `struct kwq_worker`.  With
per-CPU allocation those addresses are effectively random in the low
two bytes, which is what the hash wants but does not guarantee; with
one static array the stride is `sizeof(struct kwq_worker)`, and a stride
of 64, 128 or 256 bytes keeps up to 256 workers of a class on distinct
chains (above).  Either way, verify at load under INVARIANTS that no two
workers of a class hash to the same chain and `printf` if they do; on a
128-CPU machine a collision costs a shared chain lock on every doorbell
of both workers.  Producers never sleep on these
channels; the doorbell is `wakeup_one()` on the target worker's channel,
so the chain lock a producer takes is the target's, one per wakeup, and
the coalescing in S3 (one wakeup per burst) is what keeps it cold.

**Rule 6: items are the client's; do not touch them more than once.**
Enqueue writes the link field in the item's own line (for an mbuf, the
`m_stailqpkt` union inside `pkthdr`, a line the producer just wrote
anyway); the consumer reads it once when walking the list.  kwq adds no
per-item metadata (no enqueue timestamp, S5), precisely so it never
touches a second line per item.

**What to measure in P0/P1.**  `pmcstat` on the Ampere with the
`L2D_CACHE_WB`/`REMOTE_ACCESS`-class events, and on amd64 the
`MEM_LOAD_L3_MISS_RETIRED.REMOTE_HITM` family, before and after Rule 4;
`lockstat` on the `kwq` mutexes (their hold time is a proxy for line
transfer); `sysctl debug.sleepq` does not exist, so chain collisions are
checked by the load-time assertion of Rule 5.  Record the numbers in
../NOTES.md next to the t_20 contention data.

## 13. Revision log

- 2026-09-23: first draft (S0-S8); S9 locks vs Concurrency Kit, locked-only
  first version; S10 DTrace; S11 FreeBSD facility audit and the three
  gaps, including the `kern_yield(PRI_USER)` finding; S12 module-first.
- 2026-09-24: terminology normalised to GLOSSARY.md (pass, bound, name,
  work class, overrun, reserved worker, worker replacement; API
  identifiers `KWQ_F_RESERVE`, `nreserve`, `kwq_name`, probes `pass-*`,
  `overrun`); *client* defined; S3 gained the failure-mode table, the
  caller-context table and the handler-safety table; `kwq_requeue()`
  introduced and `kwq_yield()` removed after finding that handing
  leftovers back through `kwq_enqueue_list()` would reorder items behind
  ones that arrived during the pass; `kwq_budget_left()` added to the API
  listing.
- 2026-09-24 (later): S10.7 sysctl reference and S10.8 documentation status
  added; the man pages remain P8 deliverables.
- 2026-09-24 (later): S14 GELI analysis - GEOM's ENOMEM retry-with-pacing
  as the reject channel, the notifier pattern for reliable clients,
  reserved workers as the memory-pressure guarantee, PUSER priority as
  the reason GELI is BLOCKING-class.
- 2026-09-24 (later): S3 gained the rationale for bounding intrusive
  queues, the open / closed / idempotent / reclamation producer taxonomy
  with the client classification table, `KWQ_LIMIT_NONE` and the
  `maxdepth` counter, and the CPU-selection guidance with
  `kwq_cpu_for_hash()` (the TCP per-CPU-timer mapping) and `KWQ_CPU_ANY`
  in the API listing; S1 gained the explicit worker-sharing paragraph.
  Examples switched from `% mp_ncpus` to `kwq_cpu_for_hash()`.


- 2026-09-25: contradiction pass over the whole text.  Decided and
  aligned: `KWQ_F_SPIN` queues are in the first version and are the only
  legal target from interrupt filters or under a spin mutex (S2, S3, S4,
  S8 and S0 had said filters could not enqueue at all, and S2 had said
  holding a spin mutex was fine); BLOCKING gets a quantum (5 ms) because
  ULE does not time-slice kernel-priority threads (S4/S10.7 had said
  "none" while S14 promised one); the hot-plug sentence in S4 now matches
  S11/S12 (no events exist); `KWQ_F_DISCARD` applies to every class (a NET
  queue frees mbufs at drain); if_pair and epair use one queue per pair
  side so the cloner's destroy can drain it (S4's VNET contract), instead
  of one shared queue; wg delivery is one queue keyed by peer, not a
  queue per peer; S10.5 lists the same counters as S10.7 and the reset
  node is `reset`; `limit`/`quantum_us` are RWTUN; S9's ring paragraph is
  now explicitly "if ever"; S12's latency criterion matches P1's factor
  of two; BLOCKING runs at `PUSER`, not `PRI_MIN_KERN` (S1 and S10.7 had
  the kernel priority while S14 argued GELI must stay at `PUSER` so bulk
  encryption does not starve user processes - the same argument applies
  to every BLOCKING client); the revision log moved to the end.
- 2026-09-25: S15 added on whether the 1995 DRR paper is still the state
  of the art.  Adopted fq_codel's new/old list rule (a newly non-empty
  queue is served first for one quantum); S5 and PLAN P1 updated.
- 2026-09-25: S16 added: pseudo interfaces enqueue per packet and batch
  at the consumer (delivery per packet, software LRO per pass, budget
  check per packet); PLAN P4 gained the LRO sub-step P4b.
- 2026-09-25: S11 Missing 1 corrected.  A kthread_add() worker is a
  timeshare-class thread, so ULE recomputes its td_user_pri every tick
  from its own interactivity score; kern_yield(PRI_USER) therefore lets
  CPU-bound user threads run full slices when the worker is saturated
  and nothing when it is lightly loaded.  Decision: kwq yields to a
  fixed per-class priority (kern.kwq.<class>.yield_prio, default
  PUSER), never PRI_USER; S5, S10.7, GLOSSARY, PLAN P1 and if_pair.4
  updated; S12 gained the consolidated table of what running as a
  module on an unpatched GENERIC costs.
- 2026-09-25: S17 added: memory layout rules (padded per-(queue, CPU)
  struct with lock and shared fields in one line, plain per-CPU counter
  fields, read-mostly header, per-CPU domain allocation with
  malloc_domainset_aligned, sleepqueue chain spreading, no per-item
  touch); PLAN P0 gained the layout bullets.
